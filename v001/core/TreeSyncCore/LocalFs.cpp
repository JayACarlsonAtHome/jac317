// Local side of a sync. Every operation opens the parent folder (OpenDir walks
// any path length) and then works relative to it with the *at calls.
#include "TreeSyncCore.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

static String Errno(const String& what)
{
	return what + ": " + strerror(errno);
}

static int64 LocalNs(const timespec& t)
{
	return (int64)t.tv_sec * 1000000000 + t.tv_nsec;
}

int LocalFs::ParentFd(const String& rel) const
{
	String folder = GetFileFolder(rel);
	return OpenDir(folder.IsEmpty() ? root : AppendFileName(root, folder));
}

// Runs fn(parent fd, name) with the parent folder of rel open.
template <class F>
static auto AtParent(const LocalFs& fs, const String& rel, String& error, F fn) -> decltype(fn(0, String()))
{
	int dp = fs.ParentFd(rel);
	if(dp < 0) {
		error = Errno("cannot open the folder of " + rel);
		return decltype(fn(0, String()))();
	}
	auto r = fn(dp, GetFileName(rel));
	close(dp);
	return r;
}

bool LocalFs::RootExists()
{
	std::error_code ec;
	return std::filesystem::is_directory(ToFs(root), ec);
}

bool LocalFs::MakeRoot(String& error)
{
	std::error_code ec;
	std::filesystem::create_directories(ToFs(root), ec);
	if(ec)
		error = "cannot create " + root + ": " + String(ec.message());
	return !ec;
}

bool LocalFs::Scan(Db& db, const SyncFlags& flags, SyncReport& report, Vector<Entry>& out,
                   String& root_hash, ScanStats& stats, String& error)
{
	return ScanTree(db, root, flags, report, out, root_hash, stats, error);
}

bool LocalFs::Stat(const String& rel, bool follow, Entry& e, String& error)
{
	return AtParent(*this, rel, error, [&](int dp, const String& name) {
		struct stat st;
		if(fstatat(dp, name, &st, follow ? 0 : AT_SYMLINK_NOFOLLOW) != 0) {
			error = Errno("cannot stat " + rel);
			return false;
		}
		e.rel = rel;
		e.type = S_ISDIR(st.st_mode) ? 'd' : S_ISLNK(st.st_mode) ? 'l' : 'f';
		e.dev = st.st_dev;
		e.ino = st.st_ino;
		e.nlink = st.st_nlink;
		e.size = st.st_size;
		e.mtime_ns = LocalNs(st.st_mtim);
		e.ctime_ns = LocalNs(st.st_ctim);
		e.mode = st.st_mode & 07777;
		e.uid = st.st_uid;
		e.gid = st.st_gid;
		if(S_ISLNK(st.st_mode)) {
			char buf[PATH_MAX + 1];
			ssize_t n = readlinkat(dp, name, buf, PATH_MAX);
			e.link = String(buf, (int)max<ssize_t>(n, 0));
		}
		return true;
	});
}

// The inodes in use on the filesystem: an estimate of the entry count when the tree
// is the whole mount (its parent is on another device), else 0 (unknown).
int64 LocalFs::EntryEstimate()
{
	int fd = OpenDir(root);
	if(fd < 0)
		return 0;
	struct stat self, parent;
	struct statvfs vfs;
	int64 n = 0;
	if(fstat(fd, &self) == 0 && fstatat(fd, "..", &parent, 0) == 0 &&
	   (self.st_dev != parent.st_dev || self.st_ino == parent.st_ino) &&
	   fstatvfs(fd, &vfs) == 0 && vfs.f_files > 0)
		n = vfs.f_files - vfs.f_ffree;
	close(fd);
	return n;
}

bool LocalFs::StableIds()
{
	if(stable < 0) {
		int fd = OpenDir(root);
		stable = fd >= 0 && !UnstableInodes(fd);
		if(fd >= 0)
			close(fd);
	}
	return stable;
}

bool LocalFs::Hash(const String& rel, bool follow, String& hash, String& error,
                   const std::atomic<bool> *stop)
{
	return AtParent(*this, rel, error, [&](int dp, const String& name) {
		return HashAt(dp, name, hash, error, stop, nullptr, follow);
	});
}

namespace {

struct LocalFile : TreeFile {
	int fd = -1;

	bool ReadAt(int64 off, void *buf, int64 len, String& error) override
	{
		for(int64 got = 0; got < len;) {
			ssize_t n = pread(fd, (byte *)buf + got, (size_t)(len - got), off + got);
			if(n < 0 && errno == EINTR)
				continue;
			if(n <= 0) {
				error = n < 0 ? Errno("read") : String("unexpected end of file");
				return false;
			}
			got += n;
		}
		return true;
	}

