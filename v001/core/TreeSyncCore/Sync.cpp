// Sync run: resume an unfinished run, or scan, plan and record a new one; then
// carry out the recorded actions (file copies on up to --threads threads). Each action is marked in_progress
// before it starts and done (or failed) after, so a stopped run resumes exactly.
// Every copy goes to a .treesync-part file, is read back and checked against the
// source's SHA-256, and only then renamed into place.
//
// Each side is a TreeFs (local or SFTP). Local to local, small files go through
// /usr/bin/cp; everything else uses the chunked copy, each chunk recorded, so
// remote copies resume part-way through a file just like big local ones.
#include "TreeSyncCore.h"

#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

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

// What one worker thread works with: both sides. A remote side gets the worker's
// own SFTP connection (a libssh2 session must not be shared between threads);
// local sides are shared, they keep no state between calls.
struct Ctx {
	TreeFs      *fs[2] = {};
	One<TreeFs>  own[2];
};

struct Runner {
	Db&               db;
	SyncReport&       report;
	SyncResult&       result;
	const SyncSetup&  setup;
	const SyncFlags&  flags;
	Ctx               main;          // the sides RunSync made; the ordered phases use these
	String            ids[2];
	int64             run = 0;
	int               copy_count = 0;
	std::atomic<int>  copy_index = 0;
	Clock::time_point started = Clock::now();
	String            old_roots[2];  // --safe-delete: where each side's deleted/replaced files go
	IdTable           id_tables[2];
	bool              ids_loaded = false;
	bool              owner_note[2] = {};
	Index<String>     unknown_names;

	// One lock for what the copy threads share: the database, the report (terminal
	// or GUI), the counters and the owner tables. File data never moves under it.
	std::recursive_mutex mx;
	using Lock = std::lock_guard<std::recursive_mutex>;

	Runner(Db& db, SyncReport& report, SyncResult& result, const SyncSetup& setup)
	:	db(db), report(report), result(result), setup(setup), flags(setup.flags) {}

	// Overall progress: finished files plus how far the ones in flight have got.
	int64                   overall_total = 0, overall_finished = 0;
	VectorMap<int, int64>   in_flight;   // copy id -> bytes done

	void Line(const String& s)              { Lock l(mx); report.WhenLine(s); }

	void Progress(const CopyProgress& p)
	{
		Lock l(mx);
		report.WhenProgress(p);
		if(p.done >= p.size) {
			in_flight.RemoveKey(p.id);
			overall_finished += p.size;
		}
		else
			in_flight.GetAdd(p.id) = p.done;
		int64 done = overall_finished;
		for(int64 d : in_flight)
			done += d;
		report.WhenOverall(min(done, overall_total), overall_total);
	}

	void SetState(int seq, const char *state, int64 bytes = 0, const String& error = Null)
	{
		Lock l(mx);
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
		Lock l(mx);
		Stmt(db, "INSERT OR REPLACE INTO files VALUES (?, ?, 'f', ?, ?, ?, ?, ?, ?, ?, '', ?, 0)")
			.Text(1, ids[side]).Text(2, rel).Int(3, e.dev).Int(4, e.ino).Int(5, e.nlink)
			.Int(6, e.size).Int(7, e.mtime_ns).Int(8, e.ctime_ns).Int(9, e.mode).Text(10, hash).Run();
	}

	// --preserve-owner, and the side being written can take owners (root there).
	// uid/gid: the owner to copy. Without root, a local copy of the user's own file
	// gets the right owner anyway: no note for that.
	bool Owners(Ctx& c, int side, int64 uid, int64 gid)
	{
		if(!flags.preserve_owner)
			return false;
		Lock l(mx);
		if(!c.fs[side]->CanChown()) {
			if(c.fs[side]->IsLocal() && c.fs[1 - side]->IsLocal() && uid == (int64)getuid() && gid == (int64)getgid())
				return false;
			if(!owner_note[side])
				report.WhenLine("owners not copied to " + c.fs[side]->Name() +
				                ": that needs root there (--no-preserve-owner hides this note)");
			owner_note[side] = true;
			return false;
		}
		if(!ids_loaded && !flags.numeric_ids) {
			for(int i = 0; i < 2; i++) {
				String error;
				if(!c.fs[i]->LoadIds(id_tables[i], error))
					report.WhenLine("warning: " + error + "; owners copied by number");
			}
			ids_loaded = true;
		}
		return true;
	}

