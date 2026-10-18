// Tree scan: walks a local tree folder by folder with openat (any path length),
// hashes files, and keeps the results in the files table as a hash cache.
// A file whose identity (device, inode, size, times) is unchanged since it was last
// hashed keeps its cached hash and is not read again.
#include "TreeSyncCore.h"

#include <dirent.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <cstring>

String SortKey(const String& rel)
{
	String k = rel;
	k.Replace("/", "\x01");
	return k;
}

bool SkipName(const String& name)
{
	return name == "." || name == ".." || name.EndsWith(".treesync-part");
}

bool GlobMatch(const String& patterns, const String& name)
{
	for(const String& p : Split(patterns, ';')) {
		String t = TrimBoth(p);
		if(t.GetCount() && fnmatch(t, name, 0) == 0)
			return true;
	}
	return false;
}

bool AnyPattern(const String& patterns)
{
	for(const String& p : Split(patterns, ';'))
		if(TrimBoth(p).GetCount())
			return true;
	return false;
}

static int64 Ns(const timespec& t)
{
	return (int64)t.tv_sec * 1000000000 + t.tv_nsec;
}

// Filesystems whose inode numbers are not stable between mounts or scans.
bool UnstableInodes(int fd)
{
	struct statfs sf;
	if(fstatfs(fd, &sf) != 0)
		return true;
	switch((unsigned)sf.f_type) {
	case 0x4d44:       // FAT
	case 0x2011BAB0:   // exFAT
	case 0x65735546:   // FUSE
	case 0x6969:       // NFS
	case 0xFF534D42:   // CIFS
	case 0xFE534D42:   // SMB2
		return true;
	}
	return false;
}

// HashCache -----------------------------------------------------------------

HashCache::HashCache(Db& db, const String& root, const SyncFlags& flags, bool unstable)
:	db(db), root(root), flags(flags), unstable(unstable),
	find(db, "SELECT dev, ino, size, mtime_ns, ctime_ns, sha256 FROM files WHERE root = ? AND rel = ?"),
	store(db, "INSERT OR REPLACE INTO files VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)")
{
	stamp = (int64)time(nullptr) * 1000 + Random(1000);
	db.Exec("BEGIN");
}

bool HashCache::Lookup(const Entry& e, String& hash)
{
	find.Reset();
	find.Text(1, root).Text(2, e.rel);
	if(!find.Step())
		return false;
	// The cache compares a file with its own earlier times on the same side, so the
	// time must match exactly: a tolerance could miss a same-size rewrite. Except
	// a DST shift, which FAT disks really show (--dst-tolerance).
	int64 dt = abs(find.Int(3) - e.mtime_ns);
	bool time_ok = dt == 0 || (flags.dst_tolerance && abs(dt - 3600000000000LL) <= ToleranceNs(flags));
	bool ok = find.Int(2) == e.size && time_ok &&
	          (unstable || (find.Int(0) == e.dev && find.Int(1) == e.ino && find.Int(4) == e.ctime_ns));
	if(ok)
		hash = find.Text(5);
	find.Reset();
	return ok && hash.GetCount() == 64;
}

void HashCache::Store(const Entry& e)
{
	store.Text(1, root).Text(2, e.rel).Text(3, String(e.type, 1))
	     .Int(4, e.dev).Int(5, e.ino).Int(6, e.nlink).Int(7, e.size)
	     .Int(8, e.mtime_ns).Int(9, e.ctime_ns).Int(10, e.mode).Text(11, e.link)
	     .Text(12, e.hash).Int(13, stamp);
	store.Run();
	// Commit in batches so a stopped scan keeps the hashes it already has.
	if(++pending >= 500) {
		db.Exec("COMMIT; BEGIN");
		pending = 0;
	}
}

void HashCache::Finish()
{
	Stmt(db, "DELETE FROM files WHERE root = ? AND seen <> ?").Text(1, root).Int(2, stamp).Run();
	db.Exec("COMMIT");
}

// Local scanner --------------------------------------------------------------

namespace {

struct Scanner {
	String            root;
	const SyncFlags&  flags;
	SyncReport&       report;
	Vector<Entry>&    out;
	ScanStats&        stats;
	HashCache&        cache;
	NameFilter        filter;
	Index<Tuple<int64, int64>> open_dirs;   // (dev, ino) of folders being walked: loop guard
	std::chrono::steady_clock::time_point last_status = std::chrono::steady_clock::now();

	Scanner(const String& root, const SyncFlags& flags, SyncReport& report,
	        Vector<Entry>& out, ScanStats& stats, HashCache& cache)
	:	root(root), flags(flags), report(report), out(out), stats(stats), cache(cache), filter(flags) {}

	void Status(const String& rel)
	{
		auto now = std::chrono::steady_clock::now();
		if(now - last_status < std::chrono::milliseconds(250))
			return;
		last_status = now;
		String s = Format("Scanning %s: %d", root, stats.entries);
		if(stats.estimate)
			s << " of ~" << stats.estimate;
		s << " entries, " << FormatBytes(stats.hashed_bytes) << " hashed  " << rel;
		report.WhenStatus(s);
	}

	void Warn(const String& msg)
	{
		stats.warnings++;
		report.WhenLine("warning: " + msg);
	}

