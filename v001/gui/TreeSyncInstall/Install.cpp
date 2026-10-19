// TreeSyncInstall: puts the release build TreeSyncBuild made where root runs it, or
// removes it. A presenter: v001/install.sh does the work, as root through one pkexec
// per window (the password is asked once, at the first Install or Remove); this window shows what each build in out/ is, what is installed now,
// what install.sh would do, and its output. It runs as you, from the source tree
// (v001/out/<compiler>/treesync-install), and is not installed itself.
//
// The GCC and Clang builds install side by side, each with its own Install and Remove.
//
//   ./treesync-install --screenshot=DIR         the window, drawn offscreen into a PNG
#include <TreeSyncUi/TreeSyncUi.h>

#include <signal.h>
#include <sys/stat.h>

static const char *INSTALLED_BIN = "/usr/local/bin";
static const char *INSTALLED_DOC = "/usr/local/share/doc/treesync";
static const char *COMPILER[2] = { "gcc", "clang" };
static const char *TAG[2] = { "GCC", "Clang" };

// out/<compiler>/treesync-install -> v001; empty when not run from a build tree.
static String SourceFolder()
{
	String v001 = NormalizePath(AppendFileName(GetExeFolder(), "../.."));
	return FileExists(AppendFileName(v001, "install.sh")) ? v001 : String();
}

static String OutFolder(const String& compiler)
{
	return AppendFileName(SourceFolder(), "out/" + compiler);
}

static String InstalledCli(const String& compiler)
{
	return AppendFileName(INSTALLED_BIN, "treesync-cli-" + compiler);
}

static int64 FileMtime(const String& path)
{
	struct stat st;
	return stat(path, &st) == 0 ? (int64)st.st_mtime : 0;
}

static String FileSha256(const String& path)
{
	return FileExists(path) ? Sha256Hex(LoadFile(path)) : String();
}

class InstallWindow : public TopWindow {
	enum { POLL = 1 };
	LabelBox  build_box, installed_box, output_box;
	Label     name[2], built[2], state[2];   // per compiler: its build in out/, and what is installed
	Button    install[2], remove[2];
	LineEdit  installed, output;
	Button    remove_all, preview, close;
	MenuBar   menu;
	One<LocalProcess> proc;      // the preview, as you
	One<LocalProcess> session;   // root bash through pkexec: every Install and Remove of the window
	String    session_out;       // its output up to the next whole line
	String    running;           // what runs now: "preview", "install" or "uninstall"

	bool   Releasable(int i) const;
	void   Show();       // the builds and what is installed, again
	void   Start(const String& what, const String& compiler);   // compiler: gcc, clang or both
	bool   SessionStart();
	void   Poll();
	void   SessionLines(bool all);
	void   Done(int code);

public:
	void Close() override;
	void Snap(const String& folder);
	InstallWindow();
	~InstallWindow();
};

