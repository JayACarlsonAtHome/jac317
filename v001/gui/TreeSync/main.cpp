// The sync window shared by TreeSyncRoot and TreeSyncHome: source and target trees
// side by side, each with a disk / mount / path picker and its remote login, the options, the runs.
#include "TreeSync.h"

#include <sys/statvfs.h>

bool run_as_root = false;

String CompilerTag()
{
#if defined(__clang__)
	return "Clang";
#else
	return "GCC";
#endif
}

String AppName()
{
	return String(run_as_root ? "TreeSyncRoot-" : "TreeSyncHome-") + CompilerTag();
}

// Each keeps its own settings: TreeSyncRoot the settings file TreeSync always had
// (its trees are anywhere), TreeSyncHome settings-home (its trees are in the home).
// The GCC and Clang builds of one share them, so either runs the same sync.
static String AppSettingsPath()
{
	return run_as_root ? SettingsPath() : SettingsPath() + "-home";
}

TreePanel::TreePanel()
{
	Add(picker.HSizePosZ(4, 4).TopPosZ(4, PathPicker::Height));
	const char *keys = "Keys only, no passwords: set up passwordless SSH first (ssh-copy-id user@host).";
	Add(remote.SetLabel("Remote machine"));
	remote.Tip("This tree is on another machine, reached over SSH (SFTP)");
	Add(remote_summary);
	Add(remote_show.SetLabel("Show / Test"));
	remote_show.Tip("Open the remote login again: see it, change it, test it");
	Add(lbl_host.SetLabel("Host"));
	Add(host);
	host.Tip(keys);
	Add(lbl_port.SetLabel("Port"));
	Add(port);
	port.MinMax(1, 65535);
	Add(lbl_user.SetLabel("User"));
	Add(user);
	user.Tip("Empty: " + GetUserName());
	Add(lbl_key.SetLabel("Key"));
	Add(key_file);
	key_file.Tip("Optional. Empty: ssh-agent, then ~/.ssh/id_ed25519, id_ecdsa, id_rsa");
	Add(test.SetLabel("Test"));
	test.Tip(String("Log in with what is entered and list the path; when it works, the login is used "
	                "and this row folds away. ") + keys);
	Add(done.SetLabel("Done"));
	done.Tip("Use this login without testing it, and fold this row away");
	Add(tree);
	Add(used.LeftPosZ(4, 120).BottomPosZ(5, 14));
	Add(free.HSizePosZ(132, 4).BottomPosZ(2, 20));
	used.Set(0, 1000);

	remote << [this] {
		open = remote;   // ticked: fill in the login; unticked: local again, at once
		Arrange();
		if(open)
			host.SetFocus();
		else
			WhenLogin();
	};
	remote_show << [this] { open = true; Arrange(); };
	test << [this] { Test(); };
	done << [this] { Done(); };
	picker.WhenRemoteBrowse = [this](String& p) { return PickRemoteFolder(Entered(), known_hosts, p); };
	port <<= 22;
	Arrange();
}

void TreePanel::Arrange()
{
	bool r = remote, rows = r && open, folded = r && !open;
	int y = 4 + PathPicker::Height + 5;
	remote.LeftPosZ(4, 130).TopPosZ(y, 19);
	remote_summary.HSizePosZ(140, 104).TopPosZ(y, 19).Show(folded);
	remote_show.RightPosZ(4, 96).TopPosZ(y, 19).Show(folded);
	y += 24;
	// Host ......... Port [  ] User [      ]
	// Key  ................... [Test] [Done]
	lbl_host.LeftPosZ(4, 40).TopPosZ(y, 19);
	host.HSizePosZ(46, 232).TopPosZ(y, 19);
	lbl_port.RightPosZ(190, 36).TopPosZ(y, 19);
	port.RightPosZ(140, 48).TopPosZ(y, 19);
	lbl_user.RightPosZ(104, 34).TopPosZ(y, 19);
	user.RightPosZ(4, 98).TopPosZ(y, 19);
	lbl_key.LeftPosZ(4, 40).TopPosZ(y + 24, 19);
	key_file.HSizePosZ(46, 140).TopPosZ(y + 24, 19);
	test.RightPosZ(72, 64).TopPosZ(y + 24, 19);
	done.RightPosZ(4, 64).TopPosZ(y + 24, 19);
	for(Ctrl *c : { (Ctrl *)&lbl_host, (Ctrl *)&host, (Ctrl *)&lbl_port, (Ctrl *)&port, (Ctrl *)&lbl_user,
	                (Ctrl *)&user, (Ctrl *)&lbl_key, (Ctrl *)&key_file, (Ctrl *)&test, (Ctrl *)&done })
		c->Show(rows);
	if(rows)
		y += 48;
	tree.HSizePosZ(0, 0).VSizePosZ(y + 2, 24);
	picker.Remote(r);
}

