// Build: compiles TreeSync with the chosen compilers and, when "Run the tests" is
// ticked, runs the tests and the checked runs under it, like jac313's Jac313Runner. A presenter: v001/build.sh does the work; this window
// builds its command lines, runs them one after another as child processes,
// streams their output and sums up how each went.
//
//   Screen 1  Options + Start   (choices saved to ~/.config/treesync/build); also the
//                               first screen when TreeSync starts, unless switched off
//   Screen 2  Running           (each step's state, elapsed time, live output, Stop)
//   Screen 3  Results           (start : stop : duration per step, the j5c Results.txt layout;
//                                with Compare, a table of the compilers side by side)
//
// Compare runs compare.sh per compiler after the builds: versions, C++26 features,
// import std (works? compile times against #include), benchmarks of the built
// program, sizes; plus each build's time and test result. Rows that differ are marked.
#include "TreeSync.h"

#include <signal.h>

// out/<compiler>/treesync -> v001/build.sh; empty when this program was not run
// from a build tree.
static String BuildScript()
{
	String s = NormalizePath(AppendFileName(GetExeFolder(), "../../build.sh"));
	return FileExists(s) ? s : String();
}

// The TreeSync program built by one compiler: out/<compiler>/treesync.
String TreeSyncExe(const String& compiler)
{
	return NormalizePath(AppendFileName(GetExeFolder(), "../" + compiler + "/treesync"));
}

static String BuildSettingsPath()
{
	return AppendFileName(GetFileFolder(SettingsPath()), "build");
}

// Display: Wayland (recommended) or X Windows (X11, not recommended: any X11 program
// can read the keyboard and the screen of the others). GTK picks by GDK_BACKEND.
bool WaylandAvailable()
{
	return GetEnv("WAYLAND_DISPLAY").GetCount();
}

String DisplayNow()
{
	return Ctrl::IsWayland() ? "wayland" : "x11";
}

String DisplayChoice()
{
	String d = "wayland";
	for(const String& line : Split(LoadFile(BuildSettingsPath()), '\n'))
		if(line == "display=x11")
			d = "x11";
	return WaylandAvailable() ? d : String("x11");
}

String DisplayName(const String& d)
{
	return d == "wayland" ? "Wayland" : "X Windows (X11)";
}

// Starts exe (with args) on the chosen display, in place of this program.
void ExecOnDisplay(const String& exe, const Vector<String>& args, const String& display)
{
	setenv("GDK_BACKEND", display == "wayland" ? "wayland" : "x11", 1);
	setenv("TREESYNC_DISPLAY_SET", "1", 1);   // the started program does not switch again
	std::vector<char *> argv;
	argv.push_back(const_cast<char *>(~exe));
	for(const String& a : args)
		argv.push_back(const_cast<char *>(~a));
	argv.push_back(nullptr);
	execv(exe, argv.data());
	Exclamation("Could not start " + DeQtf(exe) + ": " + DeQtf(strerror(errno)));
}

// At start: if this program came up on the other display than the chosen one, start
// it again on the chosen one (once; GDK_BACKEND is only read when GTK starts).
void ApplyDisplayChoice()
{
	if(GetEnv("TREESYNC_DISPLAY_SET").GetCount() || DisplayNow() == DisplayChoice())
		return;
	ExecOnDisplay(GetExeFilePath(), CommandLine(), DisplayChoice());
}

namespace {

struct BuildStep : Moveable<BuildStep> {
	String         title;
	Vector<String> args;          // for build.sh, or compare.sh
	String         compiler;      // gcc or clang
	bool           compare = false;
	int64          started = 0, stopped = 0;
	int            code = -1;     // exit code; -1 not run yet
	String         checks;        // "passed: 214/214 checks" when the tests ran

	bool Checked() const {        // a test run under TSan, ASan, valgrind or helgrind, not a build
		for(const char *a : { "--tsan", "--asan", "--valgrind", "--helgrind" })
			if(FindIndex(args, String(a)) >= 0)
				return true;
		return false;
	}
};

}

class BuildWindow : public TopWindow {
	enum { POLL = 1 };   // the output timer
	ParentCtrl s1, s2, s3;

	// Screen 1
	LabelBox  compiler_box, build_box, display_box, machine_box, toolchain_box;
	Label     req_status;
	Button    check_again, fix;
	bool      req_ok = true;
	void      CheckRequirements();
	void      OpenFixTerminal();
	Switch    display;
	Label     display_now;
	Label     compiler_label, type_label;
	Switch    compiler, type;
	Option    tests, tsan, asan, valgrind, helgrind, clean, compare;
	LineEdit  toolchain;
	Label     command;
	Button    start, close1;
	Option    at_start;