InstallWindow::InstallWindow()
{
	Title("TreeSyncInstall");
	Sizeable().Zoomable();
	SetRect(0, 0, Zx(900), Zy(640));
	AddFrame(menu);
	menu.Set([](Bar& bar) { bar.Sub("Help", [](Bar& bar) { HelpMenu(bar, "2-Build"); }); });

	// One row per compiler: the build in out/, whether it is installed, Install and Remove.
	// The two go side by side in /usr/local/bin: install one or both.
	Add(build_box.SetLabel("What TreeSyncBuild built (in v001/out), ready to install; each compiler installs on its own")
	              .HSizePosZ(8, 8).TopPosZ(8, 112));
	for(int i = 0; i < 2; i++) {
		int y = 30 + 40 * i;
		name[i].SetFont(StdFont().Bold());
		Add(name[i].SetLabel(TAG[i]).LeftPosZ(20, 60).TopPosZ(y, 20));
		Add(built[i].HSizePosZ(80, 270).TopPosZ(y, 20));
		Add(state[i].HSizePosZ(80, 270).TopPosZ(y + 18, 20));
		Add(install[i].SetLabel(String("Install ") + TAG[i] + "...").RightPosZ(146, 120).TopPosZ(y + 4, 28));
		Add(remove[i].SetLabel(String("Remove ") + TAG[i] + "...").RightPosZ(20, 120).TopPosZ(y + 4, 28));
		install[i].Tip(String("Copies TreeSyncRoot-") + TAG[i] + ", TreeSyncHome-" + TAG[i] + " and treesync-cli-" +
		               COMPILER[i] + " to /usr/local/bin, owned by root, with the manual, the app entries and "
		               "the polkit action (install.sh through pkexec; asks for your password). The other "
		               "compiler's install stays as it is.");
		remove[i].Tip(String("Removes the installed ") + TAG[i] + " build (through pkexec); the other one stays");
		install[i] << [this, i] {
			if(PromptYesNo(String("[* Install the ") + TAG[i] + " release build for root?]&&TreeSyncRoot-" + TAG[i] +
			               ", TreeSyncHome-" + TAG[i] + " and treesync-cli-" + COMPILER[i] + " go to [C /usr/local/bin], "
			               "owned by root. Administrators sync as root with TreeSyncRoot; everyone can use "
			               "TreeSyncHome.&&It asks for your password (pkexec), once for this window."))
				Start("install", COMPILER[i]);
		};
		remove[i] << [this, i] {
			if(PromptYesNo(String("[* Remove the installed ") + TAG[i] + " build?]&&TreeSyncRoot-" + TAG[i] +
			               ", TreeSyncHome-" + TAG[i] + " and treesync-cli-" + COMPILER[i] + " go from [C /usr/local/bin]. "
			               "The other compiler's build stays. Your settings, databases and logs stay.&&"
			               "It asks for your password (pkexec), once for this window."))
				Start("uninstall", COMPILER[i]);
		};
	}

	Add(installed_box.SetLabel(String("Installed now (") + INSTALLED_DOC + "/INSTALLED-gcc, -clang)")
	                  .HSizePosZ(8, 8).TopPosZ(126, 186));
	installed.SetReadOnly();
	installed.SetFont(Monospace());
	Add(installed.HSizePosZ(16, 16).TopPosZ(146, 158));

	Add(output_box.SetLabel("install.sh").HSizePosZ(8, 8).VSizePosZ(318, 44));
	output.SetReadOnly();
	output.SetFont(Monospace());
	Add(output.HSizePosZ(16, 16).VSizePosZ(338, 52));

	Add(preview.SetLabel("Show what it does").LeftPosZ(8, 150).BottomPosZ(8, 28));
	Add(remove_all.SetLabel("Remove all...").LeftPosZ(164, 120).BottomPosZ(8, 28));
	Add(close.SetLabel("Close").RightPosZ(8, 90).BottomPosZ(8, 28));
	preview.Tip("install.sh --dry-run: what installing both builds would do, changing nothing (no password)");
	remove_all.Tip("Removes everything install.sh installed, both builds (through pkexec)");
	preview << [this] { Start("preview", "both"); };
	remove_all << [this] {
		if(PromptYesNo("[* Remove the installed TreeSync, both builds?]&&Every TreeSyncRoot, TreeSyncHome and "
		               "treesync-cli goes from [C /usr/local/bin], with the manual, the app entries and the polkit "
		               "action. Your settings, databases and logs stay.&&It asks for your password (pkexec), once for this window."))
			Start("uninstall", "both");
	};
	close << [this] { Close(); };

	Show();
}

InstallWindow::~InstallWindow()
{
	KillTimeCallback(POLL);
	if(session)
		session->CloseWrite();   // its loop ends; root may not be signalled, so it is not killed
}

// install.sh installs only a release build: the programs it copies, and the build type
// build.sh recorded next to them.
bool InstallWindow::Releasable(int i) const
{
	String out = OutFolder(COMPILER[i]);
	for(const char *p : { "treesync-root", "treesync-home", "treesync-cli" })
		if(!FileExists(AppendFileName(out, p)))
			return false;
	return TrimBoth(LoadFile(AppendFileName(out, ".build-type"))) == "release";
}

