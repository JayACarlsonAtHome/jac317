// treesync-cli: runs a sync in the terminal, no GUI. Same options as treesync.
// Ctrl+C once: stop after the current file or chunk (the run can be resumed).
// Ctrl+C twice: quit at once.
// --stop-on-eof (used by the GUI): stop the same way when stdin is closed. The GUI
// can always close a pipe, even when pkexec made this a root process it may not signal.
// --gui-progress (used by the GUI): also print progress as tagged lines,
//   @file <TAB> id <TAB> done <TAB> size <TAB> seconds <TAB> index <TAB> count <TAB> name [<TAB> verify]
//   @overall <TAB> done <TAB> total
//   @stage <TAB> stage <TAB> done <TAB> total <TAB> finished <TAB> detail   (0 pre-scan, 2 post-scan)
//   @net <TAB> down <TAB> host <TAB> wait   (1: lost the connection, waiting up to wait s; 0: back)
// which the GUI turns into thermometers instead of log lines.
// Exit codes: 0 in sync, 1 failures or conflicts, 2 usage or setup error, 3 stopped.
#include <TreeSyncCore/TreeSyncCore.h>

#include <csignal>
#include <format>
#include <pthread.h>
#include <thread>
#include <unistd.h>

static std::atomic<bool> stop_requested;

static void OnSignal(int)
{
	if(stop_requested.exchange(true))
		_exit(130);
	const char msg[] = "\nstopping after the current file (Ctrl+C again to quit now)...\n";
	if(write(2, msg, sizeof(msg) - 1) < 0)
		;
}

static bool tty = isatty(1);
static bool status_shown;
static FileAppend run_log;   // every printed line, in the run's log file

static void Print(const String& line)
{
	if(status_shown)
		Cout() << "\r\x1b[K";
	status_shown = false;
	Cout() << line << "\n";
	Cout().Flush();
	if(run_log.IsOpen()) {
		run_log << line << "\n";
		run_log.Flush();
	}
}

static void Status(const String& s)
{
	if(!tty)
		return;
	Cout() << "\r\x1b[K" << s.Left(150);
	Cout().Flush();
	status_shown = true;
}

// --stop-on-eof: waits for stdin to close, then stops like Ctrl+C. It is never
// joined (it sits in read() until the end), so it is a plain pthread: a std::thread
// keeps its state on U++'s heap, and the debug leak check would report it at exit.
static void *WatchStdin(void *)
{
	char c;
	while(read(0, &c, 1) > 0)
		;
	OnSignal(0);
	return nullptr;
}

