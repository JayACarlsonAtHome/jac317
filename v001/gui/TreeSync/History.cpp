// History: the runs in the database (the run_summary view) and, for the selected
// run, what it did (run_actions), failures first, and the SHA-256 each copy was
// checked with (file_hashes), mismatches first. Plus a viewer for log files.
// Read-only; it works while a sync is running (SQLite in WAL mode).
#include "TreeSync.h"

// A log file in a read-only, monospaced viewer.
void ShowLogFile(const String& path)
{
	if(path.IsEmpty() || !FileExists(path)) {
		Exclamation("No log file" + (path.GetCount() ? " at [C " + DeQtf(path) + "]" : String()) + ".");
		return;
	}
	TopWindow w;
	LineEdit text;
	text.SetReadOnly();
	text.SetFont(Monospace());
	text <<= LoadFile(path);
	text.SetCursor(text.GetLength());   // the end: how the run finished
	w.Add(text.SizePos());
	w.Title("TreeSync log - " + path).Sizeable().Zoomable();
	w.SetRect(0, 0, Zx(1000), Zy(600));
	w.Execute();
}

// Exports a run for AI analysis (both files when logs and sql), says where they went,
// and offers to open the folder. run 0: the newest.
void ExportForAi(const String& db_path, int64 run, bool logs, bool sql)
{
	Vector<String> done;
	String error;
	for(int i = 0; i < 2; i++) {
		if(!(i ? sql : logs))
			continue;
		String out;
		if(!(i ? ExportAiSql(db_path, run, out, error) : ExportAiLogs(db_path, run, out, error))) {
			Exclamation("Export failed: " + DeQtf(error));
			return;
		}
		done.Add(out);
	}
	String list;
	for(const String& p : done)
		list << "[C " << DeQtf(p) << "] (" << DeQtf(FormatBytes(GetFileLength(p))) << ")&";
	if(PromptYesNo("[* Exported for AI analysis:]&" + list + "&Give the file to an AI and ask it about the run.&&"
	               "Open the folder?"))
		LaunchWebBrowser(GetFileFolder(done[0]));
}

// The newest log file (their names are their start times).
String LatestLog(const String& db_path)
{
	String best;
	for(FindFile ff(AppendFileName(LogFolder(db_path), "*.log")); ff; ff.Next())
		if(ff.GetName() > GetFileName(best))
			best = ff.GetPath();
	return best;
}

class HistoryWindow : public TopWindow {
	String    db_path;
	Splitter  split;
	ArrayCtrl runs, actions, hashes;
	TabCtrl   tabs;
	Button    show_log, refresh, close, ai_logs, ai_sql;

	void LoadRuns();
	void LoadActions();
	void LoadHashes(Db& db, int64 run);

public:
	void ShowHashes()   { tabs.Set(1); }
	HistoryWindow(const String& db_path);
};