	// Scans the open folder fd; returns its folder hash, or Null if stopped. inside:
	// in an included folder (NameFilter).
	String Dir(int fd, const String& rel, bool inside)
	{
		Vector<String> names;
		if(DIR *d = fdopendir(dup(fd))) {
			while(dirent *e = readdir(d))
				if(!SkipName(e->d_name))
					names.Add(e->d_name);
			closedir(d);
		}
		else
			Warn("cannot read folder " + AppendFileName(root, rel) + ": " + strerror(errno));
		Sort(names);

		String listing;
		for(const String& name : names) {
			if(report.Stopping())
				return Null;
			struct stat st;
			if(fstatat(fd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
				Warn("cannot stat " + AppendFileName(AppendFileName(root, rel), name) + ": " + strerror(errno));
				continue;
			}
			Entry e;
			e.rel = rel.IsEmpty() ? name : rel + "/" + name;
			// --follow-links: a symlink stands for what it points to (file or folder).
			bool follow = false;
			if(S_ISLNK(st.st_mode) && flags.follow_links) {
				struct stat target;
				if(fstatat(fd, name, &target, 0) == 0 && (S_ISDIR(target.st_mode) || S_ISREG(target.st_mode))) {
					st = target;
					follow = true;
				}
			}
			e.dev = st.st_dev;
			e.ino = st.st_ino;
			e.nlink = st.st_nlink;
			e.size = st.st_size;
			e.mtime_ns = Ns(st.st_mtim);
			e.ctime_ns = Ns(st.st_ctim);
			e.mode = st.st_mode & 07777;
			stats.entries++;
			Status(e.rel);

			if(S_ISDIR(st.st_mode)) {
				if(filter.SkipDir(name))
					continue;
				Tuple<int64, int64> id = { e.dev, e.ino };
				if(open_dirs.Find(id) >= 0) {
					Warn("symlink loop skipped: " + AppendFileName(root, e.rel));
					continue;
				}
				int child = openat(fd, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | (follow ? 0 : O_NOFOLLOW));
				if(child < 0) {
					Warn("cannot open folder " + AppendFileName(root, e.rel) + ": " + strerror(errno));
					continue;
				}
				open_dirs.Add(id);
				e.type = 'd';
				e.size = 0;
				int at = out.GetCount();
				out.Add(e);
				bool in = filter.Inside(inside, name);
				String h = Dir(child, e.rel, in);
				open_dirs.Drop();
				close(child);
				if(IsNull(h))
					return Null;
				if(out.GetCount() == at + 1 && !filter.KeepEmptyDir(in)) {   // nothing included in it
					out.Drop();
					continue;
				}
				out[at].hash = h;
				cache.Store(out[at]);
				ListingLine(listing, 'd', h, name);
			}
			else if(S_ISLNK(st.st_mode)) {
				if(!filter.KeepFile(inside, name))
					continue;
				char buf[PATH_MAX + 1];
				ssize_t n = readlinkat(fd, name, buf, PATH_MAX);
				e.type = 'l';
				e.link = String(buf, (int)max<ssize_t>(n, 0));
				e.hash = SHA256String("symlink\n" + e.link);
				out.Add(e);
				cache.Store(e);
				ListingLine(listing, 'l', e.hash, name);
			}
			else if(S_ISREG(st.st_mode)) {
				if(!filter.KeepFile(inside, name))
					continue;
				e.type = 'f';
				if(cache.Lookup(e, e.hash))
					stats.reused++;
				else {
					String error;
					int64 bytes = 0;
					if(!HashAt(fd, name, e.hash, error, report.stop, &bytes, follow)) {
						if(report.Stopping())
							return Null;
						Warn(error);
						continue;
					}
					stats.hashed++;
					stats.hashed_bytes += bytes;
				}
				out.Add(e);
				cache.Store(e);
				ListingLine(listing, 'f', e.hash, name);
			}
			else
				Warn("skipped special file (device, pipe or socket): " + AppendFileName(root, e.rel));
		}
		return SHA256String(listing);
	}
};

}

bool ScanTree(Db& db, const String& root, const SyncFlags& flags, SyncReport& report,
              Vector<Entry>& out, String& root_hash, ScanStats& stats, String& error)
{
	out.Clear();
	stats = ScanStats();
	int fd = OpenDir(root);
	if(fd < 0) {
		error = "cannot open " + root + ": " + strerror(errno);
		return false;
	}

	// statvfs counts inodes in use on the whole filesystem: a good estimate of the
	// entry count when the tree is the mount itself (its parent is on another device).
	struct stat self, parent;
	struct statvfs vfs;
	if(fstat(fd, &self) == 0 && fstatat(fd, "..", &parent, 0) == 0 &&
	   (self.st_dev != parent.st_dev || self.st_ino == parent.st_ino) &&
	   fstatvfs(fd, &vfs) == 0 && vfs.f_files > 0)
		stats.estimate = vfs.f_files - vfs.f_ffree;

	HashCache cache(db, root, flags, UnstableInodes(fd));
	Scanner s(root, flags, report, out, stats, cache);
	root_hash = s.Dir(fd, Null, s.filter.RootInside());
	close(fd);
	if(IsNull(root_hash)) {
		db.Exec("COMMIT");
		error = "stopped";
		return false;
	}
	cache.Finish();
	Sort(out, [](const Entry& a, const Entry& b) { return SortKey(a.rel) < SortKey(b.rel); });
	return true;
}