	// Owner and group from side `from`, as the numbers the other side uses for the same names.
	void MapOwner(Ctx& c, int from, int64 uid, int64 gid, FileAttrs& fa)
	{
		Lock l(mx);
		for(int group = 0; group < 2; group++) {
			String unknown;
			int64 id = MapId(id_tables[from], id_tables[1 - from], group, group ? gid : uid,
			                 flags.numeric_ids || !ids_loaded, &unknown);
			(group ? fa.gid : fa.uid) = id;
			if(unknown.GetCount() && unknown_names.Find(unknown) < 0) {
				unknown_names.Add(unknown);
				report.WhenLine(Format("warning: %s '%s' does not exist on %s; kept its number %d",
				                       group ? "group" : "user", unknown, c.fs[1 - from]->Name(), id));
			}
		}
	}

	// Sets the owner of something just created (folder or local symlink) from the source.
	void CopyOwner(Ctx& c, int dir, const String& rel)
	{
		Entry src;
		String error;
		if(!flags.preserve_owner || !c.fs[dir]->Stat(rel, false, src, error) || src.uid < 0 ||
		   !Owners(c, 1 - dir, src.uid, src.gid))
			return;
		FileAttrs fa;
		MapOwner(c, dir, src.uid, src.gid, fa);
		if(!c.fs[1 - dir]->Chown(rel, fa.uid, fa.gid, error))
			Line("warning: " + error);
	}

	// --safe-delete: move what is at rel on side `side` out to its .old tree.
	bool MoveOut(Ctx& c, int side, const String& rel, String& error)
	{
		String moved;
		if(!c.fs[side]->MoveOut(rel, old_roots[side] + "/" + rel, moved, error))
			return false;
		if(moved.GetCount())
			Line("moved " + rel + " to " + moved);
		return true;
	}

	// Makes room at rel for a new version: the old one moves out if --safe-delete.
	bool MakeRoom(Ctx& c, const Action& a, String& error)
	{
		return !(a.replaces && flags.safe_delete) || MoveOut(c, 1 - a.dir, a.from.rel, error);
	}

	void Synced(const String& rel, const String& hash)
	{
		Lock l(mx);
		Stmt(db, "INSERT OR REPLACE INTO synced VALUES (?, ?, ?, ?)")
			.Text(1, ids[0]).Text(2, ids[1]).Text(3, rel).Text(4, hash).Run();
	}

