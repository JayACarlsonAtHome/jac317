// History: the runs in the database (the run_summary view) and, for the selected
// run, what it did (run_actions), failures first. Plus a viewer for log files.
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
	ArrayCtrl runs, actions;
	Button    show_log, refresh, close;

	void LoadRuns();
	void LoadActions();

public:
	HistoryWindow(const String& db_path);
};

HistoryWindow::HistoryWindow(const String& path) : db_path(path)
{
	Title("TreeSync - History  (" + db_path + ")");
	Sizeable().Zoomable();
	SetRect(0, 0, Zx(1100), Zy(650));

	runs.AddColumn("Run", 5);
	runs.AddColumn("Started", 14);
	runs.AddColumn("State", 7);
	runs.AddColumn("Source -> target", 45);
	runs.AddColumn("Folders", 6).HeaderTab().AlignRight();
	runs.AddColumn("Copied", 6).HeaderTab().AlignRight();
	runs.AddColumn("Deleted", 6).HeaderTab().AlignRight();
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

	split.Vert(runs, actions);
	split.SetPos(4500);
	Add(split.HSizePosZ(4, 4).VSizePosZ(4, 36));
	Add(show_log.SetLabel("Show log").LeftPosZ(4, 100).BottomPosZ(6, 24));
	Add(refresh.SetLabel("Refresh").LeftPosZ(110, 100).BottomPosZ(6, 24));
	Add(close.SetLabel("Close").RightPosZ(4, 100).BottomPosZ(6, 24));
	show_log << [this] {
		if(runs.IsCursor())
			ShowLogFile(runs.Get(runs.GetCursor(), 10));
	};
	refresh << [this] { LoadRuns(); };
	close << [this] { Close(); };
	LoadRuns();
}

void HistoryWindow::LoadRuns()
{
	runs.Clear();
	actions.Clear();
	Db db;
	String error;
	if(!db.Open(db_path, error)) {
		runs.Add(Null, Null, "error", error);
		return;
	}
	Stmt q(db, "SELECT run, started, state, source, target, folders, copied, deleted, failed, conflicts, "
	           "bytes, log FROM run_summary ORDER BY run DESC LIMIT 1000");
	while(q.Step()) {
		runs.Add(q.Int(0), q.Text(1), q.Text(2), q.Text(3) + "  ->  " + q.Text(4),
		         q.Int(5), q.Int(6), q.Int(7), q.Int(8), q.Int(9), FormatBytes(q.Int(10)), q.Text(11));
		if(q.Int(8) > 0 || q.Text(2) == "failed")
			runs.SetLineColor(runs.GetCount() - 1, Color(255, 215, 215));
	}
	if(runs.GetCount())
		runs.SetCursor(0);
}

void HistoryWindow::LoadActions()
{
	actions.Clear();
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
}

void HistoryScreenshot(const String& db_path, const String& png)
{
	HistoryWindow w(db_path);
	SaveScreenshot(w, png);
}

void ShowHistory(const String& db_path)
{
	HistoryWindow(db_path).Execute();
}
