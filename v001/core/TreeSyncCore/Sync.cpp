// Sync run: resume an unfinished run, or scan, plan and record a new one; then
// carry out the recorded actions (file copies on up to --threads threads). Each action is marked in_progress
// before it starts and done (or failed) after, so a stopped run resumes exactly.
// Every copy goes to a .treesync-part file, is read back and checked against the
// source's SHA-256, and only then renamed into place.
//
// Each side is a TreeFs (local or SFTP). Local to local, small files go through
// /usr/bin/cp; everything else uses the chunked copy, each chunk recorded, so
// remote copies resume part-way through a file just like big local ones.
#include "Runner.h"

#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

using namespace TsRun;

namespace TsRun {

// Makes one side: a LocalFs, or an SftpFs that is connected.
One<TreeFs> MakeSide(const TreeEndpoint& e, const SyncSetup& setup, SyncReport& report, String& error)
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

	// Copy and Mirror stream: pre-scan, copy and post-scan at the same time. Merge
	// needs both sides complete to tell which side changed: it plans first.
	if(flags.mode != SyncMode::Merge) {
		TreeFs *sides[2] = { ~side[0], ~side[1] };
		return RunStreaming(r, sides, key, setup.fresh, error);
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
		.Int(1, (int64)time(nullptr)).Text(2, src).Text(3, dst).Text(4, key).Run();
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