	// Chunked copy, a pipeline per chunk: read and hash it, write it, read it back and
	// check it. Every checked chunk is recorded, so a restart continues from the next
	// one. Works in 1 MiB slices (little memory per thread). src_hash and dst_hash are
	// the whole-file SHA-256 of what was read and of what was read back, built up
	// along the way: no separate full read is needed to verify the copy.
	// Returns false with error "stopped" on Stop.
	bool ChunkCopy(int seq, TreeFs& from, TreeFs& to, const String& rel, const String& part,
	               int64 size, CopyProgress& p, Clock::time_point t0, One<TreeFile>& out,
	               String& src_hash, String& dst_hash, String& error)
	{
		int64 chunk = (int64)flags.chunk_mib << 20;
		One<TreeFile> in = from.Open(rel, false, error);
		if(!in)
			return false;
		out = to.Open(part, true, error);
		if(!out)
			return false;
		const int64 SLICE = 1 << 20;
		Buffer<byte> buf((size_t)SLICE);
		bool count_read = !from.IsLocal();
		bool count_write = !to.IsLocal() || from.IsLocal();   // --bwlimit: network, or local writes
		bool count_back = !to.IsLocal();

		// SHA-256 of [off, off + len) of a file, in slices; feeds `also` too.
		auto hash_range = [&](TreeFile& f, int64 off, int64 len, bool count, Sha256Stream *also,
		                      String& hash) -> bool {
			Sha256Stream sha;
			for(int64 k = 0; k < len; k += SLICE) {
				int64 n = min(SLICE, len - k);
				if(!f.ReadAt(off + k, buf, n, error))
					return false;
				if(count)
					report.Count(n);
				sha.Put(buf, (int)n);
				if(also)
					also->Put(buf, (int)n);
			}
			hash = sha.FinishString();
			return true;
		};

		// Chunks already done in an earlier, stopped attempt.
		Vector<String> done;
		{
			Lock l(mx);
			Stmt q(db, "SELECT idx, sha256 FROM chunks WHERE run = ? AND seq = ? ORDER BY idx");
			q.Int(1, run).Int(2, seq);
			while(q.Step() && q.Int(0) == done.GetCount())
				done.Add(q.Text(1));
		}
		int64 count = (size + chunk - 1) / chunk;
		int next = done.GetCount();
		Sha256Stream src_sha, dst_sha;
		if(next > 0) {
			// The last recorded chunk may have been torn by the stop: check it again.
			int64 off = (next - 1) * chunk, len = min(chunk, size - off);
			String h, ignore = error;
			if(!hash_range(*out, off, len, count_back, nullptr, h) || h != done.Top())
				next--;
			error = ignore;
			{
				Lock l(mx);
				Stmt(db, "DELETE FROM chunks WHERE run = ? AND seq = ? AND idx >= ?")
					.Int(1, run).Int(2, seq).Int(3, next).Run();
			}
			Line(Format("resuming %s at chunk %d of %d", rel, next + 1, count));
			// The running hashes cover the chunks already there: those bytes were
			// checked equal when they were recorded, so the source is read once for both.
			int64 prefix = min(next * chunk, size);
			for(int64 k = 0; k < prefix; k += SLICE) {
				int64 n = min(SLICE, prefix - k);
				if(!in->ReadAt(k, buf, n, error))
					return false;
				if(count_read)
					report.Count(n);
				src_sha.Put(buf, (int)n);
				dst_sha.Put(buf, (int)n);
			}
		}

		Clock::time_point last_shown = Clock::now();
		for(int64 idx = next; idx < count; idx++) {
			if(report.Stopping()) {
				error = "stopped";
				return false;
			}
			int64 off = idx * chunk, len = min(chunk, size - off);
			// 1. read and hash, 2. write, slice by slice
			Sha256Stream chunk_sha;
			for(int64 k = 0; k < len; k += SLICE) {
				int64 n = min(SLICE, len - k);
				if(!in->ReadAt(off + k, buf, n, error))
					return false;
				if(count_read)
					report.Count(n);
				chunk_sha.Put(buf, (int)n);
				src_sha.Put(buf, (int)n);
				if(!out->WriteAt(off + k, buf, n, error))
					return false;
				if(count_write)
					report.Count(n);
				// Within a chunk too, about four times a second, so a thermometer
				// moves smoothly even when one chunk takes many seconds.
				if(Since(last_shown) >= 0.25 && off + k + n < size) {
					p.done = off + k + n;
					p.seconds = Since(t0);
					Progress(p);
					last_shown = Clock::now();
				}
			}
			if(report.Stopping()) {
				error = "stopped";
				return false;
			}
			// A recorded chunk must really be on disk. A one-chunk file needs no flush:
			// if it is cut short it is copied again, and a later scan catches a copy
			// lost in a crash after its rename.
			if(count > 1)
				out->Flush();
			// 3. read the chunk back and check it against what was read
			String sent = chunk_sha.FinishString(), back;
			if(!hash_range(*out, off, len, count_back, &dst_sha, back))
				return false;
			if(back != sent) {
				error = Format("verify failed: chunk %d of %s reads back differently; kept as %s", idx + 1, rel, part);
				return false;
			}
			{
				Lock l(mx);
				Stmt(db, "INSERT OR REPLACE INTO chunks VALUES (?, ?, ?, ?, ?, ?)")
					.Int(1, run).Int(2, seq).Int(3, idx).Int(4, off).Int(5, len).Text(6, sent).Run();
				result.bytes += len;
			}
			p.done = off + len;
			p.seconds = Since(t0);
			if(p.done < p.size)
				Progress(p);
		}
		if(!out->Truncate(size, error))
			return false;
		src_hash = src_sha.FinishString();
		dst_hash = dst_sha.FinishString();
		return true;
	}