void InstallWindow::Show()
{
	bool idle = running.IsEmpty() && SourceFolder().GetCount();
	String info;
	bool any = false;
	for(int i = 0; i < 2; i++) {
		String c = COMPILER[i];
		String out = OutFolder(c);
		String cli = AppendFileName(out, "treesync-cli");
		String type = TrimBoth(LoadFile(AppendFileName(out, ".build-type")));
		bool ok = Releasable(i);
		String installed_cli = InstalledCli(c);
		bool is_installed = FileExists(installed_cli);
		any = any || is_installed;
		bool same = is_installed && FileExists(cli) && FileSha256(cli) == FileSha256(installed_cli);

		built[i] = !FileExists(cli) ? String("not built yet (TreeSyncBuild builds it)")
		           : "built: " + Nvl(type, String("unknown")) + " build, " + DateTimeText(FileMtime(cli)) +
		             (ok ? String() : type == "release" ? String("  (not all programs built: build again)")
		                                               : String("  (build a release in TreeSyncBuild to install it)"));
		built[i].SetInk(ok ? SColorText() : SColorDisabled());
		state[i] = !is_installed ? String("not installed")
		           : same ? String("installed: this build")
		           : "installed: an older build (" + DateTimeText(FileMtime(installed_cli)) + "); Install gives root this one";
		state[i].SetInk(!is_installed ? SColorDisabled() : same ? Color(0, 128, 0) : Color(160, 90, 0));
		install[i].Enable(idle && ok);
		remove[i].Enable(idle && is_installed);

		String text = LoadFile(AppendFileName(INSTALLED_DOC, "INSTALLED-" + c));
		if(text.GetCount())
			info << (info.GetCount() ? "\n" : "") << TrimRight(text) << "\n";
	}
	if(info.IsEmpty())
		info = FileExists(AppendFileName(INSTALLED_DOC, "INSTALLED"))
		       ? LoadFile(AppendFileName(INSTALLED_DOC, "INSTALLED")) + "\n(an install from before the GCC and Clang builds "
		         "went side by side: installing either replaces it)"
		       : String("Nothing installed in ") + INSTALLED_BIN + ".";
	installed <<= info;
	remove_all.Enable(idle && (any || FileExists(AppendFileName(INSTALLED_DOC, "INSTALLED"))));
	preview.Enable(idle);
	if(SourceFolder().IsEmpty())
		installed <<= "Cannot install from here: install.sh is not two folders above this program (" + GetExeFolder() + ").";
}

// The root half: one bash through pkexec for the whole window, so the password is asked
// once. It reads "install|uninstall gcc|clang|both" lines and runs install.sh for each,
// ending each one's output with a line of its exit code. Started without waiting: the
// first request waits in the pipe while polkit asks.
static const char *SESSION_SCRIPT = R"(cd -- "$1" || exit 2
while read -r what compiler; do
  case $what in install) set -- ;; uninstall) set -- --uninstall ;; *) echo "@@exit 2"; continue ;; esac
  case $compiler in gcc|clang|both) ;; *) echo "@@exit 2"; continue ;; esac
  bash ./install.sh "$@" --compiler "$compiler" 2>&1
  echo "@@exit $?"
done)";

bool InstallWindow::SessionStart()
{
	if(session && session->IsRunning())
		return true;
	session.Create();
	if(!session->Start("/usr/bin/pkexec", Vector<String>{ "/usr/bin/bash", "-c", SESSION_SCRIPT,
	                   "treesync-install", SourceFolder() }, nullptr, SourceFolder())) {
		session.Clear();
		return false;
	}
	session_out.Clear();
	return true;
}

// install.sh as a child: as root through the session to install or uninstall (root writes
// /usr/local), as you for the preview (--dry-run changes nothing).
void InstallWindow::Start(const String& what, const String& compiler)
{
	if(running.GetCount() || SourceFolder().IsEmpty())
		return;
	Vector<String> args;
	args << AppendFileName(SourceFolder(), "install.sh");
	if(what == "uninstall")
		args << "--uninstall";
	args << "--compiler" << compiler;
	if(what == "preview")
		args << "--dry-run";
	output.Clear();
	bool fresh = what != "preview" && !(session && session->IsRunning());
	output.Insert(0, ("$ " + (what == "preview" ? String() : "(as root) ") + Join(args, " ") + "\n" +
	                  (fresh ? "(asks for your password once; later Installs and Removes in this window do not)\n"
	                         : "")).ToWString());
	if(what == "preview") {
		proc.Create();
		if(!proc->Start("/usr/bin/bash", args, nullptr, SourceFolder())) {
			proc.Clear();
			output.Insert(output.GetLength(), String("cannot start /usr/bin/bash\n").ToWString());
			return;
		}
	}
	else {
		if(!SessionStart()) {
			output.Insert(output.GetLength(), String("cannot start /usr/bin/pkexec\n").ToWString());
			return;
		}
		session->Write(what + " " + compiler + "\n");
	}
	running = what;
	Show();
	SetTimeCallback(-100, [this] { Poll(); }, POLL);
}

