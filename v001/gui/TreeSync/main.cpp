// TreeSync bootstrap: main window with menu and status bar, plus the setup screen.
// Scan, SHA-256 hashing, SQLite cache, remote transport and sync are not implemented yet.
#include "TreeSync.h"

TreeSyncWindow::TreeSyncWindow()
{
	Title("TreeSync");
	AddFrame(menu);
	AddFrame(status);
	menu.Set([this](Bar& bar) { MainMenu(bar); });
	status = TS_CONTRACTS_NATIVE ? "Ready (native contracts)" : "Ready";
	Sizeable().Zoomable();
	SetRect(0, 0, Zx(800), Zy(500));
}

void TreeSyncWindow::MainMenu(Bar& bar)
{
	bar.Sub("File", [this](Bar& bar) { FileMenu(bar); });
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

void TreeSyncWindow::EditSetup()
{
	SetupDlg dlg;
	dlg.Set(setup);
	if(dlg.Execute() != IDOK)
		return;
	setup = dlg.Get();
	TS_POST(setup.source.port > 0 && setup.target.port > 0);
	TS_POST(setup.flags.threads >= 1);
	status = setup.source.path + " -> " +
	         (setup.target.remote ? setup.target.user + "@" + setup.target.host + ":" : String()) +
	         setup.target.path + "   " + setup.flags.ToString();
}

GUI_APP_MAIN
{
	TreeSyncWindow().Run();
}
