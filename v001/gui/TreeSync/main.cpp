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
	menu.Ctrl::Add(eta.RightPosZ(8, 640).VSizePosZ(0, 0));   // a child of the bar, not a menu item: rebuilds keep it
	eta.SetAlign(ALIGN_RIGHT);
	split.Horz(source_panel, target_panel);

	const char *stage_name[3] = { "Pre-scan: source", "Copy", "Post-scan: target" };
	for(int i = 0; i < 3; i++) {
		ParentCtrl& p = stage_pane[i];
		p.Add(stage_box[i].SetLabel(stage_name[i]).SizePos());
		p.Add(stage_bar[i].HSizePosZ(8, 8).TopPosZ(18, 16));
		p.Add(stage_text[i].HSizePosZ(8, 8).TopPosZ(36, 18));
		stage_bar[i].Set(0, 1000);
		stages.Add(p);
	}
	stages.Horz();
	progress_pane.Add(stages.HSizePosZ(2, 2).TopPosZ(0, 60));
	progress_pane.Add(active.HSizePosZ(4, 4).VSizePosZ(62, 2));
	active.AddKey();                                   // copy id
	active.AddColumn("Copying", 50);
	active.AddColumn("", 30).SetDisplay(ProgressDisplay());   // 0..1000
	active.AddColumn("Rate", 12).HeaderTab().AlignRight();
	active.AddColumn("Size", 12).HeaderTab().AlignRight();
	active.AddColumn("Time", 8).HeaderTab().AlignRight();
	active.NoCursor().NoGrid();

	bottom.Vert(progress_pane, log);
	bottom.SetPos(6500);
	ProgressVisible(false);
	vsplit.Vert(split, bottom);
	vsplit.SetPos(5500);
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
	bar.Add("Build and test..", [] { ShowBuild(); })
	   .Help("Compile TreeSync with gcc, clang or both and run the tests (v001/build.sh)");
	bar.Separator();
	bar.Add("About..", [this] {
		String qtf = "[* TreeSync]&&Sync two directory trees by SHA-256.&&[*@6 Use at your own risk.]&";
		qtf << "&Display: " << DeQtf(DisplayName(DisplayNow())) << "&";
		for(const String& line : Split(DISCLAIMER, '\n'))
			qtf << DeQtf(line) << " ";
		qtf << "&&[C1 ";
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
	bar.Separator();
	bar.Add("History...", [this] { ShowHistory(Nvl(setup.db, DefaultDbPath())); }).Key(K_CTRL_H)
	   .Help("Past runs, what each did, failures first (from the database)");
	bar.Add("Show log file", [this] { ShowLogFile(LatestLog(setup.db)); }).Key(K_CTRL_L)
	   .Help("The newest run's log file");
	bar.Add("Export newest run for AI (logs + SQL)", [this] { ExportForAi(setup.db, 0, true, true); })
	   .Help("Writes run-N-logs.AI.md and run-N-sql.AI.md in the logs folder, for an AI to analyse");
}

void TreeSyncWindow::SyncMenu(Bar& bar)
{
	bool idle = child == 0;
	bar.Add(idle, "Run", [this] { StartSync(false); }).Key(K_F9)
	   .Help("Sync for real, after a confirmation (changes the target)");
	bar.Add(idle, "Dry run", [this] { StartSync(true); }).Key(K_SHIFT_F9)
	   .Help("Show what would be done, change nothing");
	bar.Add(!idle, "Stop", [this] { StopSync(); }).Key(K_ESCAPE)
	   .Help("Stop after the current file or chunk; Run again resumes");
	bar.Separator();
	bar.Add(idle, "Scan only (SHA-256 check)", [this] { StartSync(true, "--scan-only"); }).Key(K_F7)
	   .Help("Change nothing: read and hash every file on both sides, compare attributes; "
	         "results in View > History > SHA-256 checks");
	bar.Add(idle, "Sync attributes", [this] { StartSync(false, "--sync-attrs"); }).Key(K_F8)
	   .Help("Set only the attributes that differ (mode, time, owner, extended attributes) on the "
	         "target; no file content is copied");
	bar.Add(idle, "Clear .old folders...", [this] { StartSync(false, "--clear-old"); })
	   .Help("Delete the safe-delete folders (<target>.old) for good, after a confirmation");
	bar.Separator();
	bar.Add(idle, "Run as root (pkexec)", [this] { as_root = !as_root; }).Check(as_root)
	   .Help("Start treesync-cli through pkexec; the GUI itself stays an ordinary user");
	bar.Add("Clear log", [this] { log.Clear(); });
}

void TreeSyncWindow::LoadTrees()
{
	source_panel.picker.Set(setup.source.path);
	source_panel.picker.Remote(setup.source.remote);
	source_panel.tree.Load("Source", setup.source, setup.flags.follow_links, setup.known_hosts);
	target_panel.picker.Set(setup.target.path);
	target_panel.picker.Remote(setup.target.remote);
	target_panel.tree.Load("Target", setup.target, setup.flags.follow_links, setup.known_hosts);
}

void TreeSyncWindow::ProgressVisible(bool on)
{
	if(on)
		bottom.NoZoom();
	else
		bottom.Zoom(1);   // the log only
}

Vector<String> WithoutDryRun(Vector<String> args)
{
	for(int i = args.GetCount() - 1; i >= 0; i--)
		if(args[i] == "--dry-run" || args[i] == "--no-dry-run")
			args.Remove(i);
	return args;
}

// Off for --screenshot and --bench-log: they never touch the user's settings file.
static bool save_settings = false;

void TreeSyncWindow::ShowStatus()
{
	status = CommandLineText(WithoutDryRun(ToArgs(setup)));
	// Every change of the setup comes through here: saved at once, so a crash loses nothing.
	String error;
	if(save_settings && !SaveSettings(SettingsPath(), setup, error))
		Log("warning: settings not saved: " + error);
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

void SaveScreenshot(Ctrl& c, const String& path)
{
	Size sz = c.GetRect().GetSize();
	ImageDraw iw(sz);
	iw.DrawRect(sz, SColorFace());
	c.DrawCtrl(iw);
	PNGEncoder().SaveFile(path, iw);
}

GUI_APP_MAIN
{
	Vector<String> args = clone(CommandLine());
	String shots;
	int bench = 0;
	bool from_start_page = false;
	for(int i = args.GetCount() - 1; i >= 0; i--)
		if(args[i].StartsWith("--screenshot=")) {
			shots = args[i].Mid(13);
			args.Remove(i);
		}
		else if(args[i] == "--from-start-page") {   // started by the options page's Continue
			from_start_page = true;
			args.Remove(i);
		}
		else if(args[i] == "--build-run") {   // a developer aid: Build and test, started at once
			BuildRun();
			return;
		}
		else if(args[i].StartsWith("--bench-log=")) {
			bench = ScanInt(args[i].Mid(12));
			args.Remove(i);
		}

	SyncSetup setup;
	String error;
	switch(ParseCommandLine(args, setup, error)) {
	case PARSE_HELP:
		Cout() << Usage();
		return;
	case PARSE_ERROR:
		Cerr() << ToLower(GetExeTitle()) << ": " << error << "\nTry '--help' for more information.\n";
		SetExitCode(2);
		return;
	}
	if(bench > 0) {   // a developer aid: how fast the GUI takes in a big run's output
		TreeSyncWindow w;
		w.OpenMain();
		double t = w.BenchLog(bench);
		Cout() << Format("%d lines taken in %.2f s (%.0f lines/s)\n", bench, t, bench / max(t, 0.001));
		w.Close();
		return;
	}
	if(shots.GetCount()) {   // draw the windows offscreen into PNGs and quit
		RealizeDirectory(shots);
		{
			TreeSyncWindow w;
			w.SetSetup(setup);
			SaveScreenshot(w, AppendFileName(shots, "main.png"));
			w.DemoProgress();
			SaveScreenshot(w, AppendFileName(shots, "main-running.png"));
			SetupDlg dlg;
			dlg.Set(setup);
			dlg.Screenshots(shots);
			BuildScreenshots(shots);
			if(setup.db.GetCount())
				HistoryScreenshot(setup.db, AppendFileName(shots, "history.png"));
		}
		DirTree::WaitForWorkers(10);   // a remote tree may still be connecting
		Cout() << "screenshots in " << shots << "\n";
		return;
	}
	// Options on the command line win (and become the saved settings); without any,
	// the settings saved last time are used.
	bool from_file = false;
	if(args.IsEmpty()) {
		String why;
		from_file = LoadSettings(SettingsPath(), setup, why);
		if(why.GetCount())
			Exclamation("The saved settings could not be read; starting with the defaults.&&[C " + DeQtf(why) + "]");
	}
	save_settings = true;
	ApplyDisplayChoice();   // Wayland or X11, as chosen on the options page
	// First screen: compiler and test options (like jac313's runner); then the sync
	// window. If this program was rebuilt there, start the new build in its place.
	bool page = !from_start_page && BuildPageAtStart();
	String use = page ? BuildStartPage() : String();
	if(page && use.IsEmpty())   // Exit on the options page
		return;
	if(use.GetCount()) {   // the chosen compiler's build, newest, without the page again
		Vector<String> pass = clone(CommandLine());
		pass.Add("--from-start-page");
		ExecOnDisplay(TreeSyncExe(use), pass, DisplayChoice());
	}
	WarnNotRoot();
	for(TreeEndpoint *e : { &setup.source, &setup.target })
		if(!e->remote && !PathAllowed(e->path)) {
			RefusePath(e->path);
			e->path.Clear();
		}
	{
		TreeSyncWindow w;
		if(args.GetCount() || from_file)
			w.SetSetup(setup);
		w.Run();
	}   // the trees are gone: their remote workers are told to finish
	DirTree::WaitForWorkers(3);
}
