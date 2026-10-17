// TreeSync bootstrap: main window with source and target trees side by side, each
// with a disk / mount / path picker, plus the setup screen. Scan, SHA-256 hashing,
// SQLite cache, remote transport and sync are not implemented yet.
#include "TreeSync.h"

bool run_as_root = false;

TreePanel::TreePanel()
{
	Add(picker.HSizePosZ(4, 4).TopPosZ(4, PathPicker::Height));
	Add(tree.HSizePosZ(0, 0).VSizePosZ(PathPicker::Height + 8, 0));
}

TreeSyncWindow::TreeSyncWindow()
{
	Title(IsRoot() ? "TreeSync [root]" : "TreeSync");
	AddFrame(menu);
	AddFrame(status);
	menu.Set([this](Bar& bar) { MainMenu(bar); });
	split.Horz(source_panel, target_panel);
	vsplit.Vert(split, log);
	vsplit.SetPos(7000);
	log.SetReadOnly();
	log.SetFont(Monospace());
	Add(vsplit.SizePos());

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
	status = String(IsRoot() ? "Ready" : "Not running as root: limited to " + HomeDir()) +
	         (TS_CONTRACTS_NATIVE ? " (native contracts)" : "");
	Sizeable().Zoomable();
	SetRect(0, 0, Zx(1000), Zy(620));
}

void TreeSyncWindow::MainMenu(Bar& bar)
{
	bar.Sub("File", [this](Bar& bar) { FileMenu(bar); });
	bar.Sub("View", [this](Bar& bar) { ViewMenu(bar); });
	bar.Sub("Sync", [this](Bar& bar) { SyncMenu(bar); });
}

void TreeSyncWindow::FileMenu(Bar& bar)
{
	bar.Add("Setup..", [this] { EditSetup(); });
	bar.Separator();
	bar.Add("About..", [this] {
		String qtf = "[* TreeSync]&&Sync two directory trees by SHA-256.&&[C1 ";
		for(const String& line : Split(ToolchainReport(), '\n'))
			qtf << DeQtf(line) << "&";
		PromptOK(qtf + "]");
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

void TreeSyncWindow::SyncMenu(Bar& bar)
{
	bool idle = child == 0;
	bar.Add(idle, "Run", [this] { StartSync(false); }).Key(K_F9)
	   .Help("Run treesync-cli with the current settings (honours Dry run)");
	bar.Add(idle, "Dry run", [this] { StartSync(true); }).Key(K_SHIFT_F9)
	   .Help("Show what would be done, change nothing");
	bar.Add(!idle, "Stop", [this] { StopSync(); }).Key(K_ESCAPE)
	   .Help("Stop after the current file or chunk; Run again resumes");
	bar.Separator();
	bar.Add(idle, "Run as root (pkexec)", [this] { as_root = !as_root; }).Check(as_root)
	   .Help("Start treesync-cli through pkexec; the GUI itself stays an ordinary user");
	bar.Add("Clear log", [this] { log.Clear(); });
}

void TreeSyncWindow::LoadTrees()
{
	source_panel.picker.Set(setup.source.path);
	source_panel.picker.Remote(setup.source.remote);
	source_panel.tree.Load("Source", setup.source, setup.flags.follow_links);
	target_panel.picker.Set(setup.target.path);
	target_panel.picker.Remote(setup.target.remote);
	target_panel.tree.Load("Target", setup.target, setup.flags.follow_links);
}

void TreeSyncWindow::ShowStatus()
{
	status = CommandLineText(ToArgs(setup));
}

void TreeSyncWindow::EditSetup()
{
	SetupDlg dlg;
	dlg.Set(setup);
	if(dlg.Execute() != IDOK)
		return;
	SyncSetup s = dlg.Get();
	for(TreeEndpoint *e : { &s.source, &s.target })
		if(!e->remote && !PathAllowed(e->path)) {
			RefusePath(e->path);
			e->path.Clear();
		}
	setup = s;
	TS_POST(setup.source.port > 0 && setup.target.port > 0);
	TS_POST(setup.flags.threads >= 1);
	LoadTrees();
	ShowStatus();
}

void TreeSyncWindow::SetSetup(const SyncSetup& s)
{
	setup = s;
	LoadTrees();
	ShowStatus();
}

GUI_APP_MAIN
{
	SyncSetup setup;
	String error;
	switch(ParseCommandLine(CommandLine(), setup, error)) {
	case PARSE_HELP:
		Cout() << Usage();
		return;
	case PARSE_ERROR:
		Cerr() << ToLower(GetExeTitle()) << ": " << error << "\nTry '--help' for more information.\n";
		SetExitCode(2);
		return;
	}
	WarnNotRoot();
	for(TreeEndpoint *e : { &setup.source, &setup.target })
		if(!e->remote && !PathAllowed(e->path)) {
			RefusePath(e->path);
			e->path.Clear();
		}
	TreeSyncWindow w;
	if(CommandLine().GetCount())
		w.SetSetup(setup);
	w.Run();
}