void TreePanel::Summary()
{
	String key = TrimBoth(~key_file);
	remote_summary = Nvl(String(~user), GetUserName()) + "@" + String(~host) + ":" + AsString(Nvl((int)~port, 22)) +
	                 (key.GetCount() ? "   key " + key : String());
	remote_summary.Tip(remote_summary.GetText());
}

TreeEndpoint TreePanel::Entered() const
{
	TreeEndpoint e;
	e.path = picker.Get();
	GetLogin(e);
	return e;
}

void TreePanel::GetLogin(TreeEndpoint& e) const
{
	e.remote = remote;
	e.host = TrimBoth(~host);
	e.port = Nvl((int)~port, 22);
	e.user = TrimBoth(~user);
	e.key_file = TrimBoth(~key_file);
}

void TreePanel::SetLogin(const TreeEndpoint& e, const String& known)
{
	known_hosts = known;
	remote <<= e.remote;
	host <<= e.host;
	port <<= e.port;
	user <<= e.user;
	key_file <<= e.key_file;
	open = e.remote && e.host.IsEmpty();   // a login not filled in yet stays open
	Summary();
	Arrange();
}

void TreePanel::Done()
{
	if(TrimBoth(~host).IsEmpty()) {
		PromptOK("Enter the remote machine's host first, or untick [* Remote machine].");
		return;
	}
	open = false;
	Summary();
	Arrange();
	WhenLogin();
}

// The login a run will make (SftpFs, the same known_hosts check and key order), then a
// listing of the path. In a thread, behind a progress box that waits for it. A pass
// applies the login and folds the row; a failure says why and leaves it open.
void TreePanel::Test()
{
	TreeEndpoint e = Entered();
	if(IsNull(e.host)) {
		PromptOK("Enter the remote machine's host first.");
		return;
	}
	String where = Nvl(e.user, GetUserName()) + "@" + e.host + ":" + AsString(e.port);
	bool   ok = false;
	String error;
	int    entries = -1;
	std::atomic<bool> finished = false;
	std::thread t([&] {
		SftpFs fs(e, known_hosts, false);
		ok = fs.Connect(error);
		if(ok && !IsNull(e.path)) {
			Vector<FolderEntry> list;
			if(fs.List("", list, error))
				entries = list.GetCount();
			else
				ok = false;
		}
		finished = true;
	});
	{
		Progress pi("Testing " + where + "...");
		while(!finished) {
			pi.Step();
			Sleep(20);
		}
	}
	t.join();
	if(!ok) {
		Exclamation("[* " + DeQtf(where) + "]: the test failed.&&" + DeQtf(error));
		return;
	}
	Done();
	remote_summary = remote_summary.GetText() + (entries < 0 ? "   (logged in; no path yet)"
	                 : "   (test passed: " + AsString(entries) + (entries == 1 ? " entry)" : " entries)"));
}

void TreePanel::ShowFree(int64 free_bytes, int64 total, bool exact)
{
	used.Set(total > 0 ? (int)((total - free_bytes) * 1000 / total) : 0, 1000);
	free = String(exact ? "" : "~") + FormatBytes(free_bytes) + " free of " + FormatBytes(total) +
	       Format(" (%d%% used)", total > 0 ? (int)((total - free_bytes) * 100 / total) : 0);
	free.Tip(exact ? "The disk's free space, as the disk says"
	               : "Worked out from what this run has written; the disk is asked again at the end");
	free.SetInk(total > 0 && free_bytes < total / 20 ? LtRed() : SColorText());   // under 5% free
}

void TreePanel::NoFree(const String& why)
{
	used.Set(0, 1000);
	free = why;
	free.Tip(String());
	free.SetInk(SColorDisabled());
}

