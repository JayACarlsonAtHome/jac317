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
static int64 run_started = -1;   // NowMicros(): lines about the whole run span from here

// A line for the terminal and the run's log. In the log every line starts with
// start : stop : duration (the j5c Results.txt columns): for a copied file, from
// when its copy began to when it finished; for anything else, the moment it happened.
// To the microsecond (start in NowMicros() units). stamp false: as it is (the results
// table at the end).
static void Print(const String& line, int64 start = -1, bool stamp = true)
{
	if(status_shown)
		Cout() << "\r\x1b[K";
	status_shown = false;
	Cout() << line << "\n";
	Cout().Flush();
	if(run_log.IsOpen()) {
		int64 now = NowMicros();
		if(stamp)
			run_log << LogStampFine(start < 0 ? now : min(start, now), now);
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
	// Developer switches for comparing compilers (the Build window's compare step):
	// facts about this build, one "key<TAB>value" per line.
	if(args.GetCount() == 1 && (args[0] == "--toolchain" || args[0] == "--bench")) {
		Cout() << (args[0] == "--toolchain" ? ToolchainFacts() : BenchFacts());
		return;
	}
	// --export-ai-logs[=RUN] / --export-ai-sql[=RUN] [--db=PATH]: a run as Markdown for an
	// AI to analyse (RUN: a number; default the newest). Prints where it went.
	{
		String db;
		Vector<String> what;
		Vector<int64> runs;
		for(const String& a : args)
			if(a.StartsWith("--db="))
				db = a.Mid(5);
			else if(a.StartsWith("--export-ai-logs") || a.StartsWith("--export-ai-sql")) {
				int eq = a.Find('=');
				what.Add(a.Mid(2, (eq < 0 ? a.GetCount() : eq) - 2));
				runs.Add(eq < 0 ? 0 : ScanInt64(a.Mid(eq + 1)));
			}
		if(what.GetCount()) {
			for(int i = 0; i < what.GetCount(); i++) {
				String out, error;
				bool ok = what[i] == "export-ai-logs" ? ExportAiLogs(db, runs[i], out, error)
				                                      : ExportAiSql(db, runs[i], out, error);
				if(ok)
					Cout() << out << "\n";
				else {
					Cerr() << "treesync-cli: " << error << "\n";
					SetExitCode(2);
				}
			}
			return;
		}
	}
	bool stop_on_eof = false, compare_ids = false, scan_only = false, sync_attrs = false, clear_old = false;
	static bool gui_progress = false;
	for(int i = args.GetCount() - 1; i >= 0; i--) {
		bool *flag = args[i] == "--compare-ids" ? &compare_ids : args[i] == "--stop-on-eof" ? &stop_on_eof
		           : args[i] == "--gui-progress" ? &gui_progress : args[i] == "--scan-only" ? &scan_only
		           : args[i] == "--sync-attrs" ? &sync_attrs : args[i] == "--clear-old" ? &clear_old : nullptr;
		if(flag) {
			*flag = true;
			args.Remove(i);
		}
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
	report.WhenLine = [](const String& s) { Print(s, s.Find("] Total") >= 0 ? run_started : -1); };
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
			Print(ProgressLine(p), NowMicros() - (int64)(p.seconds * 1e6));
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
			                   : Format("post-scan done: %s folders checked", AsString(s.done)), run_started);
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
	Vector<String> shown = ToArgs(setup);
	if(scan_only) shown.Add("--scan-only");
	if(sync_attrs) shown.Add("--sync-attrs");
	if(clear_old) shown.Add("--clear-old");
	Print(CommandLineText(shown));
	Print("log: " + log_path);
	SyncResult result;
	run_started = NowMicros();
	int64 started = run_started / 1000000;
	// The start goes at the top too; stop and duration come in the table at the end.
	Print("started: " + DateTimeText(started));
	CheckResult check;
	bool ok = scan_only || sync_attrs ? RunCheck(setup, scan_only ? CheckMode::Scan : CheckMode::Attrs, report, check, error)
	        : clear_old ? ClearOld(setup, setup.flags.dry_run, report, error)
	        : RunSync(setup, report, result, error);
	if(scan_only || sync_attrs) {
		result.run = check.run;
		result.stopped = check.stopped;
		result.failed = check.failed;
	}
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
	else if(result.stopped && (scan_only || sync_attrs)) {
		Print("stopped: the rows so far are in the database; run it again to check everything");
		code = 3;
		status = "Stopped";
		summary = Format("%d files checked", check.files);
	}
	else if(scan_only || sync_attrs) {
		// Scan only: 1 when the trees are not the same (content, missing files) or reads failed.
		code = check.failed || (scan_only && (check.differ || check.missing)) ? 1 : 0;
		status = code ? (scan_only ? "Differences" : "Failures") : "Good";
		summary = scan_only ? Format("%d files: %d same, %d differ, %d missing, %d failed; %d attribute differences",
		                             check.files, check.same, check.differ, check.missing, check.failed, check.attr_diffs)
		                    : Format("%d of %d attribute differences %s, %d failed", check.fixed, check.attr_diffs,
		                             setup.flags.dry_run ? "would be set" : "set", check.failed);
	}
	else if(clear_old) {
		status = "Good";
		summary = setup.flags.dry_run ? "counted" : "cleared";
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
			             FormatBytes(result.bytes)), run_started);
	}
	Print("took " + DurationWords(ended - started) + " (" + DurationClock(ended - started) + "), from " +
	      ClockTime(started) + " to " + ClockTime(ended), run_started);

	// Start, stop and duration in the layout of the j5c build Results.txt: at the end
	// of this run's log, and one line per run in logs/Results.txt.
	auto where = [](const TreeEndpoint& e) {
		return e.remote ? Nvl(e.user, GetUserName()) + "@" + e.host + ":" + e.path : e.path;
	};
	const char *mode = setup.flags.mode == SyncMode::Mirror ? "Mirror" : setup.flags.mode == SyncMode::Merge
	                   ? "Merge" : "Copy";
	String what = scan_only ? "Scan only run " + AsString(result.run)
	            : sync_attrs ? String(setup.flags.dry_run ? "Attrs dry run " : "Sync attrs run ") + AsString(result.run)
	            : clear_old ? String(setup.flags.dry_run ? "Clear .old (count)" : "Clear .old")
	            : setup.flags.dry_run ? String(mode) + " dry run"
	            : result.run ? String(mode) + " run " + AsString(result.run) : String(mode);
	String line = ResultsLine(started, ended, what, code, status,
	                          where(setup.source) + " -> " + where(setup.target) + "  (" + summary + ")");
	Print("", -1, false);
	Print(ResultsHeader(started) + line, -1, false);
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