	// Screen 2
	ArrayCtrl steps_list;
	Label     running;
	LineEdit  output;
	Button    stop;

	// Screen 3
	Splitter  split3;
	ArrayCtrl compare_list;
	LineEdit  results;
	Button    back, close3;
	Button    use_gcc1, use_clang1, use_gcc3, use_clang3;   // startup: continue with that build
	Button    exit1, exit3;                                  // startup: quit TreeSync

	Vector<BuildStep> steps;
	int               step = -1;
	One<LocalProcess> proc;
	bool              stopping = false;
	int64             all_started = 0;
	String            partial;   // output after the last newline
	ArrayMap<String, VectorMap<String, String>> facts;   // compiler -> what -> value

	void Screen(int n)  { s1.Show(n == 1); s2.Show(n == 2); s3.Show(n == 3); }
	void Plan();
	void ShowCommand();
	void Load();
	void Save();
	void Start();
	void NextStep();
	void Poll();
	void Out(const String& text);
	void StepDone();
	void Finish();
	void Stop();
	void ShowCompare();
	String CompareText();

public:
	bool startup = false;              // the page TreeSync opens on: Continue instead of Close
	String continue_with;              // startup: "gcc" or "clang", the build to run next
	String DisplayChosen() const       { return AsString(~display); }
	void Startup();
	void UseButtons();
	bool auto_run = false;             // --build-run: start at once, close when done
	void Run()                         { Start(); }
	void Snap(const String& folder);   // --screenshot: the three screens as PNGs
	void Close() override;
	bool Key(dword key, int count) override
	{
		if(key == K_F1) {   // the manual's Build chapter
			OpenManual("2-Build");
			return true;
		}
		return TopWindow::Key(key, count);
	}

	BuildWindow();
	~BuildWindow();
};