	bool WriteAt(int64 off, const void *buf, int64 len, String& error) override
	{
		for(int64 put = 0; put < len;) {
			ssize_t n = pwrite(fd, (const byte *)buf + put, (size_t)(len - put), off + put);
			if(n < 0 && errno == EINTR)
				continue;
			if(n <= 0) {
				error = Errno("write");
				return false;
			}
			put += n;
		}
		return true;
	}

	bool Truncate(int64 size, String& error) override
	{
		if(ftruncate(fd, size) == 0)
			return true;
		error = Errno("truncate");
		return false;
	}

	void Flush() override   { fdatasync(fd); }

	bool SetAttrs(const FileAttrs& a, String& error) override
	{
		timespec times[2];
		times[0].tv_sec = times[1].tv_sec = a.mtime_ns / 1000000000;
		times[0].tv_nsec = times[1].tv_nsec = a.mtime_ns % 1000000000;
		if(fchmod(fd, a.mode & 07777) != 0 || futimens(fd, times) != 0) {
			error = Errno("cannot set times or permissions");
			return false;
		}
		if(a.uid >= 0 && IsRoot() && fchown(fd, (uid_t)a.uid, (gid_t)a.gid) != 0) {
			error = Errno("cannot set owner");
			return false;
		}
		return true;
	}

	int  Fd() const override   { return fd; }
	~LocalFile()            { if(fd >= 0) close(fd); }
};

}

// A file's extended attributes, names sorted, values as read.
static Vector<Tuple<String, String>> ReadXattrs(int fd)
{
	Vector<Tuple<String, String>> out;
	ssize_t n = fd < 0 ? -1 : flistxattr(fd, nullptr, 0);
	if(n <= 0)
		return out;
	Buffer<char> names((size_t)n);
	n = flistxattr(fd, names, (size_t)n);
	for(const char *name = names; n > 0 && name < names + n; name += strlen(name) + 1) {
		ssize_t v = fgetxattr(fd, name, nullptr, 0);
		if(v < 0)
			continue;
		Buffer<char> value((size_t)max<ssize_t>(v, 1));
		v = fgetxattr(fd, name, value, (size_t)v);
		if(v >= 0)
			out.Add(MakeTuple(String(name), String(value, (int)v)));
	}
	Sort(out, [](const auto& a, const auto& b) { return a.a < b.a; });
	return out;
}

static String Digest(const Vector<Tuple<String, String>>& xattrs)
{
	String all;
	for(const auto& x : xattrs)
		if(x.a != "security.selinux")
			all << x.a << '\0' << (int64)x.b.GetCount() << '\0' << x.b;
	return all.IsEmpty() ? String() : SHA256String(all);
}

String XattrDigest(TreeFile& f)
{
	return Digest(ReadXattrs(f.Fd()));
}

String CopyXattrs(TreeFile& from, TreeFile& to)
{
	Vector<Tuple<String, String>> xattrs = ReadXattrs(from.Fd());
	if(to.Fd() >= 0)
		for(const auto& x : xattrs)
			fsetxattr(to.Fd(), x.a, ~x.b, (size_t)x.b.GetCount(), 0);
	return Digest(xattrs);
}

bool FdEntry(int fd, Entry& e)
{
	struct stat st;
	if(fd < 0 || fstat(fd, &st) != 0)
		return false;
	e.type = S_ISDIR(st.st_mode) ? 'd' : S_ISLNK(st.st_mode) ? 'l' : 'f';
	e.dev = st.st_dev;
	e.ino = st.st_ino;
	e.nlink = st.st_nlink;
	e.size = st.st_size;
	e.mtime_ns = LocalNs(st.st_mtim);
	e.ctime_ns = LocalNs(st.st_ctim);
	e.mode = st.st_mode & 07777;
	e.uid = st.st_uid;
	e.gid = st.st_gid;
	return true;
}

One<TreeFile> LocalFs::Open(const String& rel, bool write, String& error)
{
	One<TreeFile> f;
	AtParent(*this, rel, error, [&](int dp, const String& name) {
		int fd = openat(dp, name, write ? O_RDWR | O_CREAT | O_CLOEXEC : O_RDONLY | O_CLOEXEC, 0600);
		if(fd < 0) {
			error = Errno("cannot open " + rel);
			return false;
		}
		LocalFile& lf = f.Create<LocalFile>();
		lf.fd = fd;
		return true;
	});
	return f;
}

bool LocalFs::SetAttrs(const String& rel, const FileAttrs& a, String& error)
{
	return AtParent(*this, rel, error, [&](int dp, const String& name) {
		timespec times[2];
		times[0].tv_sec = times[1].tv_sec = a.mtime_ns / 1000000000;
		times[0].tv_nsec = times[1].tv_nsec = a.mtime_ns % 1000000000;
		if(fchmodat(dp, name, a.mode & 07777, 0) != 0 || utimensat(dp, name, times, AT_SYMLINK_NOFOLLOW) != 0) {
			error = Errno("cannot set times or permissions of " + rel);
			return false;
		}
		if(a.uid >= 0 && IsRoot() && fchownat(dp, name, (uid_t)a.uid, (gid_t)a.gid, AT_SYMLINK_NOFOLLOW) != 0) {
			error = Errno("cannot set owner of " + rel);
			return false;
		}
		return true;
	});
}