	// Copies (or links, or re-creates a symlink) a.from.rel, verified, into place.
	bool Copy(Ctx& c, const Step& s, String& error)
	{
		const Action& a = s.a;
		const Entry& e = a.from;
		TreeFs& from = *c.fs[a.dir];
		TreeFs& to = *c.fs[1 - a.dir];
		String part = PartRel(e.rel, run, s.seq);

		if(a.kind == ActionKind::Link) {
			if(to.CanHardLink()) {
				if(!to.HardLink(a.link_to, part, error) || !MakeRoom(c, a, error) || !to.Rename(part, e.rel, error))
					return false;
				copy_index++;
				Line("linked " + e.rel + " -> " + a.link_to);
				// A new link changes the inode's ctime: refresh the cache for both names.
				Cache(to, 1 - a.dir, e.rel, e.hash);
				Cache(to, 1 - a.dir, a.link_to, e.hash);
				Synced(e.rel, e.hash);
				return true;
			}
			Line("hard link written out as a copy (the target cannot hard-link): " + e.rel);
		}
		if(e.type == 'l') {
			if(!to.Symlink(part, e.link, error) || !MakeRoom(c, a, error) || !to.Rename(part, e.rel, error))
				return false;
			Line("symlink " + e.rel + " -> " + e.link);
			if(to.IsLocal())   // SFTP cannot set a symlink's own owner
				CopyOwner(c, a.dir, e.rel);
			return true;
		}

		// The source may have changed since the run was planned: then hash it again.
		Entry now;
		if(!from.Stat(e.rel, flags.follow_links, now, error))
			return false;
		String expect = e.hash;
		if(now.size != e.size || now.mtime_ns != e.mtime_ns || (from.IsLocal() && now.ino != e.ino)) {
			Line("source changed since planning, hashing again: " + e.rel);
			if(!from.Hash(e.rel, flags.follow_links, expect, error, report.stop))
				return false;
		}

		// A file not hashed yet (the streaming engine hashes on the way): the cp path
		// needs it first; the chunked copy works it out while copying.
		bool hash_new = expect.IsEmpty();
		bool big_or_remote = now.size >= ((int64)flags.big_file_mib << 20) || !from.IsLocal() || !to.IsLocal() ||
		                     flags.bwlimit_kib;
		if(hash_new && !big_or_remote && !from.Hash(e.rel, flags.follow_links, expect, error, report.stop))
			return false;

		CopyProgress p;
		p.id = s.seq;
		p.index = ++copy_index;
		p.count = copy_count;
		p.name = e.rel;
		p.size = now.size;
		Clock::time_point t0 = Clock::now();
		bool big = now.size >= ((int64)flags.big_file_mib << 20);
		bool fast = from.IsLocal() && to.IsLocal() && !big && !flags.bwlimit_kib;   // cp cannot be throttled
		bool ok = false;
		One<TreeFile> out;
		String src_hash, dst_hash;
		for(int attempt = 0; attempt <= flags.retries; attempt++) {
			if(attempt) {
				Line(Format("retry %d of %d in %d s: %s (%s)", attempt, flags.retries,
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
				ok = ChunkCopy(s.seq, from, to, e.rel, part, now.size, p, t0, out, src_hash, dst_hash, error);
			if(ok || error == "stopped" || report.Stopping() || Permanent(error))
				break;
		}
		if(!ok)
			return false;
		if(fast) {
			Lock l(mx);
			result.bytes += now.size;
		}

		// The copy must have the source's SHA-256. After cp, read it back; the chunked
		// copy already read every chunk back and built both hashes on the way.
		String got;
		if(fast) {
			if(!to.Hash(part, false, got, error, report.stop))
				return false;
		}
		else {
			got = dst_hash;
			if(expect.IsEmpty())
				expect = src_hash;
			else if(src_hash != expect) {
				error = "the source changed while it was being copied: " + e.rel;
				return false;
			}
		}
		if(got != expect) {
			error = "verify failed: copy of " + e.rel + " has a different SHA-256; kept as " + part;
			return false;
		}
		if(!fast) {   // cp --preserve already did this
			FileAttrs fa;
			fa.mode = now.mode;
			fa.mtime_ns = now.mtime_ns;
			if(now.uid >= 0 && Owners(c, 1 - a.dir, now.uid, now.gid))
				MapOwner(c, a.dir, now.uid, now.gid, fa);
			String warn;
			if(!out->SetAttrs(fa, warn))
				Line("warning: " + warn);
			out.Clear();   // close before the rename
		}
		if(!MakeRoom(c, a, error) || !to.Rename(part, e.rel, error))
			return false;
		p.done = p.size;
		p.seconds = Since(t0);
		Progress(p);
		Synced(e.rel, expect);
		if(hash_new)   // the source's own hash, worked out here: keep it for the next scan
			Cache(from, a.dir, e.rel, expect, &now);
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

	bool Mkdir(Ctx& c, const Action& a, String& error)
	{
		if(!c.fs[1 - a.dir]->Mkdir(a.from.rel, a.from.mode, error))
			return false;
		Line("created folder " + a.from.rel);
		CopyOwner(c, a.dir, a.from.rel);
		return true;
	}

	bool Delete(Ctx& c, const Action& a, String& error)
	{
		// Folders are emptied first (deletes run deepest first) and then removed;
		// with --safe-delete their files have already moved out.
		if(flags.safe_delete && a.from.type != 'd')
			return MoveOut(c, 1 - a.dir, a.from.rel, error);
		int r = c.fs[1 - a.dir]->Remove(a.from.rel, a.from.type == 'd', error);
		if(r < 0)
			return false;
		Line(r ? "kept folder " + a.from.rel + ": it still holds excluded files"
		       : "deleted " + a.from.rel);
		return true;
	}

	// Records a step as started, does it, records how it went. False when stopped.
	bool RunStep(Ctx& c, const Step& s)
	{
		if(report.Stopping())
			return false;
		SetState(s.seq, "in_progress");
		String error;
		bool ok = true;
		switch(s.a.kind) {
		case ActionKind::Mkdir:  ok = Mkdir(c, s.a, error); break;
		case ActionKind::Copy:
		case ActionKind::Link:   ok = Copy(c, s, error); break;
		case ActionKind::Delete: ok = Delete(c, s.a, error); break;
		case ActionKind::Conflict: {
			Lock l(mx);
			result.conflicts++;
			report.WhenLine("CONFLICT " + s.a.from.rel + ": " + s.a.note);
			break;
		}
		}
		if(!ok && (error == "stopped" || report.Stopping())) {
			SetState(s.seq, "planned");   // not failed: redo it on resume
			return false;
		}
		SetState(s.seq, ok ? "done" : "failed", 0, ok ? String() : error);
		Lock l(mx);
		if(ok)
			result.done++;
		else {
			result.failed++;
			report.WhenLine("FAILED " + s.a.from.rel + ": " + error);
		}
		return true;
	}

	// A worker's own connections for remote sides; local sides are shared.
	bool Connect(Ctx& c, String& error)
	{
		for(int i = 0; i < 2; i++) {
			if(main.fs[i]->IsLocal()) {
				c.fs[i] = main.fs[i];
				continue;
			}
			SftpFs& s = c.own[i].Create<SftpFs>(i ? setup.target : setup.source, setup.known_hosts, flags.compress);
			if(!s.Connect(error))
				return false;
			c.fs[i] = &s;
		}
		return true;
	}

	// File copies, on up to --threads threads. False when stopped.
	bool CopyInParallel(const Vector<const Step*>& todo)
	{
		int n = clamp(flags.threads, 1, max(todo.GetCount(), 1));
		Array<Ctx> extra;
		for(int t = 1; t < n; t++) {
			String error;
			Ctx& c = extra.Add();
			if(!Connect(c, error)) {
				extra.Drop();
				Line("warning: copying with fewer threads, a connection failed: " + error);
				break;
			}
		}
		if(extra.GetCount())
			Line(Format("copying with %d threads", extra.GetCount() + 1));

		std::atomic<int> next = 0;
		std::atomic<bool> halted = false;
		auto work = [&](Ctx& c) {
			for(int i; !halted && (i = next++) < todo.GetCount();)
				if(!RunStep(c, *todo[i]))
					halted = true;
		};
		std::vector<std::thread> threads;
		for(Ctx& c : extra)
			threads.emplace_back(work, std::ref(c));
		work(main);
		for(std::thread& t : threads)
			t.join();
		return !halted;
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

		// Four phases, in plan order within each: folders (parents first) and
		// conflicts; file copies, in parallel; hard links, which need their first
		// copy; deletes, deepest first.
		Vector<const Step*> phase[4];
		for(const Step& s : steps) {
			// Links count too: on a target that cannot hard-link they are copied.
			bool counts = (s.a.kind == ActionKind::Copy || s.a.kind == ActionKind::Link) && s.a.from.type == 'f';
			copy_count += counts;
			if(s.state == "done") {
				result.done++;
				copy_index += counts;
				continue;
			}
			int p = s.a.kind == ActionKind::Copy ? 1 : s.a.kind == ActionKind::Link ? 2
			      : s.a.kind == ActionKind::Delete ? 3 : 0;
			phase[p].Add(&s);
		}
		for(int p = 1; p <= 2; p++)
			for(const Step *s : phase[p])
				if(s->a.from.type == 'f')
					overall_total += s->a.from.size;
		bool going = true;
		for(int p = 0; p < 4 && going; p++)
			if(p == 1)
				going = CopyInParallel(phase[1]);
			else
				for(const Step *s : phase[p])
					if(!(going = RunStep(main, *s)))
						break;
		result.stopped = !going;
		if(going)   // hard links copy no bytes: the run is complete all the same
			report.WhenOverall(overall_total, overall_total);

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
	// --low-priority: CPU nice 19 and I/O best-effort level 7 for this process and
	// the cp processes it starts. Not the idle I/O class: with other copying going on
	// that could starve the sync completely. The remote server's priority is its own.
	if(flags.low_priority) {
		setpriority(PRIO_PROCESS, 0, 19);
		const int IOPRIO_CLASS_BE = 2, IOPRIO_WHO_PROCESS = 1, IOPRIO_CLASS_SHIFT = 13;
		syscall(SYS_ioprio_set, IOPRIO_WHO_PROCESS, 0, (IOPRIO_CLASS_BE << IOPRIO_CLASS_SHIFT) | 7);
		report.WhenLine("running at low priority (nice 19, I/O best-effort 7)");
	}
	// --bwlimit: the throttle lives for this run only.
	Throttle throttle((int64)flags.bwlimit_kib * 1024, report.stop);
	struct Unset { SyncReport& r; ~Unset() { r.throttle = nullptr; } } unset{ report };
	if(flags.bwlimit_kib) {
		report.throttle = &throttle;
		report.WhenLine(Format("bandwidth limit %s/s", FormatBytes((int64)flags.bwlimit_kib * 1024)));
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

	Runner r(db, report, result, setup);
	for(int i = 0; i < 2; i++) {
		r.main.fs[i] = ~side[i];
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
