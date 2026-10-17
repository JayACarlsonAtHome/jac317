// TreeSync bootstrap: main window with source and target trees side by side, each
// with a disk / mount / path picker, plus the setup screen. Scan, SHA-256 hashing,
// SQLite cache, remote transport and sync are not implemented yet.
#include "TreeSync.h"

TreePanel::TreePanel()
{
	Add(picker.HSizePosZ(4, 4).TopPosZ(4, PathPicker::Height));
	Add(tree.HSizePosZ(0, 0).VSizePosZ(PathPicker::Height + 8, 0));
}

TreeSyncWindow::TreeSyncWindow()
{
	Title("TreeSync");
	AddFrame(menu);
	AddFrame(status);
	menu.Set([this](Bar& bar) { MainMenu(bar); });
	split.Horz(source_panel, target_panel);
	Add(split.SizePos());

	source_panel.picker.WhenPath = [this] {
		setup.source.path = source_panel.picker.Get();
		LoadTrees();
		ShowStatus();
	};
	target_panel.picker.WhenPath = [this] {
		setup.target.path = target_panel.picker.Get();
		LoadTrees();
		ShowStatus();
	};

	LoadTrees();
	status = TS_CONTRACTS_NATIVE ? "Ready (native contracts)" : "Ready";
	Sizeable().Zoomable();
	SetRect(0, 0, Zx(1000), Zy(620));
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
	bar.Add("Reload trees", [this] {
		source_panel.picker.LoadDisks();
		target_panel.picker.LoadDisks();
		LoadTrees();
	}).Key(K_F5);
	bar.Add("Collapse all", [this] {
		source_panel.tree.CollapseAll();
		target_panel.tree.CollapseAll();
	});
}

void TreeSyncWindow::LoadTrees()
{
	source_panel.picker.Set(setup.source.path);
	source_panel.picker.Remote(setup.source.remote);
	source_panel.tree.Load("Source", setup.source, setup.flags.xj);
	target_panel.picker.Set(setup.target.path);
	target_panel.picker.Remote(setup.target.remote);
	target_panel.tree.Load("Target", setup.target, setup.flags.xj);
}

void TreeSyncWindow::ShowStatus()
{
	status = setup.source.path + " -> " +
	         (setup.target.remote ? setup.target.user + "@" + setup.target.host + ":" : String()) +
	         setup.target.path + "   " + setup.flags.ToString();
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
	ShowStatus();
}

GUI_APP_MAIN
{
	TreeSyncWindow().Run();
}