HistoryWindow::HistoryWindow(const String& path) : db_path(path)
{
	Title("TreeSync - History  (" + db_path + ")");
	Sizeable().Zoomable();
	SetRect(0, 0, Zx(1300), Zy(650));

	runs.AddColumn("Run", 5);
	runs.AddColumn("Started", 14);
	runs.AddColumn("State", 7);
	runs.AddColumn("Source -> target", 45);
	runs.AddColumn("Folders", 6).HeaderTab().AlignRight();
	runs.AddColumn("Copied", 6).HeaderTab().AlignRight();
	runs.AddColumn("Deleted", 6).HeaderTab().AlignRight();
	runs.AddColumn("Attrs set", 6).HeaderTab().AlignRight();
	runs.AddColumn("Failed", 6).HeaderTab().AlignRight();
	runs.AddColumn("Conflicts", 7).HeaderTab().AlignRight();
	runs.AddColumn("Bytes", 9).HeaderTab().AlignRight();
	runs.AddIndex();   // log path
	runs.WhenSel = [this] { LoadActions(); };
	runs.WhenLeftDouble = [this] { show_log.WhenAction(); };

	actions.AddColumn("#", 5);
	actions.AddColumn("Action", 7);
	actions.AddColumn("Path", 60);
	actions.AddColumn("State", 8);
	actions.AddColumn("Size", 9).HeaderTab().AlignRight();
	actions.AddColumn("Error", 40);

	// Each folder is one line across the whole row; its copied files follow, each with
	// the source's SHA-256 and the copy's, read back.
	hashes.AddColumn("File name", 16);
	hashes.AddColumn("Source SHA-256", 50);
	hashes.AddColumn("Target SHA-256", 50);
	hashes.AddColumn("Match", 5);
	hashes.AddColumn("Attributes", 18);
	hashes.SpanWideCells();
	tabs.Add(actions.SizePos(), "Actions");
	tabs.Add(hashes.SizePos(), "SHA-256 checks");

	split.Vert(runs, tabs);
	split.SetPos(4500);
	Add(split.HSizePosZ(4, 4).VSizePosZ(4, 36));
	Add(show_log.SetLabel("Show log").LeftPosZ(4, 100).BottomPosZ(6, 24));
	Add(refresh.SetLabel("Refresh").LeftPosZ(110, 100).BottomPosZ(6, 24));
	Add(close.SetLabel("Close").RightPosZ(4, 100).BottomPosZ(6, 24));
	Add(ai_logs.SetLabel("Export logs to AI.md").LeftPosZ(216, 160).BottomPosZ(6, 24));
	Add(ai_sql.SetLabel("Export SQL to AI.md").LeftPosZ(382, 160).BottomPosZ(6, 24));
	ai_logs.Tip("Every log of the selected run (one per start), with a guide, the problems, the slowest "
	            "copies and throughput, as Markdown for an AI to analyse");
	ai_sql.Tip("The database for the selected run: schema, all runs, its actions, chunks, hash cache and "
	           "synced files, as Markdown for an AI to analyse");
	ai_logs << [this] { if(runs.IsCursor()) ExportForAi(db_path, (int64)runs.Get(runs.GetCursor(), 0), true, false); };
	ai_sql << [this] { if(runs.IsCursor()) ExportForAi(db_path, (int64)runs.Get(runs.GetCursor(), 0), false, true); };
	show_log << [this] {
		if(runs.IsCursor())
			ShowLogFile(runs.Get(runs.GetCursor(), 11));
	};
	refresh << [this] { LoadRuns(); };
	close << [this] { Close(); };
	LoadRuns();
}

void HistoryWindow::LoadRuns()
{
	runs.Clear();
	actions.Clear();
	hashes.Clear();
	Db db;
	String error;
	if(!db.Open(db_path, error)) {
		runs.Add(Null, Null, "error", error);
		return;
	}
	// Sync attributes sets rows of file_checks, not actions: count those too.
	Stmt q(db, "SELECT run, started, state, source, target, folders, copied, deleted, failed, conflicts, "
	           "bytes, log, args, attrs_fixed + (SELECT count(*) FROM file_checks c WHERE c.run = s.run "
	           "AND c.kind = 'attrs' AND c.result = 'fixed' AND c.run NOT IN (SELECT run FROM actions "
	           "WHERE run = s.run AND kind = 'attrs')) FROM run_summary s ORDER BY run DESC LIMIT 1000");
	while(q.Step()) {
		String args = q.Text(12);
		String state = (args.EndsWith("--scan-only") ? "scan: " : args.EndsWith("--sync-attrs") ? "attrs: " : "") + q.Text(2);
		runs.Add(q.Int(0), q.Text(1), state, q.Text(3) + "  ->  " + q.Text(4),
		         q.Int(5), q.Int(6), q.Int(7), q.Int(13), q.Int(8), q.Int(9), FormatBytes(q.Int(10)), q.Text(11));
		if(q.Int(8) > 0 || q.Text(2) == "failed")
			runs.SetLineColor(runs.GetCount() - 1, Color(255, 215, 215));
	}
	if(runs.GetCount())
		runs.SetCursor(0);
}