BuildWindow::BuildWindow()
{
	Title("TreeSync - Build");
	Sizeable().Zoomable();
	SetRect(0, 0, Zx(900), Zy(760));
	Add(s1.SizePos());
	Add(s2.SizePos());
	Add(s3.SizePos());

	// Screen 1: options
	s1.Add(compiler_box.SetLabel("Compiler").HSizePosZ(8, 8).TopPosZ(8, 52));
	compiler.Add("gcc", "gcc 15 (gcc-toolset-15)").Add("clang", "clang 21").Add("both", "both, one after the other");
	s1.Add(compiler.HSizePosZ(20, 20).TopPosZ(28, 22));

	s1.Add(build_box.SetLabel("Build").HSizePosZ(8, 8).TopPosZ(66, 222));
	s1.Add(type_label.SetText("Type:").LeftPosZ(20, 60).TopPosZ(86, 22));
	type.Add("debug", "Debug").Add("release", "Release (optimized)");
	s1.Add(type.LeftPosZ(80, 400).TopPosZ(86, 22));
	// One switch for testing: unticked, Build only builds. The checked runs below it
	// keep their ticks but run only with it (greyed out without it).
	tests.SetLabel("Run the tests (treesync-test) after building");
	s1.Add(tests.LeftPosZ(20, 600).TopPosZ(112, 22));
	tsan.SetLabel("Also under ThreadSanitizer (clang; finds data races between threads)");
	s1.Add(tsan.LeftPosZ(44, 700).TopPosZ(136, 22));
	asan.SetLabel("Also under AddressSanitizer + UBSan (each compiler; memory errors, leaks, "
	              "undefined behaviour such as int overflow)");
	s1.Add(asan.LeftPosZ(44, 836).TopPosZ(160, 22));
	valgrind.SetLabel("Also under valgrind memcheck (each compiler; reads of uninitialised memory; slow)");
	s1.Add(valgrind.LeftPosZ(44, 836).TopPosZ(184, 22));
	helgrind.SetLabel("Also under valgrind helgrind (once, gcc when both; data races, lock order; very slow)");
	s1.Add(helgrind.LeftPosZ(44, 836).TopPosZ(208, 22));
	clean.SetLabel("Clean build: compile everything, not only what changed (fair build times)");
	s1.Add(clean.LeftPosZ(20, 700).TopPosZ(232, 22));
	compare.SetLabel("Compare the compilers: versions, C++26 features, import std, speed, size");
	s1.Add(compare.LeftPosZ(20, 700).TopPosZ(256, 22));

	s1.Add(display_box.SetLabel("Display").HSizePosZ(8, 8).TopPosZ(294, 52));
	display.Add("wayland", "Wayland (recommended)").Add("x11", "X Windows / X11 (not recommended)");
	s1.Add(display.LeftPosZ(20, 520).TopPosZ(314, 22));
	s1.Add(display_now.HSizePosZ(560, 16).TopPosZ(314, 22));
	display.Tip("X11 lets any program read the keyboard and the screen of the others, and is no "
	            "longer developed. Use it only on a desktop without Wayland.");
	s1.Add(machine_box.SetLabel("This machine").HSizePosZ(8, 8).TopPosZ(352, 52));
	s1.Add(req_status.HSizePosZ(20, 360).TopPosZ(372, 22));
	s1.Add(check_again.SetLabel("Check again").RightPosZ(196, 110).TopPosZ(370, 26));
	s1.Add(fix.SetLabel("Open terminal to fix...").RightPosZ(20, 170).TopPosZ(370, 26));
	check_again << [this] { CheckRequirements(); };
	fix << [this] { OpenFixTerminal(); };
	fix.Tip("Opens a terminal with ./bootstrap.sh typed at the prompt; press Enter to run it. "
	        "It asks before it installs anything.");
	s1.Add(toolchain_box.SetLabel("This program was built with").HSizePosZ(8, 8).VSizePosZ(410, 204));
	toolchain.SetReadOnly();
	toolchain.SetFont(Monospace());
	toolchain <<= ToolchainReport();
	s1.Add(toolchain.HSizePosZ(16, 16).VSizePosZ(430, 212));

	command.SetFont(Monospace());
	s1.Add(command.HSizePosZ(8, 8).BottomPosZ(42, 156));   // up to 10 steps, one line each
	s1.Add(start.SetLabel("Build").RightPosZ(116, 100).BottomPosZ(8, 28));
	s1.Add(close1.SetLabel("Close").RightPosZ(8, 100).BottomPosZ(8, 28));
	at_start.SetLabel("Show this page when TreeSync starts");
	s1.Add(at_start.LeftPosZ(8, 400).BottomPosZ(10, 24));

	// Screen 2: running
	steps_list.AddColumn("Step", 40);
	steps_list.AddColumn("State", 12);
	steps_list.AddColumn("Time", 10).HeaderTab().AlignRight();
	steps_list.AddColumn("Tests", 20);
	steps_list.NoCursor();
	s2.Add(steps_list.HSizePosZ(8, 8).TopPosZ(8, 110));
	s2.Add(running.HSizePosZ(8, 8).TopPosZ(122, 22));
	output.SetReadOnly();
	output.SetFont(Monospace());
	s2.Add(output.HSizePosZ(8, 8).VSizePosZ(148, 44));
	s2.Add(stop.SetLabel("Stop").RightPosZ(8, 100).BottomPosZ(8, 28));

	// Screen 3: results
	results.SetReadOnly();
	results.SetFont(Monospace());
	compare_list.AddColumn("What", 30);
	compare_list.AddColumn("gcc", 35);
	compare_list.AddColumn("clang", 35);
	compare_list.NoCursor();   // no row shading: only differences are coloured
	split3.Vert(compare_list, results);
	split3.SetPos(6500);
	s3.Add(split3.HSizePosZ(8, 8).VSizePosZ(8, 44));
	s3.Add(back.SetLabel("Back").LeftPosZ(8, 100).BottomPosZ(8, 28));
	s3.Add(close3.SetLabel("Close").RightPosZ(8, 100).BottomPosZ(8, 28));
	for(Button *b : { &use_gcc1, &use_clang1, &use_gcc3, &use_clang3 }) {
		bool gcc = b == &use_gcc1 || b == &use_gcc3;
		b->SetLabel(gcc ? "Continue to TreeSync using GCC exe" : "Continue to TreeSync using Clang exe");
		*b << [this, gcc] { continue_with = gcc ? "gcc" : "clang"; Close(); };
		(b == &use_gcc1 || b == &use_clang1 ? s1 : s3).Add(b->RightPosZ(gcc ? 266 : 8, 250).BottomPosZ(8, 28));
		b->Hide();
	}
	for(Button *b : { &exit1, &exit3 }) {
		b->SetLabel("Exit").Tip("Quit TreeSync without going on");
		*b << [this] { continue_with.Clear(); Close(); };
		(b == &exit1 ? s1 : s3).Add(b->RightPosZ(8, 80).BottomPosZ(8, 28));
		b->Hide();
	}

	compiler << [this] { ShowCommand(); };
	type << [this] { ShowCommand(); };
	tests << [this] { ShowCommand(); };
	tsan << [this] { ShowCommand(); };
	asan << [this] { ShowCommand(); };
	valgrind << [this] { ShowCommand(); };
	helgrind << [this] { ShowCommand(); };
	clean << [this] { ShowCommand(); };
	compare << [this] { ShowCommand(); };
	start << [this] { Start(); };
	close1 << [this] { Close(); };
	close3 << [this] { Close(); };
	stop << [this] { Stop(); };
	at_start << [this] { Save(); };
	display << [this] { Save(); };
	back << [this] { Screen(1); };

	Load();
	ShowCommand();
	CheckRequirements();
	if(BuildScript().IsEmpty()) {
		start.Disable();
		command = "Cannot build from here: build.sh is not two folders above this program (" +
		          GetExeFolder() + ").";
	}
	Screen(1);
}

