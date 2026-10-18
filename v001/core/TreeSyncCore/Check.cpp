// Checks that change no content: Scan only (--scan-only) reads and hashes every file
// on both sides afresh and compares the attributes; Sync attributes (--sync-attrs)
// compares the attributes only and sets on the target just those that differ.
// Every file and folder gets a row in file_checks (views: file_compare,
// hash_mismatches, attr_mismatches); copies of a sync run write the same rows.
// Also --clear-old, which deletes the --safe-delete folders for good.
#include "Runner.h"

#include <thread>
#include <vector>

using namespace TsRun;

int64 FolderIds::Get(const String& rel)
{
	int i = ids.Find(rel);
	if(i >= 0)
		return ids[i];
	Stmt(db, "INSERT OR IGNORE INTO folders (rel) VALUES (?)").Text(1, rel).Run();
	Stmt q(db, "SELECT id FROM folders WHERE rel = ?");
	q.Text(1, rel);
	int64 id = q.Step() ? q.Int(0) : 0;
	ids.Add(rel, id);
	return id;
}

CheckWriter::CheckWriter(Db& db)
:	folders(db),
	insert(db, "INSERT OR REPLACE INTO file_checks VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)")
{}

void CheckWriter::Write(int64 run, const CheckRow& r)
{
	int slash = r.rel.ReverseFind('/');
	Stmt& w = insert;
	w.Int(1, run).Int(2, r.seq).Int(3, folders.Get(slash < 0 ? String() : r.rel.Left(slash)))
	 .Text(4, r.rel.Mid(slash + 1)).Text(5, r.kind).Text(6, String(r.type, 1));
	// Columns 7..20 in pairs, source then target; a side that is not there stays NULL.
	for(int i = 0; i < 2; i++) {
		if(r.side[i] && r.sha[i].GetCount())
			w.Text(7 + i, r.sha[i]);
		if(!r.side[i])
			continue;
		w.Int(9 + i, r.size[i]).Int(11 + i, r.mode[i]).Int(17 + i, r.mtime_ns[i]);
		if(r.uid[i] >= 0)
			w.Int(13 + i, r.uid[i]).Int(15 + i, r.gid[i]);
		if(r.xattrs_known[i])
			w.Text(19 + i, r.xattrs[i]);
	}
	w.Text(21, r.attr_diff).Text(22, r.result);
	w.Run();
}

static String TimeText(int64 ns)   // local time, to the nanosecond
{
	time_t t = (time_t)(ns / 1000000000);
	struct tm lt;
	localtime_r(&t, &lt);
	return Format("%04d-%02d-%02d %02d:%02d:%02d.%09d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
	              lt.tm_hour, lt.tm_min, lt.tm_sec, (int)(ns % 1000000000));
}

String AttrDiff(const CheckRow& r, int64 want_uid, int64 want_gid, bool exact_time)
{
	if(!r.side[0] || !r.side[1])
		return String();
	Vector<String> d;
	if(r.mode[0] != r.mode[1])
		d.Add(Format("mode %04o -> %04o", r.mode[1], r.mode[0]));
	if(r.type == 'f' && (exact_time ? r.mtime_ns[0] != r.mtime_ns[1]
	                                 : r.mtime_ns[0] / 1000000000 != r.mtime_ns[1] / 1000000000))
		d.Add("time " + TimeText(r.mtime_ns[1]) + " -> " + TimeText(r.mtime_ns[0]));
	if(want_uid >= 0 && r.uid[1] >= 0 && (want_uid != r.uid[1] || want_gid != r.gid[1]))
		d.Add(Format("owner %d:%d -> %d:%d", r.uid[1], r.gid[1], want_uid, want_gid));
	if(r.xattrs_known[0] && r.xattrs_known[1] && r.xattrs[0] != r.xattrs[1])
		d.Add("extended attributes");
	return Join(d, ", ");
}

