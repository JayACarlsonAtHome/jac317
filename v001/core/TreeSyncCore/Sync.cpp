// Sync run: resume an unfinished run, or scan, plan and record a new one; then
// carry out the recorded actions one at a time. Each action is marked in_progress
// before it starts and done (or failed) after, so a stopped run resumes exactly.
// Every copy goes to a .treesync-part file, is read back and checked against the
// source's SHA-256, and only then renamed into place.
//
// Each side is a TreeFs (local or SFTP). Local to local, small files go through
// /usr/bin/cp; everything else uses the chunked copy, each chunk recorded, so
// remote copies resume part-way through a file just like big local ones.
#include "TreeSyncCore.h"

#include <unistd.h>
#include <chrono>
#include <thread>

using Clock = std::chrono::steady_clock;

static double Since(Clock::time_point t)
{
	return std::chrono::duration<double>(Clock::now() - t).count();
}

// NAME.treesync-part next to rel, or a short name when that would pass the
// 255-byte name limit.
static String PartRel(const String& rel, int64 run, int seq)
{
	String name = GetFileName(rel) + ".treesync-part";
	if(name.GetCount() > 255)
		name = Format(".treesync-%d-%d.treesync-part", run, seq);
	String folder = GetFileFolder(rel);
	return folder.IsEmpty() ? name : folder + "/" + name;
}