TreeSyncWindow::TreeSyncWindow()
{
	// What its runs are, in the caption: every window of it says so.
	Title(AppName() + (run_as_root ? String("  -  syncs as root") : "  -  syncs as " + GetUserName() + ", inside " + HomeDir()) +
	      (IsRoot() ? "  [this window is root]" : ""));
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
	active.AddColumn("Copied", 12).SetDisplay(StdRightDisplay()).HeaderTab().AlignRight();   // bytes so far ("Read" in Scan only)
	active.AddColumn("Rate", 12).SetDisplay(StdRightDisplay()).HeaderTab().AlignRight();
	active.AddColumn("Size", 12).SetDisplay(StdRightDisplay()).HeaderTab().AlignRight();
	active.AddColumn("Time", 8).SetDisplay(StdRightDisplay()).HeaderTab().AlignRight();
	active.NoCursor().NoGrid();

	bottom.Vert(progress_pane, log);
	bottom.SetPos(6500);
	ProgressVisible(false);
	vsplit.Vert(split, bottom);
	vsplit.SetPos(5500);
	log.SetReadOnly();
	log.SetFont(Monospace());
	Add(vsplit.SizePos());

	// The source tree's checkboxes choose what is synced (--include-path, relative to
	// the source: a new source starts with everything checked).
	source_panel.tree.Checks();
	source_panel.tree.WhenBar = [this](Bar& bar) { EditMenu(bar, 0); };
	target_panel.tree.WhenBar = [this](Bar& bar) { EditMenu(bar, 1); };
	source_panel.tree.WhenChecks = [this] {
		setup.flags.include_paths = source_panel.tree.GetChecked();
		ShowStatus();
	};
	source_panel.picker.WhenPath = [this] {
		if(setup.source.path != source_panel.picker.Get())
			setup.flags.include_paths.Clear();
		setup.source.path = source_panel.picker.Get();
		LoadTrees();
		ShowStatus();
	};
	target_panel.picker.WhenPath = [this] {
		setup.target.path = target_panel.picker.Get();
		LoadTrees();
		ShowStatus();
	};
	// A remote login applied on a panel (Test passed, Done, or Remote unticked).
	for(int side = 0; side < 2; side++) {
		TreePanel& panel = side == 0 ? source_panel : target_panel;
		panel.WhenLogin = [this, side, &panel] {
			TreeEndpoint& e = side == 0 ? setup.source : setup.target;
			TreeEndpoint was = e;
			panel.GetLogin(e);
			if(side == 0 && (e.remote != was.remote || e.host != was.host))
				setup.flags.include_paths.Clear();   // the checked paths are relative to the source
			if(!e.remote && !run_as_root && !PathAllowed(e.path)) {   // runs as root: any path
				RefusePath(e.path);
				e.path.Clear();
			}
			TS_POST(e.port > 0);
			LoadTrees();
			ShowStatus();
		};
	}

	LoadTrees();
	status = (run_as_root ? String(IsRoot() ? "Ready" : "Ready: syncs run as root (pkexec)")
	                      : "Ready: syncs run as you, inside " + HomeDir()) +
	         (TS_CONTRACTS_NATIVE ? " (native contracts)" : "");
	Sizeable().Zoomable();
	SetRect(0, 0, Zx(1000), Zy(620));
}

void TreeSyncWindow::MainMenu(Bar& bar)
{
	bar.Add("Options", [this] { EditOptions(); });
	bar.Sub("View", [this](Bar& bar) { ViewMenu(bar); });
	bar.Sub("Sync", [this](Bar& bar) { SyncMenu(bar); });
	bar.Sub("Help", [this](Bar& bar) {
		HelpMenu(bar, "3-Run");
		bar.Separator();
		bar.Add("About...", [this] { ShowAbout(); });
	});
	bar.Add("Exit", [this] { Close(); });
}

void TreeSyncWindow::ShowAbout()
{
	String qtf = "[* " + AppName() + "]&&Sync two directory trees by SHA-256" +
	             (run_as_root ? ", as root." : ", as you, inside your home.") + "&&[*@6 Use at your own risk.]&";
	qtf << "&Display: " << DeQtf(DisplayName(DisplayNow())) << "&";
	for(const String& line : Split(DISCLAIMER, '\n'))
		qtf << DeQtf(line) << " ";
	qtf << "&&[C1 ";
	for(const String& line : Split(ToolchainReport(), '\n'))
		qtf << DeQtf(line) << "&";
	PromptOK(qtf + "]");
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
	bar.Add("Clear log", [this] { log.Clear(); });
}