BuildWindow::~BuildWindow()
{
	KillTimeCallback(POLL);
	if(proc && proc->IsRunning())
		kill(-proc->GetPid(), SIGTERM);
}

// The steps: build.sh once per compiler, then the checked test runs: ThreadSanitizer
// (clang), AddressSanitizer + UBSan and valgrind (each compiler), helgrind (once: gcc
// when both, as ThreadSanitizer covers clang).
void BuildWindow::Plan()
{
	steps.Clear();
	String c = ~compiler;
	bool release = String(~type) == "release";
	for(const char *cc : { "gcc", "clang" }) {
		if(c != "both" && c != cc)
			continue;
		BuildStep& s = steps.Add();
		s.compiler = cc;
		s.args << "--compiler" << cc;
		if(release)
			s.args << "--release";
		if(tests)
			s.args << "--test";
		if(clean)
			s.args << "--clean";
		s.title = String(cc) + (release ? " release" : " debug") + (clean ? " clean" : "") +
		          (tests ? " build + tests" : " build");
	}
	if(compare)
		for(const char *cc : { "gcc", "clang" }) {
			if(c != "both" && c != cc)
				continue;
			BuildStep& s = steps.Add();
			s.compiler = cc;
			s.compare = true;
			s.args << "--compiler" << cc;
			s.title = String("compare ") + cc + ": features, import std, speed, size";
		}
	if(!tests)   // Build only builds
		return;
	if(tsan) {
		BuildStep& s = steps.Add();
		s.compiler = "clang";
		s.args << "--compiler" << "clang" << "--tsan";
		if(release)
			s.args << "--release";
		s.title = "clang tests under ThreadSanitizer";
	}
	for(const char *check : { "asan", "valgrind" }) {
		if(!(check == String("asan") ? asan : valgrind))
			continue;
		for(const char *cc : { "gcc", "clang" }) {
			if(c != "both" && c != cc)
				continue;
			BuildStep& s = steps.Add();
			s.compiler = cc;
			s.args << "--compiler" << cc << String("--") + check;
			if(release)
				s.args << "--release";
			s.title = String(cc) + (check == String("asan") ? " tests under AddressSanitizer + UBSan"
			                                                 : " tests under valgrind memcheck");
		}
	}
	if(helgrind) {
		String cc = c == "clang" ? "clang" : "gcc";
		BuildStep& s = steps.Add();
		s.compiler = cc;
		s.args << "--compiler" << cc << "--helgrind";
		if(release)
			s.args << "--release";
		s.title = cc + " tests under valgrind helgrind";
	}
}

void BuildWindow::ShowCommand()
{
	Plan();
	String text;
	for(const BuildStep& s : steps)
		text << (text.GetCount() ? "\n" : "") << (s.compare ? "compare.sh " : "build.sh ") << Join(s.args, " ");
	command = text;
	for(Option *o : { &tsan, &asan, &valgrind, &helgrind })
		o->Enable(tests);
	start.Enable(BuildScript().GetCount() && steps.GetCount());
}