namespace {

struct Item : Moveable<Item> {
	String      rel;
	FolderEntry s, t;
	bool        on_target = false;
};

// The two sides and the bookkeeping one check run shares between its threads.
struct Checker {
	const SyncSetup& setup;
	const SyncFlags& flags;
	SyncReport&      report;
	CheckResult&     result;
	CheckMode        mode;
	Db&              db;
	CheckWriter      checks;
	TreeFs          *fs[2] = {};
	bool             local = false;          // both sides local: threads, xattrs, exact times
	bool             owners = false;         // compare owners (--preserve-owner)
	IdTable          ids[2];
	std::mutex       m;
	int              seq = 0, pending = 0;
	int64            total = 0, done = 0;    // bytes to hash, hashed
	std::atomic<int> index = 0;
	int              file_total = 0;

	Checker(const SyncSetup& setup, CheckMode mode, SyncReport& report, CheckResult& result, Db& db)
	:	setup(setup), flags(setup.flags), report(report), result(result), mode(mode), db(db), checks(db) {}

	// Source and target entries of every folder, filtered like a sync.
	void Walk(Vector<Item>& files, Vector<Item>& dirs)
	{
		NameFilter filter(flags);
		struct Dir : Moveable<Dir> { String rel; bool inside = false; };
		Vector<Dir> stack;
		stack.Add().inside = filter.RootInside();
		while(stack.GetCount() && !report.Stopping()) {
			Dir d = stack.Pop();
			Vector<FolderEntry> se, te;
			String error;
			if(!fs[0]->List(d.rel, se, error)) {
				report.WhenLine("warning: " + error);
				continue;
			}
			fs[1]->List(d.rel, te, error);   // a folder missing on the target: no entries
			VectorMap<String, int> tix;
			for(int i = 0; i < te.GetCount(); i++)
				tix.Add(te[i].name, i);
			for(const FolderEntry& s : se) {
				if(SkipName(s.name))
					continue;
				String rel = d.rel.IsEmpty() ? s.name : d.rel + "/" + s.name;
				bool inside = d.inside;
				if(s.type == 'd') {
					if(filter.SkipDir(rel))
						continue;
					inside = filter.Inside(d.inside, rel);
					Dir& sub = stack.Add();
					sub.rel = rel;
					sub.inside = inside;
					if(!inside || !filter.Checked(rel))   // a folder only on the way to checked ones
						continue;
				}
				else if(s.type != 'f' || !filter.KeepFile(d.inside, rel))
					continue;   // symlinks and special files are not checked
				Item it;
				it.rel = rel;
				it.s = s;
				int k = tix.Find(s.name);
				if(k >= 0 && te[k].type == s.type) {
					it.t = te[k];
					it.on_target = true;
				}
				if(s.type == 'd')
					dirs.Add(pick(it));
				else {
					total += s.size * (it.on_target && mode == CheckMode::Scan ? 2 : 1);
					files.Add(pick(it));
				}
			}
			// The pre-scan box: folders listed of the folders found so far.
			listed++;
			StageProgress st;
			st.stage = 0;
			st.done = listed;
			st.total = listed + stack.GetCount();
			st.detail = d.rel;
			report.WhenStage(st);
		}
		StageProgress st;
		st.stage = 0;
		st.done = st.total = listed;
		st.finished = true;
		report.WhenStage(st);
	}

	// The post-scan box: folders whose files have all been checked.
	VectorMap<String, int> waiting;   // folder -> its files not checked yet
	int64            folders_done = 0, folders_total = 0;
	int              listed = 0;

	static String FolderOf(const String& rel)   { return rel.Left(max(rel.ReverseFind('/'), 0)); }

	void FoldersStart(const Vector<Item>& files, const Vector<Item>& dirs)
	{
		for(const Item& f : files)
			waiting.GetAdd(FolderOf(f.rel), 0)++;
		folders_total = dirs.GetCount() + 1;   // and the top
		folders_done = folders_total - waiting.GetCount();
		FoldersShow(false);
	}

	void FolderStep(const String& rel)   // under m: one more file of rel's folder checked
	{
		int i = waiting.Find(FolderOf(rel));
		if(i >= 0 && --waiting[i] == 0) {
			folders_done++;
			FoldersShow(false);
		}
	}

	void FoldersShow(bool finished)
	{
		StageProgress st;
		st.stage = 2;
		st.done = folders_done;
		st.total = folders_total;
		st.finished = finished;
		report.WhenStage(st);
	}