void TreeSyncWindow::LoadTrees()
{
	source_panel.picker.Set(setup.source.path);
	source_panel.tree.SetChecked(setup.flags.include_paths);
	source_panel.tree.Load("Source", setup.source, setup.flags.follow_links, setup.known_hosts);
	target_panel.picker.Set(setup.target.path);
	target_panel.tree.Load("Target", setup.target, setup.flags.follow_links, setup.known_hosts);
	// Free space: a local tree's disk is asked here (its nearest existing folder); a
	// remote one's comes with a run (the server is asked then), and stays until the
	// tree changes.
	auto disk = [](TreePanel& panel, const TreeEndpoint& e) {
		String key = (e.remote ? e.user + "@" + e.host + ":" : String()) + e.path;
		if(e.path.IsEmpty()) {
			panel.NoFree(String());
			panel.free_for = key;
			return;
		}
		if(e.remote) {
			if(panel.free_for != key)
				panel.NoFree("Free space: shown when a run starts");
			panel.free_for = key;
			return;
		}
		panel.free_for = key;
		String p = e.path;
		struct statvfs vfs;
		while(statvfs(p, &vfs) != 0) {
			String up = GetFileFolder(p);
			if(up.IsEmpty() || up == p) {
				panel.NoFree("Free space: cannot be told");
				return;
			}
			p = up;
		}
		panel.ShowFree((int64)vfs.f_bavail * vfs.f_frsize, (int64)vfs.f_blocks * vfs.f_frsize, true);
	};
	disk(source_panel, setup.source);
	disk(target_panel, setup.target);
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
	if(save_settings && !SaveSettings(AppSettingsPath(), setup, error))
		Log("warning: settings not saved: " + error);
}

// Options: the mode and switches. The trees and logins are on the main window.
void TreeSyncWindow::EditOptions()
{
	SetupDlg dlg;
	dlg.Set(setup.flags);
	if(dlg.Execute() != IDOK)
		return;
	SyncFlags f = dlg.Get();
	f.include_paths = setup.flags.include_paths;   // the source tree's checkboxes, not the dialog's
	setup.flags = f;
	TS_POST(setup.flags.threads >= 1);
	LoadTrees();   // follow-links changes what the trees show
	ShowStatus();
}

void TreeSyncWindow::SetSetup(const SyncSetup& s)
{
	setup = s;
	source_panel.SetLogin(setup.source, setup.known_hosts);
	target_panel.SetLogin(setup.target, setup.known_hosts);
	LoadTrees();
	ShowStatus();
}

// --screenshot: the target as a remote tree, folded and open (only the panel: nothing connects).
void TreeSyncWindow::DemoRemote(bool open)
{
	TreeEndpoint e;
	e.remote = true;
	e.host = "backup.example";
	e.path = "/srv/backup";
	target_panel.SetLogin(e, setup.known_hosts);
	target_panel.picker.Set(e.path);
	if(open)
		target_panel.remote_show.WhenAction();
}

// TreeSyncRoot (root_app) syncs as root, through pkexec: for administrators only.
// TreeSyncHome syncs as the user, inside their home: for anyone, never as root.
void TreeSyncMain(bool root_app)
{
	run_as_root = root_app;
	Vector<String> args = clone(CommandLine());
	String shots;
	int bench = 0;
	for(int i = args.GetCount() - 1; i >= 0; i--)
		if(args[i].StartsWith("--screenshot=")) {
			shots = args[i].Mid(13);
			args.Remove(i);
		}
		else if(args[i].StartsWith("--bench-log=")) {
			bench = (int)clamp(ScanInt64(args[i].Mid(12)), (int64)0, (int64)INT_MAX);   // not cut to int
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
			w.DemoScanProgress();
			SaveScreenshot(w, AppendFileName(shots, "main-scan.png"));
			w.DemoRemote(false);
			SaveScreenshot(w, AppendFileName(shots, "main-remote.png"));
			w.DemoRemote(true);
			SaveScreenshot(w, AppendFileName(shots, "main-remote-open.png"));
			SetupDlg dlg;
			dlg.Set(setup.flags);
			dlg.Screenshots(shots);
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
		from_file = LoadSettings(AppSettingsPath(), setup, why);
		if(why.GetCount())
			Exclamation("The saved settings could not be read; starting with the defaults.&&[C " + DeQtf(why) + "]");
	}
	save_settings = true;
	ApplyDisplayChoice();   // Wayland or X11, as chosen in TreeSyncBuild
	if(root_app && !IsAdmin()) {
		Exclamation("[* " + AppName() + " is for administrators.]&&It syncs as root, which needs a member of "
		            "wheel (or sudo, admin). Use [* TreeSyncHome] to sync inside your home, as yourself.");
		SetExitCode(2);
		return;
	}
	if(!root_app && IsRoot()) {
		Exclamation("[* " + AppName() + " does not run as root.]&&It syncs as you, inside your home. "
		            "Start [* TreeSyncRoot] as yourself: it runs each sync as root.");
		SetExitCode(2);
		return;
	}
	for(TreeEndpoint *e : { &setup.source, &setup.target })
		if(!e->remote && !run_as_root && !PathAllowed(e->path)) {
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
