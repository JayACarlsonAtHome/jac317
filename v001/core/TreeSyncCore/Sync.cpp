// Sync run: resume an unfinished run, or scan, plan and record a new one; then
// carry out the recorded actions one at a time. Each action is marked in_progress
// before it starts and done (or failed) after, so a stopped run resumes exactly.
// Every copy goes to a .treesync-part file, is read back and checked against the
// source's SHA-256, and only then renamed into place.
#include "TreeSyncCore.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>

using Clock = std::chrono::steady_clock;

static double Since(Clock::time_point t)
{
	return std::chrono::duration<double>(Clock::now() - t).count();
}

static String Errno(const String& what)
{
	return what + ": " + strerror(errno);
}

// Folder fd for the parent of rel inside root.
static int OpenParent(const String& root, const String& rel)
{
	String folder = GetFileFolder(rel);
	return OpenDir(folder.IsEmpty() ? root : AppendFileName(root, folder));
}

// NAME.treesync-part, or a short name when that would pass the 255-byte name limit.
static String PartName(const String& name, int64 run, int seq)
{
	String part = name + ".treesync-part";
	return part.GetCount() <= 255 ? part : Format(".treesync-%d-%d.treesync-part", run, seq);
}

namespace {

struct Step : Moveable<Step> {
	int    seq = 0;
	Action a;
	String state;
};

struct Runner {
	Db&           db;
	SyncReport&   report;
	SyncResult&   result;
	const SyncFlags& flags;
	String        roots[2];   // [0] source, [1] target
	int64         run = 0;
	int           copy_count = 0, copy_index = 0;
	Clock::time_point started = Clock::now();

	Runner(Db& db, SyncReport& report, SyncResult& result, const SyncFlags& flags)
	:	db(db), report(report), result(result), flags(flags) {}

	void SetState(int seq, const char *state, int64 bytes = 0, const String& error = Null)
	{
		Stmt(db, "UPDATE actions SET state = ?, bytes = ?, error = ? WHERE run = ? AND seq = ?")
			.Text(1, state).Int(2, bytes).Text(3, error).Int(4, run).Int(5, seq).Run();
	}

	// The copy was just verified, so the target's hash cache can take it as is:
	// the next scan of the target then reuses it instead of reading the file again.
	void Cache(const String& root, const String& rel, int dp, const String& name, const String& hash)
	{
		struct stat st;
		if(fstatat(dp, name, &st, AT_SYMLINK_NOFOLLOW) != 0)
			return;
		Stmt(db, "INSERT OR REPLACE INTO files VALUES (?, ?, 'f', ?, ?, ?, ?, ?, ?, ?, '', ?, 0)")
			.Text(1, root).Text(2, rel).Int(3, st.st_dev).Int(4, st.st_ino).Int(5, st.st_nlink)
			.Int(6, st.st_size).Int(7, (int64)st.st_mtim.tv_sec * 1000000000 + st.st_mtim.tv_nsec)
			.Int(8, (int64)st.st_ctim.tv_sec * 1000000000 + st.st_ctim.tv_nsec)
			.Int(9, st.st_mode & 07777).Text(10, hash).Run();
	}

	void Synced(const String& rel, const String& hash)
	{
		Stmt(db, "INSERT OR REPLACE INTO synced VALUES (?, ?, ?, ?)")
			.Text(1, roots[0]).Text(2, roots[1]).Text(3, rel).Text(4, hash).Run();
	}