	// The source's owner as the target's numbers; -1 when owners are not compared.
	void WantOwner(const CheckRow& r, int64& uid, int64& gid)
	{
		uid = gid = -1;
		if(!owners || r.uid[0] < 0)
			return;
		uid = MapId(ids[0], ids[1], false, r.uid[0], flags.numeric_ids, nullptr);
		gid = MapId(ids[0], ids[1], true, r.gid[0], flags.numeric_ids, nullptr);
	}

	CheckRow Row(const Item& it, char type)
	{
		CheckRow r;
		r.rel = it.rel;
		r.kind = mode == CheckMode::Scan ? "scan" : "attrs";
		r.type = type;
		for(int i = 0; i < 2; i++) {
			const FolderEntry& e = i ? it.t : it.s;
			r.side[i] = i == 0 || it.on_target;
			if(!r.side[i])
				continue;
			r.size[i] = e.size;
			r.mode[i] = e.mode & 07777;
			r.uid[i] = e.uid;
			r.gid[i] = e.gid;
			r.mtime_ns[i] = e.mtime_ns;
			if(local) {
				String ignore;
				if(One<TreeFile> f = fs[i]->Open(it.rel, false, ignore)) {
					r.xattrs[i] = XattrDigest(*f);
					r.xattrs_known[i] = true;
				}
			}
		}
		int64 uid, gid;
		WantOwner(r, uid, gid);
		r.attr_diff = AttrDiff(r, uid, gid, local);
		return r;
	}

	void Write(CheckRow& r)   // under m
	{
		r.seq = seq++;
		checks.Write(result.run, r);
		if(++pending >= 500) {
			db.Exec("COMMIT; BEGIN");
			pending = 0;
		}
	}

	// One file: hash both sides (Scan), compare, record. ctx: this thread's sides.
	// n: the file's place in the list, its id in the progress shown.
	void File(TreeFs *side[2], const Item& it, int n)
	{
		CheckRow r = Row(it, 'f');
		String error;
		// Scan only: shown while it is hashed (the GUI's list, the terminal's status line),
		// then one line with what it found. It reads both sides; it copies nothing.
		CopyProgress p;
		p.id = n;
		p.check = true;
		p.name = it.rel;
		p.size = r.size[0] * (r.side[1] ? 2 : 1);   // bytes to read
		p.count = file_total;
		auto t0 = std::chrono::steady_clock::now();
		if(mode == CheckMode::Scan && p.size > 0) {
			std::lock_guard<std::mutex> l(m);
			p.index = result.files + 1;
			report.WhenProgress(p);
		}
		if(mode == CheckMode::Scan)
			for(int i = 0; i < 2 && r.side[i] && !report.Stopping(); i++)
				if(!side[i]->Hash(it.rel, false, r.sha[i], error, report.stop)) {
					if(error != "stopped")
						r.result = "failed: " + error;
					r.sha[i].Clear();
				}
		if(report.Stopping())
			return;
		std::lock_guard<std::mutex> l(m);
		Write(r);
		result.files++;
		if(mode == CheckMode::Scan) {
			done += r.size[0] * (r.side[1] ? 2 : 1);
			result.bytes += r.size[0] * (r.side[1] ? 2 : 1);
			report.WhenOverall(min(done, total), total);
			if(r.result.GetCount()) {
				result.failed++;
				report.WhenLine("FAILED " + it.rel + ": " + r.result.Mid(8));
			}
			else if(!r.side[1])
				result.missing++;
			else if(r.sha[0] == r.sha[1])
				result.same++;
			else
				result.differ++;
		}
		else if(!r.side[1])
			result.missing++;
		if(r.attr_diff.GetCount())
			result.attr_diffs++;
		FolderStep(it.rel);
		if(mode == CheckMode::Scan) {
			p.index = result.files;
			p.done = p.size;
			p.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
			p.state = r.result.GetCount() ? "failed" : !r.side[1] ? "missing" : r.sha[0] == r.sha[1] ? "same" : "DIFFERS";
			report.WhenProgress(p);
		}
	}

