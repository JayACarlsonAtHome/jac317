// TreeSync bootstrap: main window with source and target trees side by side,
// plus the setup screen. Scan, SHA-256 hashing, SQLite cache, remote transport
// and sync are not implemented yet.
#include "TreeSync.h"

TreeSyncWindow::TreeSyncWindow()
{
	Title("TreeSync");
	AddFrame(menu);
	AddFrame(status);
	menu.Set([this](Bar& bar) { MainMenu(bar); });
	split.Horz(source_tree, target_tree);
	Add(split.SizePos());
	LoadTrees();
	status = TS_CONTRACTS_NATIVE ? "Ready (native contracts)" : "Ready";
	Sizeable().Zoomable();
	SetRect(0, 0, Zx(900), Zy(560));
}

void TreeSyncWindow::MainMenu(Bar& bar)
{
	bar.Sub("File", [this](Bar& bar) { FileMenu(bar); });
	bar.Sub("View", [this](Bar& bar) { ViewMenu(bar); });
}

void TreeSyncWindow::FileMenu(Bar& bar)
{
	bar.Add("Setup..", [this] { EditSetup(); });
	bar.Separator();
	bar.Add("About..", [this] {
		PromptOK("[* TreeSync]&&Sync two directory trees by SHA-256.&&Stub build.");
	});
	bar.Separator();
	bar.Add("Exit", [this] { Close(); });
}

void TreeSyncWindow::ViewMenu(Bar& bar)
{
	bar.Add("Reload trees", [this] { LoadTrees(); }).Key(K_F5);
	bar.Add("Collapse all", [this] {
		source_tree.CollapseAll();
		target_tree.CollapseAll();
	});
}

void TreeSyncWindow::LoadTrees()
{
	source_tree.Load("Source", setup.source, setup.flags.xj);
	target_tree.Load("Target", setup.target, setup.flags.xj);
}

void TreeSyncWindow::EditSetup()
{
	SetupDlg dlg;
	dlg.Set(setup);
	if(dlg.Execute() != IDOK)
		return;
	setup = dlg.Get();
	TS_POST(setup.source.port > 0 && setup.target.port > 0);
	TS_POST(setup.flags.threads >= 1);
	LoadTrees();
	status = setup.source.path + " -> " +
	         (setup.target.remote ? setup.target.user + "@" + setup.target.host + ":" : String()) +
	         setup.target.path + "   " + setup.flags.ToString();
}

GUI_APP_MAIN
{
	TreeSyncWindow().Run();
}