	// Chunked copy for big files; every finished chunk is recorded, so a restart
	// continues from the next one. Returns false with error "stopped" on Stop.
	bool CopyBig(int seq, int sp, const String& name, int dp, const String& part,
	             const String& rel, int64 size, CopyProgress& p, Clock::time_point t0, String& error)
	{
		int64 chunk = (int64)flags.chunk_mib << 20;
		int in = openat(sp, name, O_RDONLY | O_CLOEXEC);
		if(in < 0) {
			error = Errno("cannot open source " + rel);
			return false;
		}
		int out = openat(dp, part, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
		if(out < 0) {
			error = Errno("cannot create " + part);
			close(in);
			return false;
		}
		Buffer<byte> buf((size_t)min(chunk, max<int64>(size, 1)));
		auto read_at = [&](int fd, int64 off, int64 len) -> bool {
			for(int64 got = 0; got < len;) {
				ssize_t n = pread(fd, buf + got, (size_t)(len - got), off + got);
				if(n < 0 && errno == EINTR)
					continue;
				if(n <= 0)
					return false;
				got += n;
			}
			return true;
		};

		// Chunks already done in an earlier, stopped attempt.
		Vector<String> done;
		Stmt q(db, "SELECT idx, sha256 FROM chunks WHERE run = ? AND seq = ? ORDER BY idx");
		q.Int(1, run).Int(2, seq);
		while(q.Step() && q.Int(0) == done.GetCount())
			done.Add(q.Text(1));
		int64 count = (size + chunk - 1) / chunk;
		int next = done.GetCount();
		if(next > 0) {
			// The last recorded chunk may have been torn by the stop: check it again.
			int64 off = (next - 1) * chunk, len = min(chunk, size - off);
			if(!read_at(out, off, len) || SHA256String(~buf, (dword)len) != done.Top())
				next--;
			Stmt(db, "DELETE FROM chunks WHERE run = ? AND seq = ? AND idx >= ?")
				.Int(1, run).Int(2, seq).Int(3, next).Run();
			report.WhenLine(Format("resuming %s at chunk %d of %d", rel, next + 1, count));
		}

		bool ok = true;
		for(int64 idx = next; idx < count; idx++) {
			if(report.Stopping()) {
				error = "stopped";
				ok = false;
				break;
			}
			int64 off = idx * chunk, len = min(chunk, size - off);
			if(!read_at(in, off, len)) {
				error = Errno("cannot read " + rel);
				ok = false;
				break;
			}
			for(int64 put = 0; put < len;) {
				ssize_t n = pwrite(out, buf + put, (size_t)(len - put), off + put);
				if(n < 0 && errno == EINTR)
					continue;
				if(n <= 0) {
					error = Errno("cannot write " + part);
					ok = false;
					break;
				}
				put += n;
			}
			if(!ok)
				break;
			fdatasync(out);
			Stmt(db, "INSERT OR REPLACE INTO chunks VALUES (?, ?, ?, ?, ?, ?)")
				.Int(1, run).Int(2, seq).Int(3, idx).Int(4, off).Int(5, len)
				.Text(6, SHA256String(~buf, (dword)len)).Run();
			result.bytes += len;
			p.done = off + len;
			p.seconds = Since(t0);
			if(p.done < p.size)
				report.WhenProgress(p);
		}
		if(ok && ftruncate(out, size) != 0) {
			error = Errno("cannot size " + part);
			ok = false;
		}
		close(in);
		close(out);
		return ok;
	}

	// Copies (or links, or re-creates a symlink) a.from.rel, verified, into place.
	bool Copy(const Step& s, String& error)
	{
		const Action& a = s.a;
		const Entry& e = a.from;
		const String& from = roots[a.dir];
		const String& to = roots[1 - a.dir];
		String name = GetFileName(e.rel);
		String part = PartName(name, run, s.seq);
		int sp = OpenParent(from, e.rel);
		int dp = OpenParent(to, e.rel);
		if(sp < 0 || dp < 0) {
			error = Errno("cannot open folder for " + e.rel);
			if(sp >= 0) close(sp);
			if(dp >= 0) close(dp);
			return false;
		}
		bool ok = false;
		auto finish = [&] { close(sp); close(dp); return ok; };

		if(a.kind == ActionKind::Link) {
			int fp = OpenParent(to, a.link_to);
			ok = fp >= 0 && linkat(fp, GetFileName(a.link_to), dp, part, 0) == 0 &&
			     renameat(dp, part, dp, name) == 0;
			if(!ok)
				error = Errno("cannot link " + e.rel + " to " + a.link_to);
			else {
				report.WhenLine("linked " + e.rel + " -> " + a.link_to);
				// A new link changes the inode's ctime: refresh the cache for both names.
				Cache(to, e.rel, dp, name, e.hash);
				Cache(to, a.link_to, fp, GetFileName(a.link_to), e.hash);
				Synced(e.rel, e.hash);
			}
			if(fp >= 0)
				close(fp);
			return finish();
		}
		if(e.type == 'l') {
			unlinkat(dp, part, 0);
			ok = symlinkat(e.link, dp, part) == 0 && renameat(dp, part, dp, name) == 0;
			if(!ok)
				error = Errno("cannot create symlink " + e.rel);
			else
				report.WhenLine("symlink " + e.rel + " -> " + e.link);
			return finish();
		}

		// The source may have changed since the run was planned: then hash it again.
		struct stat st;
		if(fstatat(sp, name, &st, flags.follow_links ? 0 : AT_SYMLINK_NOFOLLOW) != 0) {
			error = Errno("source gone: " + e.rel);
			return finish();
		}
		String expect = e.hash;
		int64 size = st.st_size;
		if(st.st_size != e.size || st.st_ino != e.ino ||
		   (int64)st.st_mtim.tv_sec * 1000000000 + st.st_mtim.tv_nsec != e.mtime_ns) {
			report.WhenLine("source changed since planning, hashing again: " + e.rel);
			if(!HashAt(sp, name, expect, error, report.stop, nullptr, flags.follow_links))
				return finish();
		}

		copy_index++;
		CopyProgress p;
		p.index = copy_index;
		p.count = copy_count;
		p.name = e.rel;
		p.size = size;
		Clock::time_point t0 = Clock::now();
		bool big = size >= ((int64)flags.big_file_mib << 20);
		for(int attempt = 0; attempt <= flags.retries; attempt++) {
			if(attempt) {
				report.WhenLine(Format("retry %d of %d in %ds: %s (%s)", attempt, flags.retries,
				                       flags.retry_wait, e.rel, error));
				std::this_thread::sleep_for(std::chrono::seconds(flags.retry_wait));
			}
			error.Clear();
			ok = big ? CopyBig(s.seq, sp, name, dp, part, e.rel, size, p, t0, error)
			         : CopySmall(sp, name, dp, part, error);
			if(ok || error == "stopped" || report.Stopping())
				break;
		}
		if(!ok)
			return finish();
		if(!big)
			result.bytes += size;

		// Read the copy back: it must have the source's SHA-256.
		String got;
		if(!HashAt(dp, part, got, error, report.stop))
			return finish(), false;
		if(got != expect) {
			error = "verify failed: copy of " + e.rel + " has a different SHA-256; kept as " + part;
			return ok = false, finish();
		}
		if(big) {
			int fd = openat(dp, part, O_WRONLY | O_CLOEXEC);
			if(fd >= 0) {
				fchmod(fd, st.st_mode & 07777);
				if(IsRoot() && fchown(fd, st.st_uid, st.st_gid) != 0)
					report.WhenLine(Errno("warning: cannot set owner of " + e.rel));
				timespec times[2] = { st.st_atim, st.st_mtim };
				futimens(fd, times);
				close(fd);
			}
		}
		if(renameat(dp, part, dp, name) != 0) {
			error = Errno("cannot rename " + part + " into place");
			return ok = false, finish();
		}
		p.done = p.size;
		p.seconds = Since(t0);
		report.WhenProgress(p);
		Synced(e.rel, expect);
		Cache(to, e.rel, dp, name, expect);
		return finish();
	}

	bool Mkdir(const Action& a, String& error)
	{
		const String& to = roots[1 - a.dir];
		int dp = OpenParent(to, a.from.rel);
		if(dp < 0)
			return error = Errno("cannot open folder for " + a.from.rel), false;
		String name = GetFileName(a.from.rel);
		bool ok = mkdirat(dp, name, (a.from.mode & 07777) | 0700) == 0;
		struct stat st;
		if(!ok && errno == EEXIST && fstatat(dp, name, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(st.st_mode))
			ok = true;
		if(!ok)
			error = Errno("cannot create folder " + a.from.rel);
		else
			report.WhenLine("created folder " + a.from.rel);
		close(dp);
		return ok;
	}

	bool Delete(const Action& a, String& error)
	{
		const String& to = roots[1 - a.dir];
		int dp = OpenParent(to, a.from.rel);
		if(dp < 0)
			return error = Errno("cannot open folder for " + a.from.rel), false;
		String name = GetFileName(a.from.rel);
		bool dir = a.from.type == 'd';
		bool ok = unlinkat(dp, name, dir ? AT_REMOVEDIR : 0) == 0 || errno == ENOENT;
		if(!ok && dir && (errno == ENOTEMPTY || errno == EEXIST)) {
			report.WhenLine("kept folder " + a.from.rel + ": it still holds excluded files");
			ok = true;
		}
		else if(!ok)
			error = Errno("cannot delete " + a.from.rel);
		else
			report.WhenLine("deleted " + a.from.rel);
		close(dp);
		return ok;
	}

	bool Execute()
	{
		Vector<Step> steps;
		Stmt q(db, "SELECT seq, kind, dir, rel, type, link, link_to, size, sha256, dev, ino, "
		           "mtime_ns, ctime_ns, mode, note, state FROM actions WHERE run = ? ORDER BY seq");
		q.Int(1, run);
		while(q.Step()) {
			Step& s = steps.Add();
			s.seq = (int)q.Int(0);
			String kind = q.Text(1);
			for(ActionKind k : { ActionKind::Mkdir, ActionKind::Copy, ActionKind::Link,
			                     ActionKind::Delete, ActionKind::Conflict })
				if(kind == KindName(k))
					s.a.kind = k;
			s.a.dir = (int)q.Int(2);
			Entry& e = s.a.from;
			e.rel = q.Text(3);
			e.type = *q.Text(4);
			e.link = q.Text(5);
			s.a.link_to = q.Text(6);
			e.size = q.Int(7);
			e.hash = q.Text(8);
			e.dev = q.Int(9);
			e.ino = q.Int(10);
			e.mtime_ns = q.Int(11);
			e.ctime_ns = q.Int(12);
			e.mode = (int)q.Int(13);
			s.a.note = q.Text(14);
			s.state = q.Text(15);
		}
		result.planned = steps.GetCount();
		for(const Step& s : steps)
			if(s.a.kind == ActionKind::Copy && s.a.from.type == 'f') {
				copy_count++;
				if(s.state == "done")
					copy_index++;
			}

		for(const Step& s : steps) {
			if(s.state == "done") {
				result.done++;
				continue;
			}
			if(report.Stopping()) {
				result.stopped = true;
				break;
			}
			SetState(s.seq, "in_progress");
			String error;
			bool ok = true;
			switch(s.a.kind) {
			case ActionKind::Mkdir:  ok = Mkdir(s.a, error); break;
			case ActionKind::Copy:
			case ActionKind::Link:   ok = Copy(s, error); break;
			case ActionKind::Delete: ok = Delete(s.a, error); break;
			case ActionKind::Conflict:
				result.conflicts++;
				report.WhenLine("CONFLICT " + s.a.from.rel + ": " + s.a.note);
				break;
			}
			if(!ok && (error == "stopped" || report.Stopping())) {
				SetState(s.seq, "planned");   // not failed: redo it on resume
				result.stopped = true;
				break;
			}
			if(ok) {
				SetState(s.seq, "done");
				result.done++;
			}
			else {
				SetState(s.seq, "failed", 0, error);
				result.failed++;
				report.WhenLine("FAILED " + s.a.from.rel + ": " + error);
			}
		}
		Stmt(db, "UPDATE runs SET state = ?, finished = ? WHERE id = ?")
			.Text(1, result.stopped ? "stopped" : result.failed ? "failed" : "done")
			.Int(2, GetUTCSeconds(GetSysTime())).Int(3, run).Run();

		if(copy_count == 0)
			report.WhenLine("nothing to copy");
		else {
			CopyProgress total;
			total.index = copy_index;
			total.count = copy_count;
			total.name = result.stopped ? "Total (stopped)" : "Total";
			total.done = total.size = result.bytes;
			total.seconds = Since(started);
			report.WhenLine(ProgressLine(total));
		}
		return true;
	}
};

}

bool RunSync(const SyncSetup& setup, SyncReport& report, SyncResult& result, String& error)
{
	result = SyncResult();
	const SyncFlags& flags = setup.flags;
	if(setup.source.remote || setup.target.remote) {
		error = "remote trees are not supported by the engine yet";
		return false;
	}
	if(setup.source.path.IsEmpty() || setup.target.path.IsEmpty()) {
		error = "both --source and --target are needed";
		return false;
	}
	String src = CanonicalPath(setup.source.path);
	String dst = CanonicalPath(setup.target.path);
	if(IsUnder(dst, src) || IsUnder(src, dst)) {
		error = "source and target overlap: " + src + " and " + dst;
		return false;
	}
	if(!PathAllowed(src) || !PathAllowed(dst)) {
		error = RefusedPathMessage(PathAllowed(src) ? dst : src);
		return false;
	}

	Db db;
	String db_path = Nvl(setup.db, DefaultDbPath());
	if(!db.Open(db_path, error))
		return false;

	SyncFlags key_flags = flags;
	key_flags.dry_run = false;
	String key = Join(FlagArgs(key_flags), " ");

	Runner r(db, report, result, flags);
	r.roots[0] = src;
	r.roots[1] = dst;

	// An unfinished run for the same trees and switches: continue it.
	if(!setup.fresh && !flags.dry_run) {
		Stmt q(db, "SELECT id FROM runs WHERE source = ? AND target = ? AND args = ? "
		           "AND state IN ('running', 'stopped') ORDER BY id DESC LIMIT 1");
		q.Text(1, src).Text(2, dst).Text(3, key);
		if(q.Step()) {
			r.run = result.run = q.Int(0);
			result.resumed = true;
			Stmt(db, "UPDATE runs SET state = 'running' WHERE id = ?").Int(1, r.run).Run();
			Stmt c(db, "SELECT count(*), sum(state = 'done') FROM actions WHERE run = ?");
			c.Int(1, r.run);
			c.Step();
			report.WhenLine(Format("resuming run %d: %d of %d steps already done", r.run, c.Int(1), c.Int(0)));
			return r.Execute();
		}
	}
	if(setup.fresh)
		Stmt(db, "UPDATE runs SET state = 'abandoned' WHERE source = ? AND target = ? "
		         "AND state IN ('running', 'stopped')").Text(1, src).Text(2, dst).Run();

	std::error_code ec;
	bool dst_exists = std::filesystem::is_directory(ToFs(dst), ec);
	if(!dst_exists && !flags.dry_run) {
		std::filesystem::create_directories(ToFs(dst), ec);
		if(ec) {
			error = "cannot create target " + dst + ": " + String(ec.message());
			return false;
		}
		dst_exists = true;
	}

	Vector<Entry> se, de;
	String sh, dh;
	report.WhenLine("scanning source " + src);
	if(!ScanTree(db, src, flags, report, se, sh, result.source, error))
		return error == "stopped" ? (result.stopped = true) : false;
	report.WhenLine(Format("source: %d entries, %d hashed (%s), %d unchanged since last scan",
	                       result.source.entries, result.source.hashed,
	                       FormatBytes(result.source.hashed_bytes), result.source.reused));
	if(dst_exists) {
		report.WhenLine("scanning target " + dst);
		if(!ScanTree(db, dst, flags, report, de, dh, result.target, error))
			return error == "stopped" ? (result.stopped = true) : false;
		report.WhenLine(Format("target: %d entries, %d hashed (%s), %d unchanged since last scan",
		                       result.target.entries, result.target.hashed,
		                       FormatBytes(result.target.hashed_bytes), result.target.reused));
	}

	VectorMap<String, String> last;
	if(flags.mode == SyncMode::Merge) {
		Stmt q(db, "SELECT rel, sha256 FROM synced WHERE source = ? AND target = ?");
		q.Text(1, src).Text(2, dst);
		while(q.Step())
			last.Add(q.Text(0), q.Text(1));
	}
	Vector<Action> plan = PlanSync(se, de, flags, last);
	result.planned = plan.GetCount();

	if(flags.dry_run) {
		for(const Action& a : plan) {
			String line = Format("%-8s %s", KindName(a.kind), a.from.rel);
			if(a.dir)
				line << "   (target -> source)";
			if(a.kind == ActionKind::Link)
				line << " -> " << a.link_to;
			if(a.note.GetCount())
				line << ": " << a.note;
			result.plan.Add(line);
			report.WhenLine("would " + line);
		}
		report.WhenLine(Format("dry run: %d actions, nothing changed", plan.GetCount()));
		return true;
	}

	// Record the whole plan before doing any of it.
	db.Exec("BEGIN");
	Stmt(db, "INSERT INTO runs (started, source, target, args, state) VALUES (?, ?, ?, ?, 'running')")
		.Int(1, GetUTCSeconds(GetSysTime())).Text(2, src).Text(3, dst).Text(4, key).Run();
	r.run = result.run = db.LastId();
	Stmt ins(db, "INSERT INTO actions VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 'planned', 0, NULL)");
	for(int i = 0; i < plan.GetCount(); i++) {
		const Action& a = plan[i];
		const Entry& e = a.from;
		ins.Int(1, r.run).Int(2, i).Text(3, KindName(a.kind)).Int(4, a.dir).Text(5, e.rel)
		   .Text(6, String(e.type, 1)).Text(7, e.link).Text(8, a.link_to).Int(9, e.size)
		   .Text(10, e.hash).Int(11, e.dev).Int(12, e.ino).Int(13, e.mtime_ns)
		   .Int(14, e.ctime_ns).Int(15, e.mode).Text(16, a.note);
		ins.Run();
	}
	// Merge: remember pairs that are already equal, so later changes show a direction.
	if(flags.mode == SyncMode::Merge) {
		VectorMap<String, String> dh_map;
		for(const Entry& e : de)
			if(e.type != 'd')
				dh_map.Add(e.rel, e.hash);
		Stmt syn(db, "INSERT OR REPLACE INTO synced VALUES (?, ?, ?, ?)");
		for(const Entry& e : se)
			if(e.type != 'd' && dh_map.Get(e.rel, Null) == e.hash && last.Get(e.rel, Null) != e.hash)
				syn.Text(1, src).Text(2, dst).Text(3, e.rel).Text(4, e.hash).Run();
	}
	db.Exec("COMMIT");
	report.WhenLine(Format("run %d: %d actions planned", r.run, plan.GetCount()));
	return r.Execute();
}