void BuildWindow::Load()
{
	compiler <<= "both";
	type <<= "debug";
	tests = true;
	tsan = false;
	asan = false;
	valgrind = false;
	helgrind = false;
	clean = false;
	compare = false;
	at_start = true;
	display <<= DisplayChoice();
	display.EnableCase(0, WaylandAvailable());
	display_now = "Now running on " + DisplayName(DisplayNow()) +
	              (WaylandAvailable() ? String() : String(" (no Wayland session found)"));
	for(String line : Split(LoadFile(BuildSettingsPath()), '\n')) {
		String key = line.Left(max(line.Find('='), 0)), value = line.Mid(line.Find('=') + 1);
		if(key == "compiler" && (value == "gcc" || value == "clang" || value == "both"))
			compiler <<= value;
		else if(key == "type" && (value == "debug" || value == "release"))
			type <<= value;
		else if(key == "tests")
			tests = value == "1";
		else if(key == "tsan")
			tsan = value == "1";
		else if(key == "asan")
			asan = value == "1";
		else if(key == "valgrind")
			valgrind = value == "1";
		else if(key == "helgrind")
			helgrind = value == "1";
		else if(key == "clean")
			clean = value == "1";
		else if(key == "compare")
			compare = value == "1";
		else if(key == "at_start")
			at_start = value == "1";
	}
}

void BuildWindow::Save()
{
	String path = BuildSettingsPath();
	RealizePath(path);
	SaveFile(path, "compiler=" + AsString(~compiler) + "\ntype=" + AsString(~type) +
	               "\ntests=" + AsString((int)tests) + "\ntsan=" + AsString((int)tsan) +
		               "\nasan=" + AsString((int)asan) + "\nvalgrind=" + AsString((int)valgrind) +
		               "\nhelgrind=" + AsString((int)helgrind) +
	               "\nclean=" + AsString((int)clean) + "\ncompare=" + AsString((int)compare) +
	               "\nat_start=" + AsString((int)at_start) + "\ndisplay=" + AsString(~display) + "\n");
}

// What this machine has, from bootstrap.sh --check (a fraction of a second): each
// requirement of setup/recipes.conf, and U++.
void BuildWindow::CheckRequirements()
{
	String script = BuildScript();
	if(script.IsEmpty()) {
		req_status = "Not run from a build tree: nothing to check.";
		check_again.Disable();
		fix.Disable();
		return;
	}
	// Visible even though it takes a fraction of a second: "Checking...", then the
	// result with the time it was checked, so a second press shows it ran again.
	req_status = "Checking...";
	req_status.SetInk(SColorText());
	check_again.Disable();
	Ctrl::ProcessEvents();
	String out;
	int code = Sys("bash " + ShellQuote(AppendFileName(GetFileFolder(script), "bootstrap.sh")) + " --check", out);
	check_again.Enable();
	Vector<String> missing, optional;
	int count = 0;
	for(const String& line : Split(out, '\n')) {
		Vector<String> f = Split(line, '\t', false);
		if(f.GetCount() < 5 || f[0] != "@req")
			continue;
		count++;
		if(f[2] == "missing")
			(f[3] == "required" ? missing : optional).Add(f[4]);
	}
	req_ok = code == 0 && count > 0;
	if(count == 0)
		req_status = "Could not check (bootstrap.sh --check gave nothing).";
	else if(req_ok)
		req_status = Format("All %d requirements present", count) +
		             (optional.GetCount() ? " (optional, missing: " + Join(optional, "; ") + ")" : String());
	else
		req_status = "Missing: " + Join(missing, "; ");
	req_status = req_status.GetText() + "  (checked " + ClockTime((int64)time(nullptr)) + ")";
	req_status.SetInk(req_ok ? Color(0, 128, 0) : LtRed());
	req_status.Tip(req_ok ? String() : "Missing: " + Join(missing, "\n"));
	fix.Enable(!req_ok || optional.GetCount());
}

// A terminal with ./bootstrap.sh typed at the prompt, waiting for Enter.
void BuildWindow::OpenFixTerminal()
{
	String opener = AppendFileName(GetFileFolder(BuildScript()), "setup/fix-in-terminal.sh");
	String out;
	if(Sys("bash " + ShellQuote(opener), out) != 0)
		Exclamation("Could not open a terminal: " + DeQtf(TrimBoth(out)) + "&&Run this in one yourself:&[C " +
		            DeQtf("cd " + GetFileFolder(BuildScript()) + " && ./bootstrap.sh") + "]");
}

void BuildWindow::Start()
{
	if(BuildScript().IsEmpty())
		return;
	if(!req_ok && !PromptYesNo("Some requirements are missing (see [* This machine]), so the build "
	                           "will probably fail.&&Build anyway?"))
		return;
	Save();
	Plan();
	steps_list.Clear();
	for(const BuildStep& s : steps)
		steps_list.Add(s.title, "waiting", "", "");
	output.Clear();
	results.Clear();
	facts.Clear();
	stopping = false;
	all_started = (int64)time(nullptr);
	step = -1;
	stop.Enable();
	Screen(2);
	NextStep();
}