	// Sets what differs on the target, row by row, as the SQL finds it.
	void Fix()
	{
		struct Want : Moveable<Want> { int seq; String rel, diff; char type; int mode; int64 uid, gid, mtime, dst_mtime;
		                               bool xattrs; };
		Vector<Want> todo;
		{
			Stmt q(db, "SELECT c.seq, f.rel, c.name, c.type, c.attr_diff, c.src_mode, c.src_uid, c.src_gid, "
			           "c.src_mtime_ns, c.dst_mtime_ns, coalesce(c.src_xattrs <> c.dst_xattrs, 0) "
			           "FROM file_checks c JOIN folders f ON f.id = c.folder "
			           "WHERE c.run = ? AND c.attr_diff <> '' AND c.dst_mode IS NOT NULL "
			           "AND (c.type = 'd' OR c.src_size = c.dst_size) "
			           "ORDER BY c.type = 'd', f.rel DESC, c.name");
			q.Int(1, result.run);
			while(q.Step()) {
				Want& w = todo.Add();
				w.seq = (int)q.Int(0);
				w.rel = q.Text(1).IsEmpty() ? q.Text(2) : q.Text(1) + "/" + q.Text(2);
				w.type = *q.Text(3);
				w.diff = q.Text(4);
				w.mode = (int)q.Int(5);
				w.uid = q.Text(6).IsEmpty() ? -1 : q.Int(6);
				w.gid = q.Text(7).IsEmpty() ? -1 : q.Int(7);
				w.mtime = q.Int(8);
				w.dst_mtime = q.Int(9);
				w.xattrs = q.Int(10);
			}
		}
		{
			Stmt q(db, "SELECT count(*) FROM file_checks WHERE run = ? AND attr_diff <> '' AND type = 'f' "
			           "AND dst_size IS NOT NULL AND src_size <> dst_size");
			q.Int(1, result.run);
			if(q.Step() && q.Int(0))
				report.WhenLine(Format("%d file(s) with different attributes also differ in size: left alone, "
				                       "a sync copies them", (int)q.Int(0)));
		}
		bool chown = owners && fs[1]->CanChown();
		if(owners && !fs[1]->CanChown())
			report.WhenLine("owners not set on " + fs[1]->Name() + ": that needs root there");
		for(const Want& w : todo) {
			if(report.Stopping())
				return;
			if(flags.dry_run) {
				report.WhenLine("would set attrs " + w.rel + ": " + w.diff);
				result.fixed++;
				continue;
			}
			FileAttrs fa;
			fa.mode = w.mode;
			fa.mtime_ns = w.type == 'f' ? w.mtime : w.dst_mtime;   // a folder keeps its own time
			if(chown && w.uid >= 0) {
				fa.uid = MapId(ids[0], ids[1], false, w.uid, flags.numeric_ids, nullptr);
				fa.gid = MapId(ids[0], ids[1], true, w.gid, flags.numeric_ids, nullptr);
			}
			String error;
			bool ok = fs[1]->SetAttrs(w.rel, fa, error);
			if(ok && w.xattrs && local) {
				String ignore;
				One<TreeFile> a = fs[0]->Open(w.rel, false, ignore);
				One<TreeFile> b = a ? fs[1]->Open(w.rel, false, ignore) : One<TreeFile>();
				if(a && b)
					CopyXattrs(*a, *b);
			}
			Stmt(db, "UPDATE file_checks SET result = ? WHERE run = ? AND seq = ?")
				.Text(1, ok ? String("fixed") : "failed: " + error).Int(2, result.run).Int(3, w.seq).Run();
			if(ok) {
				result.fixed++;
				report.WhenLine("set attrs " + w.rel + ": " + w.diff);
			}
			else {
				result.failed++;
				report.WhenLine("FAILED attrs " + w.rel + ": " + error);
			}
		}
	}

	// The problems, from the views, for the log.
	void Summary()
	{
		const int SHOW = 1000;
		auto list = [&](const char *sql, const char *label) {
			Stmt q(db, sql);
			q.Int(1, result.run);
			int n = 0;
			while(q.Step())
				if(n++ < SHOW)
					report.WhenLine(String(label) + q.Text(0) + (q.Text(1).GetCount() ? ": " + q.Text(1) : String()));
			if(n > SHOW)
				report.WhenLine(Format("... and %d more (History > SHA-256 checks, or the view in the database)", n - SHOW));
		};
		if(mode == CheckMode::Scan) {
			list("SELECT CASE WHEN path = '' THEN file_name ELSE path || '/' || file_name END, problem "
			     "FROM hash_mismatches WHERE run = ? ORDER BY path, file_name", "DIFFERS ");
			list("SELECT CASE WHEN path = '' THEN file_name ELSE path || '/' || file_name END, attr_diff "
			     "FROM attr_mismatches WHERE run = ? ORDER BY path, file_name", "attrs differ ");
		}
	}
};

}