void HistoryWindow::LoadActions()
{
	actions.Clear();
	hashes.Clear();
	if(!runs.IsCursor())
		return;
	Db db;
	String error;
	if(!db.Open(db_path, error))
		return;
	Stmt q(db, "SELECT seq, kind, rel, state, size, error FROM run_actions WHERE run = ? "
	           "ORDER BY state = 'failed' DESC, seq LIMIT 50000");
	q.Int(1, (int64)runs.Get(runs.GetCursor(), 0));
	while(q.Step()) {
		actions.Add(q.Int(0), q.Text(1), q.Text(2), q.Text(3),
		            q.Text(1) == "copy" ? FormatBytes(q.Int(4)) : String(), q.Text(5));
		if(q.Text(3) == "failed")
			actions.SetLineColor(actions.GetCount() - 1, Color(255, 215, 215));
	}
	LoadHashes(db, (int64)runs.Get(runs.GetCursor(), 0));
}

void HistoryWindow::LoadHashes(Db& db, int64 run)
{
	hashes.Clear();
	// Grouped by folder: a folder's own row (its attributes) goes on its line, its files
	// under it. Folders holding a problem first, and in a folder the problems first.
	Stmt q(db, "SELECT grp, file_name, type, src_sha256, dst_sha256, match, attrs_match, attr_diff, result, "
	           "dst_size IS NULL FROM (SELECT *, CASE WHEN type = 'd' THEN "
	           "(CASE WHEN path = '' THEN file_name ELSE path || '/' || file_name END) ELSE path END AS grp, "
	           "coalesce(match, 'true') = 'true' AND coalesce(attrs_match, 'true') = 'true' AS ok "
	           "FROM file_compare WHERE run = ?) "
	           "ORDER BY min(ok) OVER (PARTITION BY grp), grp, type <> 'd', ok, file_name LIMIT 50000");
	q.Int(1, run);
	int files = 0, bad = 0;
	String group = "\x01";
	auto attrs = [&] {
		String a = q.Text(6) == "false" ? q.Text(7) : q.Text(6) == "true" ? String("same") : String();
		if(q.Text(8).GetCount())
			a << " (" << q.Text(8) << ")";
		return a;
	};
	while(q.Step()) {
		bool folder = q.Text(2) == "d";
		if(q.Text(0) != group) {
			group = q.Text(0);
			// The folder line; a folder's own attributes, when they differ, go on it.
			String a = folder && q.Text(6) == "false" ? attrs() : String();
			hashes.Add(AttrText(Nvl(group, String("(top folder)"))).Bold(), Null, Null, Null, a.GetCount() ? Value(a) : Value());
			hashes.SetLineColor(hashes.GetCount() - 1, a.GetCount() ? Color(255, 235, 200) : Color(230, 236, 245));
			if(folder)
				continue;
		}
		if(folder)
			continue;
		String dst = q.Int(9) ? String("(missing on the target)") : q.Text(4);
		hashes.Add("    " + q.Text(1), AttrText(q.Text(3)).SetFont(Monospace()),
		           AttrText(dst).SetFont(Monospace()), q.Text(5), attrs());
		files++;
		if(q.Text(5) == "false") {
			hashes.SetLineColor(hashes.GetCount() - 1, Color(255, 215, 215));
			bad++;
		}
		else if(q.Text(6) == "false")
			hashes.SetLineColor(hashes.GetCount() - 1, Color(255, 235, 200));
	}
	tabs.GetItem(1).Text(bad ? Format("SHA-256 checks (%d, %d differ)", files, bad)
	                         : Format("SHA-256 checks (%d)", files));
}

void HistoryScreenshot(const String& db_path, const String& png)
{
	HistoryWindow w(db_path);
	SaveScreenshot(w, png);
	w.ShowHashes();
	SaveScreenshot(w, ForceExt(png, "") + "-hashes.png");
}

void ShowHistory(const String& db_path)
{
	HistoryWindow(db_path).Execute();
}