void BuildWindow::NextStep()
{
	if(++step >= steps.GetCount() || stopping) {
		Finish();
		return;
	}
	BuildStep& s = steps[step];
	String script = s.compare ? AppendFileName(GetFileFolder(BuildScript()), "compare.sh") : BuildScript();
	Vector<String> args;
	args << "bash" << script;
	args.Append(s.args);
	Out("\n== " + s.title + ": " + GetFileName(script) + " " + Join(s.args, " ") + "\n");
	s.started = (int64)time(nullptr);
	steps_list.Set(step, 1, "running");
	partial.Clear();
	// Its own session, so Stop ends build.sh and the compilers it started.
	proc.Create();
	if(!proc->Start("/usr/bin/setsid", args, nullptr, GetFileFolder(script))) {
		Out("cannot start build.sh\n");
		s.code = 127;
		StepDone();
		return;
	}
	SetTimeCallback(-100, [this] { Poll(); }, POLL);
}

void BuildWindow::Poll()
{
	if(!proc)
		return;
	String s;
	while(proc->Read(s) && s.GetCount()) {
		Out(s);
		s.Clear();
	}
	if(step >= 0 && step < steps.GetCount()) {
		int64 now = (int64)time(nullptr);
		steps_list.Set(step, 2, DurationClock(now - steps[step].started).Mid(8));
		running = Format("Step %d of %d: %s  -  ", step + 1, steps.GetCount(), steps[step].title) +
		          TrimBoth(DurationWords(now - all_started)) + " so far";
	}
	if(!proc->IsRunning()) {
		KillTimeCallback(POLL);
		while(proc->Read(s) && s.GetCount()) {
			Out(s);
			s.Clear();
		}
		steps[step].code = proc->GetExitCode();
		proc.Clear();
		StepDone();
	}
}

// Adds output; keeps the last few thousand lines, and notes the test result line.
void BuildWindow::Out(const String& text)
{
	partial << text;
	int nl = partial.ReverseFind('\n');
	if(nl >= 0) {
		for(const String& line : Split(partial.Left(nl), '\n', false)) {
			if(step < 0 || step >= steps.GetCount())
				continue;
			if((line.StartsWith("passed: ") || line.StartsWith("FAILED: ")) && line.Find("checks") > 0)
				steps[step].checks = TrimBoth(line);
			if(line.StartsWith("@cmp\t")) {   // compare.sh: what<TAB>value
				Vector<String> f = Split(line.Mid(5), '\t', false);
				if(f.GetCount() >= 2)
					facts.GetAdd(steps[step].compiler).GetAdd(f[0]) = f[1];
			}
		}
		partial = partial.Mid(nl + 1);
	}
	output.Insert(output.GetLength(), text.ToWString());
	if(output.GetLineCount() > 6000)
		output.Remove(0, output.GetPos(output.GetLineCount() - 4000));
	output.SetCursor(output.GetLength());
}

void BuildWindow::StepDone()
{
	BuildStep& s = steps[step];
	s.stopped = (int64)time(nullptr);
	String state = stopping ? "stopped" : s.code == 0 ? "good" : "FAILED";
	steps_list.Set(step, 1, state);
	steps_list.Set(step, 2, DurationClock(s.stopped - s.started).Mid(8));
	steps_list.Set(step, 3, s.checks);
	if(s.code != 0)
		steps_list.SetLineColor(step, Color(255, 215, 215));
	if(compare && !s.compare && !s.Checked()) {   // the build itself, for the table
		VectorMap<String, String>& f = facts.GetAdd(s.compiler);
		f.GetAdd(String("build") + (clean ? " (clean)" : " (changes only)") + ": time (s)") = AsString(s.stopped - s.started);
		f.GetAdd("build: result") = s.code == 0 ? String("good") : "FAILED (exit " + AsString(s.code) + ")";
		if(s.checks.GetCount())
			f.GetAdd("tests") = s.checks;
	}
	NextStep();
}