// Errors that waiting will not fix: no point in retrying them.
static bool Permanent(const String& error)
{
	String e = ToLower(error);
	for(const char *s : { "permission denied", "operation not permitted", "no space left",
	                      "read-only file system", "quota", "not supported", "no such file" })
		if(e.Find(s) >= 0)
			return true;
	return false;
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
	TreeFs       *fs[2] = {};   // [0] source, [1] target
	String        ids[2];
	int64         run = 0;
	int           copy_count = 0, copy_index = 0;
	Clock::time_point started = Clock::now();
	String        old_roots[2];   // --safe-delete: where each side's deleted/replaced files go
	IdTable       id_tables[2];
	bool          ids_loaded = false;
	bool          owner_note[2] = {};
	Index<String> unknown_names;

	Runner(Db& db, SyncReport& report, SyncResult& result, const SyncFlags& flags)
	:	db(db), report(report), result(result), flags(flags) {}

	void SetState(int seq, const char *state, int64 bytes = 0, const String& error = Null)
	{
		Stmt(db, "UPDATE actions SET state = ?, bytes = ?, error = ? WHERE run = ? AND seq = ?")
			.Text(1, state).Int(2, bytes).Text(3, error).Int(4, run).Int(5, seq).Run();
	}

	// A verified copy goes straight into the target's hash cache, so the next scan
	// of the target reuses the hash instead of reading the file again.
	void Cache(TreeFs& to, int side, const String& rel, const String& hash, const Entry *known = nullptr)
	{
		Entry e;
		String error;
		if(known)
			e = clone(*known);
		else if(!to.Stat(rel, false, e, error))
			return;
		Stmt(db, "INSERT OR REPLACE INTO files VALUES (?, ?, 'f', ?, ?, ?, ?, ?, ?, ?, '', ?, 0)")
			.Text(1, ids[side]).Text(2, rel).Int(3, e.dev).Int(4, e.ino).Int(5, e.nlink)
			.Int(6, e.size).Int(7, e.mtime_ns).Int(8, e.ctime_ns).Int(9, e.mode).Text(10, hash).Run();
	}

	// --preserve-owner, and the side being written can take owners (root there).
	bool Owners(int side)
	{
		if(!flags.preserve_owner)
			return false;
		if(!fs[side]->CanChown()) {
			if(!owner_note[side])
				report.WhenLine("owners not copied to " + fs[side]->Name() +
				                ": that needs root there (--no-preserve-owner hides this note)");
			owner_note[side] = true;
			return false;
		}
		if(!ids_loaded && !flags.numeric_ids) {
			for(int i = 0; i < 2; i++) {
				String error;
				if(!fs[i]->LoadIds(id_tables[i], error))
					report.WhenLine("warning: " + error + "; owners copied by number");
			}
			ids_loaded = true;
		}
		return true;
	}

	// Owner and group from side `from`, as the numbers the other side uses for the same names.
	void MapOwner(int from, int64 uid, int64 gid, FileAttrs& fa)
	{
		for(int group = 0; group < 2; group++) {
			String unknown;
			int64 id = MapId(id_tables[from], id_tables[1 - from], group, group ? gid : uid,
			                 flags.numeric_ids || !ids_loaded, &unknown);
			(group ? fa.gid : fa.uid) = id;
			if(unknown.GetCount() && unknown_names.Find(unknown) < 0) {
				unknown_names.Add(unknown);
				report.WhenLine(Format("warning: %s '%s' does not exist on %s; kept its number %d",
				                       group ? "group" : "user", unknown, fs[1 - from]->Name(), id));
			}
		}
	}

	// Sets the owner of something just created (folder or local symlink) from the source.
	void CopyOwner(int dir, const String& rel)
	{
		if(!Owners(1 - dir))
			return;
		Entry src;
		String error;
		if(!fs[dir]->Stat(rel, false, src, error) || src.uid < 0)
			return;
		FileAttrs fa;
		MapOwner(dir, src.uid, src.gid, fa);
		if(!fs[1 - dir]->Chown(rel, fa.uid, fa.gid, error))
			report.WhenLine("warning: " + error);
	}

	// --safe-delete: move what is at rel on side `side` out to its .old tree.
	bool MoveOut(int side, const String& rel, String& error)
	{
		String moved;
		if(!fs[side]->MoveOut(rel, old_roots[side] + "/" + rel, moved, error))
			return false;
		if(moved.GetCount())
			report.WhenLine("moved " + rel + " to " + moved);
		return true;
	}

	// Makes room at rel for a new version: the old one moves out if --safe-delete.
	bool MakeRoom(const Action& a, String& error)
	{
		return !(a.replaces && flags.safe_delete) || MoveOut(1 - a.dir, a.from.rel, error);
	}

	void Synced(const String& rel, const String& hash)
	{
		Stmt(db, "INSERT OR REPLACE INTO synced VALUES (?, ?, ?, ?)")
			.Text(1, ids[0]).Text(2, ids[1]).Text(3, rel).Text(4, hash).Run();
	}

	// Chunked copy; every finished chunk is recorded, so a restart continues from
	// the next one. Returns false with error "stopped" on Stop.
	// out stays open for the caller, to verify and set attributes through it.
	bool ChunkCopy(int seq, TreeFs& from, TreeFs& to, const String& rel, const String& part,
	               int64 size, CopyProgress& p, Clock::time_point t0, One<TreeFile>& out, String& error)
	{
		int64 chunk = (int64)flags.chunk_mib << 20;
		One<TreeFile> in = from.Open(rel, false, error);
		if(!in)
			return false;
		out = to.Open(part, true, error);
		if(!out)
			return false;
		Buffer<byte> buf((size_t)min(chunk, max<int64>(size, 1)));

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
			String ignore;
			if(!out->ReadAt(off, buf, len, ignore) || SHA256String(~buf, (dword)len) != done.Top())
				next--;
			Stmt(db, "DELETE FROM chunks WHERE run = ? AND seq = ? AND idx >= ?")
				.Int(1, run).Int(2, seq).Int(3, next).Run();
			report.WhenLine(Format("resuming %s at chunk %d of %d", rel, next + 1, count));
		}

		for(int64 idx = next; idx < count; idx++) {
			if(report.Stopping()) {
				error = "stopped";
				return false;
			}
			int64 off = idx * chunk, len = min(chunk, size - off);
			if(!in->ReadAt(off, buf, len, error) || !out->WriteAt(off, buf, len, error))
				return false;
			// A recorded chunk must really be on disk. A one-chunk file needs no flush:
			// if it is cut short it is copied again, and a later scan catches a copy
			// lost in a crash after its rename.
			if(count > 1)
				out->Flush();
			Stmt(db, "INSERT OR REPLACE INTO chunks VALUES (?, ?, ?, ?, ?, ?)")
				.Int(1, run).Int(2, seq).Int(3, idx).Int(4, off).Int(5, len)
				.Text(6, SHA256String(~buf, (dword)len)).Run();
			result.bytes += len;
			p.done = off + len;
			p.seconds = Since(t0);
			if(p.done < p.size)
				report.WhenProgress(p);
		}
		return out->Truncate(size, error);
	}

	// Copies (or links, or re-creates a symlink) a.from.rel, verified, into place.
	bool Copy(const Step& s, String& error)
	{
		const Action& a = s.a;
		const Entry& e = a.from;
		TreeFs& from = *fs[a.dir];
		TreeFs& to = *fs[1 - a.dir];
		String part = PartRel(e.rel, run, s.seq);

		if(a.kind == ActionKind::Link) {
			if(to.CanHardLink()) {
				if(!to.HardLink(a.link_to, part, error) || !MakeRoom(a, error) || !to.Rename(part, e.rel, error))
					return false;
				copy_index++;
				report.WhenLine("linked " + e.rel + " -> " + a.link_to);
				// A new link changes the inode's ctime: refresh the cache for both names.
				Cache(to, 1 - a.dir, e.rel, e.hash);
				Cache(to, 1 - a.dir, a.link_to, e.hash);
				Synced(e.rel, e.hash);
				return true;
			}
			report.WhenLine("hard link written out as a copy (the target cannot hard-link): " + e.rel);
		}
		if(e.type == 'l') {
			if(!to.Symlink(part, e.link, error) || !MakeRoom(a, error) || !to.Rename(part, e.rel, error))
				return false;
			report.WhenLine("symlink " + e.rel + " -> " + e.link);
			if(to.IsLocal())   // SFTP cannot set a symlink's own owner
				CopyOwner(a.dir, e.rel);
			return true;
		}

		// The source may have changed since the run was planned: then hash it again.
		Entry now;
		if(!from.Stat(e.rel, flags.follow_links, now, error))
			return false;
		String expect = e.hash;
		if(now.size != e.size || now.mtime_ns != e.mtime_ns || (from.IsLocal() && now.ino != e.ino)) {
			report.WhenLine("source changed since planning, hashing again: " + e.rel);
			if(!from.Hash(e.rel, flags.follow_links, expect, error, report.stop))
				return false;
		}

		copy_index++;
		CopyProgress p;
		p.index = copy_index;
		p.count = copy_count;
		p.name = e.rel;
		p.size = now.size;
		Clock::time_point t0 = Clock::now();
		bool big = now.size >= ((int64)flags.big_file_mib << 20);
		bool fast = from.IsLocal() && to.IsLocal() && !big;
		bool ok = false;
		One<TreeFile> out;
		for(int attempt = 0; attempt <= flags.retries; attempt++) {
			if(attempt) {
				report.WhenLine(Format("retry %d of %d in %d s: %s (%s)", attempt, flags.retries,
				                       flags.retry_wait, e.rel, error));
				std::this_thread::sleep_for(std::chrono::seconds(flags.retry_wait));
			}
			error.Clear();
			if(fast) {
				const LocalFs& lf = static_cast<const LocalFs&>(from);
				const LocalFs& lt = static_cast<const LocalFs&>(to);
				int sp = lf.ParentFd(e.rel), dp = lt.ParentFd(e.rel);
				ok = sp >= 0 && dp >= 0 && CopySmall(sp, GetFileName(e.rel), dp, GetFileName(part), error);
				if(sp < 0 || dp < 0)
					error = "cannot open the folders of " + e.rel;
				if(sp >= 0) close(sp);
				if(dp >= 0) close(dp);
			}
			else
				ok = ChunkCopy(s.seq, from, to, e.rel, part, now.size, p, t0, out, error);
			if(ok || error == "stopped" || report.Stopping() || Permanent(error))
				break;
		}
		if(!ok)
			return false;
		if(fast)
			result.bytes += now.size;

		// Read the copy back: it must have the source's SHA-256. The chunked copy
		// reads it through the handle it wrote with, and sets the attributes there too.
		String got;
		if(fast ? !to.Hash(part, false, got, error, report.stop)
		        : !out->Hash(now.size, got, error, report.stop))
			return false;
		if(got != expect) {
			error = "verify failed: copy of " + e.rel + " has a different SHA-256; kept as " + part;
			return false;
		}
		if(!fast) {   // cp --preserve already did this
			FileAttrs fa;
			fa.mode = now.mode;
			fa.mtime_ns = now.mtime_ns;
			if(now.uid >= 0 && Owners(1 - a.dir))
				MapOwner(a.dir, now.uid, now.gid, fa);
			String warn;
			if(!out->SetAttrs(fa, warn))
				report.WhenLine("warning: " + warn);
			out.Clear();   // close before the rename
		}
		if(!MakeRoom(a, error) || !to.Rename(part, e.rel, error))
			return false;
		p.done = p.size;
		p.seconds = Since(t0);
		report.WhenProgress(p);
		Synced(e.rel, expect);
		if(to.IsLocal())
			Cache(to, 1 - a.dir, e.rel, expect);
		else {
			// Over SFTP everything needed is already known: save a round trip.
			Entry placed;
			placed.size = now.size;
			placed.mtime_ns = now.mtime_ns / 1000000000 * 1000000000;   // SFTP keeps whole seconds
			placed.mode = now.mode;
			Cache(to, 1 - a.dir, e.rel, expect, &placed);
		}
		return true;
	}

	bool Mkdir(const Action& a, String& error)
	{
		if(!fs[1 - a.dir]->Mkdir(a.from.rel, a.from.mode, error))
			return false;
		report.WhenLine("created folder " + a.from.rel);
		CopyOwner(a.dir, a.from.rel);
		return true;
	}

	bool Delete(const Action& a, String& error)
	{
		// Folders are emptied first (deletes run deepest first) and then removed;
		// with --safe-delete their files have already moved out.
		if(flags.safe_delete && a.from.type != 'd')
			return MoveOut(1 - a.dir, a.from.rel, error);
		int r = fs[1 - a.dir]->Remove(a.from.rel, a.from.type == 'd', error);
		if(r < 0)
			return false;
		report.WhenLine(r ? "kept folder " + a.from.rel + ": it still holds excluded files"
		                  : "deleted " + a.from.rel);
		return true;
	}

	bool Execute()
	{
		Vector<Step> steps;
		Stmt q(db, "SELECT seq, kind, dir, rel, type, link, link_to, size, sha256, dev, ino, "
		           "mtime_ns, ctime_ns, mode, note, state, replaces FROM actions WHERE run = ? ORDER BY seq");
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
			s.a.replaces = q.Int(16);
		}
		result.planned = steps.GetCount();
		// Links count too: on a target that cannot hard-link they are copied.
		for(const Step& s : steps)
			if((s.a.kind == ActionKind::Copy || s.a.kind == ActionKind::Link) && s.a.from.type == 'f') {
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

// Makes one side: a LocalFs, or an SftpFs that is connected.
static One<TreeFs> MakeSide(const TreeEndpoint& e, const SyncSetup& setup, SyncReport& report, String& error)
{
	One<TreeFs> fs;
	if(!e.remote) {
		String path = CanonicalPath(e.path);
		if(!PathAllowed(path)) {
			error = RefusedPathMessage(path);
			return fs;
		}
		fs.Create<LocalFs>(path);
		return fs;
	}
	if(e.host.IsEmpty()) {
		error = "a remote side needs a host (--source-host or --target-host)";
		return fs;
	}
	report.WhenLine(Format("connecting to %s@%s:%d", Nvl(e.user, GetUserName()), e.host, e.port));
	SftpFs& s = fs.Create<SftpFs>(e, setup.known_hosts, setup.flags.compress);
	if(!s.Connect(error))
		fs.Clear();
	return fs;
}

}