bool RunCheck(const SyncSetup& setup, CheckMode mode, SyncReport& report, CheckResult& result, String& error)
{
	result = CheckResult();
	const SyncFlags& flags = setup.flags;
	if(setup.source.path.IsEmpty() || setup.target.path.IsEmpty()) {
		error = "both --source and --target are needed";
		return false;
	}
	One<TreeFs> side[2];
	for(int i = 0; i < 2; i++) {
		side[i] = MakeSide(i ? setup.target : setup.source, setup, report, error);
		if(!side[i])
			return false;
	}
	if(!side[0]->RootExists()) {
		error = "source " + side[0]->Id() + " does not exist or is not a folder";
		return false;
	}
	if(!side[1]->RootExists()) {
		error = "target " + side[1]->Id() + " does not exist: nothing to compare with";
		return false;
	}
	Db db;
	if(!db.Open(Nvl(setup.db, DefaultDbPath()), error))
		return false;

	Checker k(setup, mode, report, result, db);
	for(int i = 0; i < 2; i++)
		k.fs[i] = ~side[i];
	k.local = side[0]->IsLocal() && side[1]->IsLocal();
	k.owners = flags.preserve_owner;
	if(k.owners && !flags.numeric_ids)
		for(int i = 0; i < 2; i++)
			if(!side[i]->LoadIds(k.ids[i], error)) {
				report.WhenLine("warning: " + error + "; owners compared by number");
				error.Clear();
			}

	SyncFlags key_flags = flags;
	key_flags.dry_run = false;
	String key = Join(FlagArgs(key_flags), " ") + (mode == CheckMode::Scan ? " --scan-only" : " --sync-attrs");
	Stmt(db, "INSERT INTO runs (started, source, target, args, state) VALUES (?, ?, ?, ?, 'running')")
		.Int(1, (int64)time(nullptr)).Text(2, side[0]->Id()).Text(3, side[1]->Id()).Text(4, key).Run();
	result.run = db.LastId();
	report.WhenLine(Format("run %d: %s %s -> %s", result.run,
	                       mode == CheckMode::Scan ? "scan only (SHA-256 of both sides, attributes)"
	                                               : "sync attributes", side[0]->Id(), side[1]->Id()));

	db.Exec("BEGIN");
	Vector<Item> files, dirs;
	k.Walk(files, dirs);
	for(const Item& d : dirs) {
		CheckRow r = k.Row(d, 'd');
		k.Write(r);
		result.folders++;
		if(r.attr_diff.GetCount())
			result.attr_diffs++;
	}
	// Files: on --threads threads when both sides are local (each reads its own files),
	// one at a time over SFTP (a connection must not be shared between threads).
	int threads = k.local ? max(1, flags.threads) : 1;
	k.file_total = files.GetCount();
	k.FoldersStart(files, dirs);
	auto work = [&] {
		TreeFs *s[2] = { k.fs[0], k.fs[1] };
		for(int i; !report.Stopping() && (i = k.index++) < files.GetCount();)
			k.File(s, files[i], i);
	};
	std::vector<std::thread> pool;
	for(int t = 1; t < threads; t++)
		pool.emplace_back(work);
	work();
	for(auto& t : pool)
		t.join();
	db.Exec("COMMIT");
	if(!report.Stopping())
		k.FoldersShow(true);

	result.stopped = report.Stopping();
	if(!result.stopped && mode == CheckMode::Attrs) {
		db.Exec("BEGIN");
		k.Fix();
		db.Exec("COMMIT");
	}
	if(!result.stopped)
		k.Summary();
	result.stopped = report.Stopping();
	Stmt(db, "UPDATE runs SET state = ?, finished = ? WHERE id = ?")
		.Text(1, result.stopped ? "stopped" : "done").Int(2, (int64)time(nullptr)).Int(3, result.run).Run();
	if(mode == CheckMode::Scan)
		report.WhenLine(Format("scan only: %d files (%d same, %d differ, %d missing on the target, %d failed), "
		                       "%d folders; %d with different attributes; %s read",
		                       result.files, result.same, result.differ, result.missing, result.failed,
		                       result.folders, result.attr_diffs, FormatBytes(result.bytes)));
	else
		report.WhenLine(Format("sync attributes: %d files and %d folders compared, %d differ, %d %s, %d failed, "
		                       "%d missing on the target", result.files, result.folders, result.attr_diffs,
		                       result.fixed, flags.dry_run ? "would be set" : "set", result.failed, result.missing));
	return true;
}