bool LocalFs::Mkdir(const String& rel, int mode, String& error)
{
	return AtParent(*this, rel, error, [&](int dp, const String& name) {
		struct stat st;
		if(mkdirat(dp, name, (mode & 07777) | 0700) == 0 ||
		   (errno == EEXIST && fstatat(dp, name, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(st.st_mode)))
			return true;
		error = Errno("cannot create folder " + rel);
		return false;
	});
}

int LocalFs::Remove(const String& rel, bool dir, String& error)
{
	int r = AtParent(*this, rel, error, [&](int dp, const String& name) {
		if(unlinkat(dp, name, dir ? AT_REMOVEDIR : 0) == 0 || errno == ENOENT)
			return 1;
		if(dir && (errno == ENOTEMPTY || errno == EEXIST))
			return 2;
		error = Errno("cannot delete " + rel);
		return 0;
	});
	return r == 1 ? 0 : r == 2 ? 1 : -1;
}

bool LocalFs::Symlink(const String& rel, const String& target, String& error)
{
	return AtParent(*this, rel, error, [&](int dp, const String& name) {
		unlinkat(dp, name, 0);
		if(symlinkat(target, dp, name) == 0)
			return true;
		error = Errno("cannot create symlink " + rel);
		return false;
	});
}

bool LocalFs::Rename(const String& from, const String& to, String& error)
{
	int fp = ParentFd(from), tp = ParentFd(to);
	bool ok = fp >= 0 && tp >= 0 && renameat(fp, GetFileName(from), tp, GetFileName(to)) == 0;
	if(!ok)
		error = Errno("cannot rename " + from + " to " + to);
	if(fp >= 0) close(fp);
	if(tp >= 0) close(tp);
	return ok;
}

// mkdir -p for an absolute path, one component at a time (any length). Returns an
// O_RDONLY fd of the last folder, or -1.
static int MakeDirsAt(const String& path, String& error)
{
	int fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	for(const String& part : Split(path, '/')) {
		if(fd < 0)
			break;
		if(mkdirat(fd, part, 0755) != 0 && errno != EEXIST) {
			error = Errno("cannot create folder " + part + " in " + path);
			close(fd);
			return -1;
		}
		int next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		close(fd);
		fd = next;
	}
	if(fd < 0 && error.IsEmpty())
		error = Errno("cannot open " + path);
	return fd;
}

bool LocalFs::MoveOut(const String& rel, const String& dest, String& moved_to, String& error)
{
	int sp = ParentFd(rel);
	if(sp < 0) {
		error = Errno("cannot open the folder of " + rel);
		return false;
	}
	String name = GetFileName(rel);
	struct stat st;
	if(fstatat(sp, name, &st, AT_SYMLINK_NOFOLLOW) != 0 && errno == ENOENT) {
		close(sp);   // already moved (a resumed run)
		moved_to.Clear();
		return true;
	}
	int dp = MakeDirsAt(GetFileFolder(dest), error);
	if(dp < 0) {
		close(sp);
		return false;
	}
	// dest, or dest.1, dest.2, ... whichever is free.
	String base = GetFileName(dest), cand = base;
	for(int n = 1; fstatat(dp, cand, &st, AT_SYMLINK_NOFOLLOW) == 0; n++)
		cand = base + "." + AsString(n);
	bool ok = renameat(sp, name, dp, cand) == 0;
	if(!ok)
		error = errno == EXDEV
		      ? "cannot move " + rel + " to " + GetFileFolder(dest) + ": it is on a different disk; "
		        "use --safe-delete-dir on the same disk, or --no-safe-delete"
		      : Errno("cannot move " + rel + " to " + GetFileFolder(dest));
	else
		moved_to = AppendFileName(GetFileFolder(dest), cand);
	close(sp);
	close(dp);
	return ok;
}

bool LocalFs::Chown(const String& rel, int64 uid, int64 gid, String& error)
{
	return AtParent(*this, rel, error, [&](int dp, const String& name) {
		if(fchownat(dp, name, (uid_t)uid, (gid_t)gid, AT_SYMLINK_NOFOLLOW) == 0)
			return true;
		error = Errno("cannot set owner of " + rel);
		return false;
	});
}

bool LocalFs::HardLink(const String& existing, const String& rel, String& error)
{
	int ep = ParentFd(existing), dp = ParentFd(rel);
	if(dp >= 0)
		unlinkat(dp, GetFileName(rel), 0);
	bool ok = ep >= 0 && dp >= 0 && linkat(ep, GetFileName(existing), dp, GetFileName(rel), 0) == 0;
	if(!ok)
		error = Errno("cannot link " + rel + " to " + existing);
	if(ep >= 0) close(ep);
	if(dp >= 0) close(dp);
	return ok;
}