bool CompareSides(const SyncSetup& setup, SyncReport& report, String& out, String& error)
{
	if(!setup.source.remote && !setup.target.remote) {
		error = "--compare-ids compares two machines: give --source-host or --target-host";
		return false;
	}
	One<TreeFs> side[2];
	IdTable t[2];
	for(int i = 0; i < 2; i++) {
		side[i] = MakeSide(i ? setup.target : setup.source, setup, report, error);
		if(!side[i] || !side[i]->LoadIds(t[i], error))
			return false;
	}
	out = CompareIds(t[0], t[1], side[0]->Name(), side[1]->Name());
	return true;
}

bool RunSync(const SyncSetup& setup, SyncReport& report, SyncResult& result, String& error)
{
	result = SyncResult();
	const SyncFlags& flags = setup.flags;
	if(setup.source.path.IsEmpty() || setup.target.path.IsEmpty()) {
		error = "both --source and --target are needed";
		return false;
	}
	if(!setup.source.remote && !setup.target.remote) {
		String src = CanonicalPath(setup.source.path), dst = CanonicalPath(setup.target.path);
		if(IsUnder(dst, src) || IsUnder(src, dst)) {
			error = "source and target overlap: " + src + " and " + dst;
			return false;
		}
	}
	One<TreeFs> side[2];
	side[0] = MakeSide(setup.source, setup, report, error);
	if(!side[0])
		return false;
	side[1] = MakeSide(setup.target, setup, report, error);
	if(!side[1])
		return false;

	Db db;
	String db_path = Nvl(setup.db, DefaultDbPath());
	if(!db.Open(db_path, error))
		return false;

	SyncFlags key_flags = flags;
	key_flags.dry_run = false;
	String key = Join(FlagArgs(key_flags), " ");

	Runner r(db, report, result, flags);
	for(int i = 0; i < 2; i++) {
		r.fs[i] = ~side[i];
		r.ids[i] = side[i]->Id();
	}
	const String& src = r.ids[0];
	const String& dst = r.ids[1];

	// --safe-delete: <side>.old next to each tree, or --safe-delete-dir for the target.
	for(int i = 0; i < 2; i++) {
		r.old_roots[i] = side[i]->RootPath() + ".old";
		if(i == 1 && setup.safe_delete_dir.GetCount())
			r.old_roots[i] = setup.target.remote ? setup.safe_delete_dir : CanonicalPath(setup.safe_delete_dir);
	}
	// Each .old folder is on its own side's machine; it can only meet the other
	// tree when both trees are local.
	bool both_local = side[0]->IsLocal() && side[1]->IsLocal();
	if(flags.safe_delete)
		for(int i = 0; i < 2; i++) {
			const String& old = r.old_roots[i];
			for(int t = 0; t < 2; t++)
				if((t == i || both_local) && (IsUnder(old, side[t]->RootPath()) || IsUnder(side[t]->RootPath(), old))) {
					error = "--safe-delete folder " + old + " must be outside both trees";
					return false;
				}
			if(side[i]->IsLocal() && !PathAllowed(old)) {
				error = RefusedPathMessage(old);
				return false;
			}
		}

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

	if(!side[0]->RootExists()) {
		error = "source " + src + " does not exist or is not a folder";
		return false;
	}
	bool dst_exists = side[1]->RootExists();
	if(!dst_exists && !flags.dry_run) {
		if(!side[1]->MakeRoot(error))
			return false;
		dst_exists = true;
	}

	Vector<Entry> se, de;
	String sh, dh;
	report.WhenLine("scanning source " + src);
	if(!side[0]->Scan(db, flags, report, se, sh, result.source, error))
		return error == "stopped" ? (result.stopped = true) : false;
	report.WhenLine(Format("source: %d entries, %d hashed (%s), %d unchanged since last scan",
	                       result.source.entries, result.source.hashed,
	                       FormatBytes(result.source.hashed_bytes), result.source.reused));
	if(dst_exists) {
		report.WhenLine("scanning target " + dst);
		if(!side[1]->Scan(db, flags, report, de, dh, result.target, error))
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
		if(flags.safe_delete)
			report.WhenLine("deleted and replaced files would move to " + r.old_roots[1] +
			                (flags.mode == SyncMode::Merge ? " and " + r.old_roots[0] : String()));
		report.WhenLine(Format("dry run: %d actions, nothing changed", plan.GetCount()));
		return true;
	}

	// Record the whole plan before doing any of it.
	db.Exec("BEGIN");
	Stmt(db, "INSERT INTO runs (started, source, target, args, state) VALUES (?, ?, ?, ?, 'running')")
		.Int(1, GetUTCSeconds(GetSysTime())).Text(2, src).Text(3, dst).Text(4, key).Run();
	r.run = result.run = db.LastId();
	Stmt ins(db, "INSERT INTO actions (run, seq, kind, dir, rel, type, link, link_to, size, sha256, dev, ino, "
	             "mtime_ns, ctime_ns, mode, note, state, bytes, error, replaces) "
	             "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 'planned', 0, NULL, ?)");
	for(int i = 0; i < plan.GetCount(); i++) {
		const Action& a = plan[i];
		const Entry& e = a.from;
		ins.Int(1, r.run).Int(2, i).Text(3, KindName(a.kind)).Int(4, a.dir).Text(5, e.rel)
		   .Text(6, String(e.type, 1)).Text(7, e.link).Text(8, a.link_to).Int(9, e.size)
		   .Text(10, e.hash).Int(11, e.dev).Int(12, e.ino).Int(13, e.mtime_ns)
		   .Int(14, e.ctime_ns).Int(15, e.mode).Text(16, a.note).Int(17, a.replaces);
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