// --clear-old ----------------------------------------------------------------------

namespace {

// Deletes rel and everything under it on fs, or (dry) only counts. Never follows a
// symlink: a link is removed, not what it points to.
bool RemoveTree(TreeFs& fs, const String& rel, bool dry, SyncReport& report, int64& files, int64& bytes,
                int64& folders, String& error)
{
	Vector<String> stack, order;
	stack.Add(rel);
	while(stack.GetCount()) {
		if(report.Stopping()) {
			error = "stopped";
			return false;
		}
		String d = stack.Pop();
		order.Add(d);
		Vector<FolderEntry> list;
		if(!fs.List(d, list, error))
			return false;
		for(const FolderEntry& e : list) {
			if(e.name == "." || e.name == "..")
				continue;
			String r = d + "/" + e.name;
			if(e.type == 'd') {
				stack.Add(r);
				continue;
			}
			files++;
			bytes += e.size;
			if(!dry && fs.Remove(r, false, error) < 0)
				return false;
		}
	}
	folders += order.GetCount();
	if(!dry)
		for(int i = order.GetCount() - 1; i >= 0; i--)
			if(fs.Remove(order[i], true, error) != 0) {
				if(error.IsEmpty())
					error = "could not remove " + order[i];
				return false;
			}
	return true;
}

}

bool ClearOld(const SyncSetup& setup, bool dry, SyncReport& report, String& error)
{
	for(int i = 0; i < 2; i++) {
		const TreeEndpoint& e = i ? setup.target : setup.source;
		if(e.path.IsEmpty())
			continue;
		String root = e.remote ? e.path : CanonicalPath(e.path);
		String old = root + ".old";
		if(i == 1 && setup.safe_delete_dir.GetCount())
			old = e.remote ? setup.safe_delete_dir : CanonicalPath(setup.safe_delete_dir);
		while(old.GetCount() > 1 && old.EndsWith("/"))
			old.TrimEnd("/");
		String parent = GetFileFolder(old), name = GetFileName(old);
		// Never a tree, a folder holding one, the file system root or a home folder.
		bool bad = name.IsEmpty() || parent.IsEmpty() || old == "/" || old == HomeDir() ||
		           (!e.remote && (old == "/home" || old == "/root"));
		for(int t = 0; t < 2 && !bad; t++) {
			const TreeEndpoint& o = t ? setup.target : setup.source;
			if(o.path.GetCount() && o.remote == e.remote && o.host == e.host) {
				String other = o.remote ? o.path : CanonicalPath(o.path);
				bad = IsUnder(other, old) || IsUnder(old, other);
			}
		}
		if(bad) {
			error = "--clear-old refuses " + old + ": it is, or holds, a tree or a system folder";
			return false;
		}
		TreeEndpoint at = e;
		at.path = parent;
		One<TreeFs> fs = MakeSide(at, setup, report, error);
		if(!fs)
			return false;
		Entry st;
		String missing;
		if(!fs->Stat(name, false, st, missing)) {
			report.WhenLine("no " + old + ": nothing to clear");
			continue;
		}
		if(st.type != 'd') {
			error = old + " is not a folder";
			return false;
		}
		int64 files = 0, bytes = 0, folders = 0;
		report.WhenLine((dry ? "counting " : "deleting ") + old);
		if(!RemoveTree(*fs, name, dry, report, files, bytes, folders, error))
			return false;
		report.WhenLine(Format("%s %s: %d files, %d folders, %s", dry ? "would delete" : "deleted", old,
		                       files, folders, FormatBytes(bytes)));
	}
	return true;
}