CONSOLE_APP_MAIN
{
	Vector<String> args = clone(CommandLine());
	bool stop_on_eof = false, compare_ids = false;
	static bool gui_progress = false;
	for(int i = args.GetCount() - 1; i >= 0; i--)
		if(args[i] == "--stop-on-eof" || args[i] == "--compare-ids" || args[i] == "--gui-progress") {
			(args[i] == "--compare-ids" ? compare_ids : args[i] == "--stop-on-eof" ? stop_on_eof : gui_progress) = true;
			args.Remove(i);
		}

	SyncSetup setup;
	String error;
	switch(ParseCommandLine(args, setup, error)) {
	case PARSE_HELP:
		Cout() << Usage();
		return;
	case PARSE_ERROR:
		Cerr() << "treesync-cli: " << error << "\nTry '--help' for more information.\n";
		SetExitCode(2);
		return;
	}
	if(!IsRoot())
		Cerr() << "treesync-cli: " << Join(Split(NotRootMessage(), '\n'), "\n  ") << "\n";

	struct sigaction sa = {};
	sa.sa_handler = OnSignal;
	sigaction(SIGINT, &sa, nullptr);
	sigaction(SIGTERM, &sa, nullptr);
	if(stop_on_eof) {
		pthread_t t;
		if(pthread_create(&t, nullptr, WatchStdin, nullptr) == 0)
			pthread_detach(t);
	}

	SyncReport report;
	report.stop = &stop_requested;
	report.WhenLine = [](const String& s) { Print(s); };
	report.WhenStatus = [](const String& s) { Status(s); };
	report.WhenProgress = [](const CopyProgress& p) {
		if(gui_progress) {
			String name = p.name;
			name.Replace("\t", " ");
			name.Replace("\n", " ");
			Cout() << std::format("@file\t{}\t{}\t{}\t{:.2f}\t{}\t{}\t", p.id, p.done, p.size, p.seconds,
			                      p.index, std::to_string(p.count) + (p.more ? "+" : "")) << name << (p.verifying ? "\tverify" : "") << "\n";
		}
		if(p.done >= p.size && !p.verifying)
			Print(ProgressLine(p));
		else
			Status(ProgressLine(p, 100, true));
	};
	report.WhenStage = [](const StageProgress& s) {
		if(gui_progress) {
			String d = s.detail;
			d.Replace("\t", " ");
			d.Replace("\n", " ");
			Cout() << std::format("@stage\t{}\t{}\t{}\t{}\t", s.stage, s.done, s.total, s.finished ? 1 : 0)
			       << d << "\n";
		}
		if(s.finished)
			Print(s.stage == 0 ? Format("pre-scan done: %s entries", AsString(s.done))
			                   : Format("post-scan done: %s folders checked", AsString(s.done)));
	};
	// The messages themselves come as lines; this is for the GUI's banner.
	report.WhenNetwork = [&](bool down, const String& host) {
		if(gui_progress)
			Cout() << std::format("@net\t{}\t{}\t{}\n", down ? 1 : 0, host.ToStd(), setup.flags.network_wait);
	};
	report.WhenOverall = [](int64 done, int64 total) {
		if(gui_progress)
			Cout() << std::format("@overall\t{}\t{}\n", done, total);
	};

	if(compare_ids) {
		String out;
		if(!CompareSides(setup, report, out, error)) {
			Print("treesync-cli: " + error);
			SetExitCode(2);
			return;
		}
		Cout() << out;
		return;
	}

	// The run's log file: logs/YYYY-MM-DD_HHMMSS.log next to the database.
	String log_path;
	{
		Time t = GetSysTime();
		String folder = LogFolder(setup.db);
		RealizeDirectory(folder);
		log_path = AppendFileName(folder, Format("%04d-%02d-%02d_%02d%02d%02d.log",
		                                         (int)t.year, (int)t.month, (int)t.day,
		                                         (int)t.hour, (int)t.minute, (int)t.second));
		for(int n = 1; FileExists(log_path); n++)   // two runs in the same second
			log_path = log_path.Left(log_path.GetCount() - 4) + "." + AsString(n) + ".log";
		run_log.Open(log_path);
	}
	Print(CommandLineText(ToArgs(setup)));
	Print("log: " + log_path);
	SyncResult result;
	int64 started = (int64)time(nullptr);
	// The start goes at the top too; stop and duration come in the table at the end.
	Print("started: " + DateTimeText(started));
	bool ok = RunSync(setup, report, result, error);
	SetRunLog(setup.db, result.run, log_path);
	int64 ended = (int64)time(nullptr);
	int code = 0;
	String status, summary;
	if(!ok) {
		Print("treesync-cli: " + error);
		code = 2;
		status = "Error";
		summary = error;
	}
	else if(result.stopped) {
		if(result.run)
			Print(Format("stopped: %d of %d actions done; run treesync-cli again with the same options to resume",
			             result.done, result.planned));
		else
			Print("stopped while scanning; the hashes finished so far are kept, so the next run is quicker");
		code = 3;
		status = "Stopped";
		summary = Format("%d of %d actions done", result.done, result.planned);
	}
	else {
		code = result.failed || result.conflicts ? 1 : 0;
		status = code ? "Failures or conflicts" : "Good";
		summary = Format("%d actions, %d failed, %d conflicts, %s copied", result.done, result.failed,
		                 result.conflicts, FormatBytes(result.bytes));
		if(!setup.flags.dry_run)
			Print(Format("finished run %d: %d of %d actions done, %d failed, %d conflicts, %s copied",
			             result.run, result.done, result.planned, result.failed, result.conflicts,
			             FormatBytes(result.bytes)));
	}
	Print("took " + DurationWords(ended - started) + " (" + DurationClock(ended - started) + "), from " +
	      ClockTime(started) + " to " + ClockTime(ended));

	// Start, stop and duration in the layout of the j5c build Results.txt: at the end
	// of this run's log, and one line per run in logs/Results.txt.
	auto where = [](const TreeEndpoint& e) {
		return e.remote ? Nvl(e.user, GetUserName()) + "@" + e.host + ":" + e.path : e.path;
	};
	const char *mode = setup.flags.mode == SyncMode::Mirror ? "Mirror" : setup.flags.mode == SyncMode::Merge
	                   ? "Merge" : "Copy";
	String what = setup.flags.dry_run ? String(mode) + " dry run"
	            : result.run ? String(mode) + " run " + AsString(result.run) : String(mode);
	String line = ResultsLine(started, ended, what, code, status,
	                          where(setup.source) + " -> " + where(setup.target) + "  (" + summary + ")");
	Print("");
	Print(ResultsHeader(started) + line);
	String results = AppendFileName(LogFolder(setup.db), "Results.txt");
	bool fresh = !FileExists(results);
	FileAppend fa(results);
	if(fa) {
		if(fresh)
			fa << "\n" << ResultsHeader(started);
		fa << line << "\n";
	}
	SetExitCode(code);
}