void BuildWindow::Finish()
{
	stop.Disable();
	int64 ended = (int64)time(nullptr);
	String text = ResultsHeader(all_started);
	bool good = !stopping;
	for(const BuildStep& s : steps) {
		if(s.code < 0)
			continue;   // not reached (stopped)
		good = good && s.code == 0;
		text << ResultsLine(s.started, s.stopped, s.title, s.code,
		                    stopping && &s == &steps[min(step, steps.GetCount() - 1)] ? "Stopped"
		                    : s.code == 0 ? "Good" : "Failed", s.checks) << "\n";
	}
	text << LogStamp(all_started, ended) << "End of build: " << (good ? "all good" : stopping ? "stopped" : "FAILED");
	String here = GetFileName(GetExeFolder());   // gcc or clang: the build this program came from
	bool rebuilt_me = false;
	for(const BuildStep& s : steps)
		rebuilt_me = rebuilt_me || (s.code == 0 && s.args.GetCount() >= 2 && s.args[1] == here && !s.Checked());
	ShowCompare();
	if(facts.GetCount())
		text << "\n\n" << CompareText();
	String kept = text;   // for Build-Results.txt: without the note below
	if(rebuilt_me)
		text << "\n\nThis program (" << here << ") was rebuilt: " <<
		        (startup ? "the Continue buttons start the new builds." : "close TreeSync and start it again to use the new build.");
	UseButtons();
	results <<= text;
	// Kept: Build-Results.txt in the logs folder, next to the sync runs' Results.txt.
	String path = AppendFileName(LogFolder(DefaultDbPath()), "Build-Results.txt");
	RealizePath(path);
	bool fresh = !FileExists(path);
	FileAppend fa(path);
	if(fa) {
		if(fresh)
			fa << "\n" << ResultsHeader(all_started);
		fa << kept.Mid(kept.Find("=\n") + 2) << "\n";   // the step lines (and table), without the header
	}
	if(auto_run) {   // --build-run: say how it went and close
		Cout() << text << "\n";
		SetExitCode(good ? 0 : 1);
		PostCallback([this] { TopWindow::Close(); });
	}
	running = good ? "Done: all good." : stopping ? "Stopped." : "Done, with failures: see the red steps.";
	Screen(3);
}

// The compilers side by side: every "what" any of them reported, in the order first
// seen; rows whose values differ are marked.
void BuildWindow::ShowCompare()
{
	compare_list.Clear();
	split3.Zoom(facts.IsEmpty() ? 1 : -1);
	if(facts.IsEmpty())
		return;
	Index<String> rows;
	for(const VectorMap<String, String>& f : facts)
		for(const String& k : f.GetKeys())
			rows.FindAdd(k);
	const char *cols[] = { "gcc", "clang" };
	for(const String& r : rows) {
		Vector<Value> line;
		line.Add(r.StartsWith("feature:") ? r.Mid(8) : r);
		Index<String> values;
		for(const char *c : cols) {
			int q = facts.Find(c);
			String v = q >= 0 ? facts[q].Get(r, "") : String();
			line.Add(v);
			if(q >= 0)
				values.FindAdd(v);
		}
		compare_list.Add(line);
		if(values.GetCount() > 1)
			compare_list.SetLineColor(compare_list.GetCount() - 1, Color(255, 240, 190));
	}
}

// The same table as text, for Build-Results.txt; "*" marks a difference.
String BuildWindow::CompareText()
{
	String t = " Compare                                  : gcc                                      : clang\n";
	for(int i = 0; i < compare_list.GetCount(); i++) {
		auto pad = [](const String& s, int w) { return s.GetCount() >= w ? s : s + String(' ', w - s.GetCount()); };
		bool differs = facts.GetCount() > 1 && compare_list.Get(i, 1) != compare_list.Get(i, 2);
		t << (differs ? "*" : " ") << pad(AsString(compare_list.Get(i, 0)), 40) << " : "
		  << pad(AsString(compare_list.Get(i, 1)), 40) << " : " << AsString(compare_list.Get(i, 2)) << "\n";
	}
	return t;
}

void BuildWindow::Stop()
{
	stopping = true;
	if(proc && proc->IsRunning())
		kill(-proc->GetPid(), SIGTERM);
}

void BuildWindow::Close()
{
	if(proc && proc->IsRunning()) {
		if(!PromptYesNo("A build is running. Stop it and close?"))
			return;
		Stop();
		for(int i = 0; i < 50 && proc->IsRunning(); i++)
			Sleep(100);
	}
	TopWindow::Close();
}