// The session's whole lines go to the pane; its "@@exit N" line ends the request.
void InstallWindow::SessionLines(bool all)
{
	int nl;
	while((nl = session_out.Find('\n')) >= 0 || (all && session_out.GetCount())) {
		String line = nl >= 0 ? session_out.Left(nl) : session_out;
		session_out.Remove(0, nl >= 0 ? nl + 1 : session_out.GetCount());
		if(line.StartsWith("@@exit ")) {
			Done(ScanInt(line.Mid(7)));
			continue;
		}
		output.Insert(output.GetLength(), (line + "\n").ToWString());
		output.SetCursor(output.GetLength());
	}
}

void InstallWindow::Poll()
{
	String s;
	if(proc) {
		while(proc->Read(s) && s.GetCount()) {
			output.Insert(output.GetLength(), s.ToWString());
			output.SetCursor(output.GetLength());
			s.Clear();
		}
		if(!proc->IsRunning()) {
			while(proc->Read(s) && s.GetCount()) {
				output.Insert(output.GetLength(), s.ToWString());
				s.Clear();
			}
			int code = proc->GetExitCode();
			proc.Clear();
			Done(code);
		}
	}
	if(session) {
		while(session->Read(s) && s.GetCount()) {
			session_out << s;
			s.Clear();
		}
		SessionLines(false);
		if(!session->IsRunning()) {   // pkexec refused or cancelled, or it died
			while(session->Read(s) && s.GetCount()) {
				session_out << s;
				s.Clear();
			}
			SessionLines(true);
			int code = session->GetExitCode();
			session.Clear();
			if(running.GetCount())
				Done(code ? code : 2);
		}
	}
	if(running.IsEmpty())
		KillTimeCallback(POLL);
}

void InstallWindow::Done(int code)
{
	String what = running;
	running.Clear();
	String end = code == 0 ? "\n-- done\n"
	           : code == 126 || code == 127 ? "\n-- pkexec refused or was cancelled: nothing changed\n"
	           : "\n-- install.sh failed (exit " + AsString(code) + ")\n";
	// The TreeSyncApp folder in your app list, for the entries install.sh just wrote.
	if(code == 0 && what == "install") {
		String out;
		Sys("bash " + ShellQuote(AppendFileName(SourceFolder(), "setup/app-folder.sh")), out);
		end = "\n" + out + end;
	}
	output.Insert(output.GetLength(), end.ToWString());
	output.SetCursor(output.GetLength());
	Show();
}

void InstallWindow::Close()
{
	if(running.GetCount()) {   // stopping it half way would leave half an install
		Exclamation("install.sh is still running: wait for it to finish.");
		return;
	}
	TopWindow::Close();
}

void InstallWindow::Snap(const String& folder)
{
	output <<= "$ install.sh --compiler gcc --dry-run\nwould: install -d -m 0755 /usr/local/bin ...\n";
	SaveScreenshot(*this, AppendFileName(folder, "install.png"));
}

GUI_APP_MAIN
{
	for(const String& a : CommandLine())
		if(a.StartsWith("--screenshot=")) {   // the window drawn offscreen into a PNG
			RealizeDirectory(a.Mid(13));
			InstallWindow w;
			w.Snap(a.Mid(13));
			Cout() << "screenshots in " << a.Mid(13) << "\n";
			return;
		}
	if(IsRoot()) {
		Exclamation("[* TreeSyncInstall does not run as root.]&&Start it as the user who built TreeSync: "
		            "it asks for the root password (pkexec) only for the install itself, once per window.");
		SetExitCode(2);
		return;
	}
	ApplyDisplayChoice();   // Wayland or X11, as chosen in TreeSyncBuild
	InstallWindow().Run();
}