void BuildWindow::Snap(const String& folder)
{
	Screen(1);
	SaveScreenshot(*this, AppendFileName(folder, "build-options.png"));
	Plan();
	steps_list.Clear();
	for(int i = 0; i < steps.GetCount(); i++)
		steps_list.Add(steps[i].title, i == 0 ? "good" : i == 1 ? "running" : "waiting",
		               i == 0 ? "00:03:12" : i == 1 ? "00:01:05" : "", i == 0 ? "passed: 214/214 checks" : "");
	running = "Step 2 of " + AsString(steps.GetCount()) + ": " + (steps.GetCount() > 1 ? steps[1].title : String()) +
	          "  -  4 minutes 17 seconds so far";
	output <<= "Building TreeSyncCli with clang (bm/CLANG26.bm)\nTreeSyncCli: 17 file(s) built in (0:41.20)\n";
	Screen(2);
	SaveScreenshot(*this, AppendFileName(folder, "build-running.png"));
	int64 t = (int64)time(nullptr) - 600;
	results <<= ResultsHeader(t) +
	            ResultsLine(t, t + 192, "gcc debug build + tests", 0, "Good", "passed: 214/214 checks") + "\n" +
	            ResultsLine(t + 192, t + 400, "clang debug build + tests", 0, "Good", "passed: 214/214 checks") + "\n" +
	            LogStamp(t, t + 400) + "End of build: all good";
	facts.Clear();
	for(int i = 0; i < 2; i++) {
		VectorMap<String, String>& f = facts.GetAdd(i ? "clang" : "gcc");
		f.Add("build (clean): time (s)", i ? "21" : "37");
		f.Add("tests", "passed: 214/214 checks");
		f.Add("size: treesync (MiB)", i ? "33.3" : "40.3");
		f.Add("feature:__cpp_structured_bindings", i ? "202411" : "202403");
		f.Add("feature:__cpp_pack_indexing", "202311");
		f.Add("bench:sha256 MiB/s", i ? "158" : "174");
		f.Add("probe: import std works", "yes");
		f.Add("probe: import std compile (s)", i ? "0.26" : "0.58");
	}
	ShowCompare();
	Screen(3);
	SaveScreenshot(*this, AppendFileName(folder, "build-results.png"));
	Screen(1);
}

// The first screen when TreeSync starts: instead of Close, one Continue button per
// compiler, each starting that compiler's build of TreeSync (the newest one).
void BuildWindow::Startup()
{
	startup = true;
	Title("TreeSync - build options");
	SetRect(0, 0, Zx(1000), Zy(760));
	// Bottom row, right to left: Exit, Continue (Clang), Continue (GCC), Build (with the
	// tests when "Run the tests" is ticked).
	start.SetLabel("Build").RightPosZ(612, 140);
	exit1.Show();
	exit3.Show();
	at_start.SetLabel("Show this page at start").LeftPosZ(8, 220);
	close1.Hide();
	close3.Hide();
	UseButtons();
	if(!req_ok)   // missing requirements: the fix, ready to run, as soon as the page is up
		PostCallback([this] { OpenFixTerminal(); });
}

// Each Continue button needs its program built.
void BuildWindow::UseButtons()
{
	if(!startup)
		return;
	for(Button *b : { &use_gcc1, &use_clang1, &use_gcc3, &use_clang3 }) {
		String c = b == &use_gcc1 || b == &use_gcc3 ? "gcc" : "clang";
		bool built = FileExists(TreeSyncExe(c));
		b->Show();
		b->RightPosZ(c == "gcc" ? 354 : 96, 250);
		b->Enable(built);
		b->Tip(built ? "Starts " + TreeSyncExe(c) : "Not built yet: Build with " + c + " first");
	}
}

bool CanBuild()
{
	return BuildScript().GetCount() && !IsRoot();
}

void ShowBuild()
{
	if(CanBuild())
		BuildWindow().Execute();
}

bool BuildPageAtStart()
{
	if(!CanBuild())   // installed, or root: nothing to build here
		return false;
	for(const String& line : Split(LoadFile(BuildSettingsPath()), '\n'))
		if(line == "at_start=0")
			return false;
	return true;
}

// The first screen: compiler and test options, Build, then Continue with the
// gcc or the clang build. Returns which ("gcc" or "clang"); empty for Exit (or the
// window's close box): quit TreeSync.
String BuildStartPage()
{
	BuildWindow w;
	w.Startup();
	w.Execute();
	return w.continue_with;
}

// --build-run (a developer aid): the saved choices, built and tested for real.
void BuildRun()
{
	if(BuildScript().IsEmpty()) {
		Cerr() << "build.sh is not two folders above this program (" << GetExeFolder() << ")\n";
		SetExitCode(2);
		return;
	}
	BuildWindow w;
	w.auto_run = true;
	w.Open();
	w.Run();
	w.Execute();
}

void BuildScreenshots(const String& folder)
{
	{
		BuildWindow w;
		w.Snap(folder);
	}
	BuildWindow w;   // as the first screen when TreeSync starts
	w.Startup();
	SaveScreenshot(w, AppendFileName(folder, "build-startup.png"));
}
