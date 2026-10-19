// treesync-cli: runs a sync in the terminal, no GUI. Same options as treesync.
// Ctrl+C once: stop after the current file or chunk (the run can be resumed).
// Ctrl+C twice: quit at once.
// --stop-on-eof (used by the GUI): stop the same way when stdin is closed. The GUI
// can always close a pipe, even when pkexec made this a root process it may not signal.
// --home (used by TreeSyncHome): run as yourself, inside your home, even when an
// administrator starts the installed copy (which otherwise starts itself through sudo).
// --root-session (used by TreeSyncRoot): one root process for a whole window, so the
// password is asked once (pkexec) and not at every run; see RootSession below.
// --gui-progress (used by the GUI): also print progress as tagged lines,
//   @file <TAB> id <TAB> done <TAB> size <TAB> seconds <TAB> index <TAB> count <TAB> name [<TAB> verify]
//   @overall <TAB> done <TAB> total
//   @stage <TAB> stage <TAB> done <TAB> total <TAB> finished <TAB> detail   (0 pre-scan, 2 post-scan)
//   @net <TAB> down <TAB> host <TAB> wait   (1: lost the connection, waiting up to wait s; 0: back)
//   @free <TAB> side <TAB> free <TAB> total <TAB> exact   (0 source, 1 target; exact: just asked,
//         else worked out from what was written since; at most once a second)
//   @proof <TAB> line   (the end-of-run check of the copies, again, for the GUI's popup)
// which the GUI turns into thermometers instead of log lines.
// Exit codes: 0 in sync, 1 failures or conflicts, 2 usage or setup error, 3 stopped
// (also by itself, when the target disk was nearly full), 4 may not fit (--precheck:
// nothing was copied; --no-precheck starts anyway).
#include <TreeSyncCore/TreeSyncCore.h>

#include <csignal>
#include <format>
#include <pthread.h>
#include <thread>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <cerrno>
#include <cstring>
#include <vector>

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

// The installed treesync-cli (install.sh: owned by root, only root may change it) runs
// as root for an administrator: started by one, it starts itself again through sudo.
// Anyone else runs it as themselves, inside their own home; so does the build in out/,
// and so does a run started with --home (TreeSyncHome).
static void AsRootIfInstalled(const Vector<String>& args)
{
	struct stat st;
	String self = GetExeFilePath();
	if(IsRoot() || !IsAdmin() || stat(self, &st) != 0 || st.st_uid != 0 || (st.st_mode & 022))
		return;
	for(const String& a : args)
		if(a == "--help" || a == "-h" || a == "--home")
			return;
	std::vector<char *> argv;
	argv.push_back((char *)"sudo");
	argv.push_back((char *)~self);
	for(const String& a : args)
		argv.push_back((char *)~a);
	argv.push_back(nullptr);
	execv("/usr/bin/sudo", argv.data());
	Cerr() << "treesync-cli: cannot start sudo (" << strerror(errno) << "); run it as root\n";
	exit(2);
}

static bool ReadAll(int fd, void *data, size_t size)
{
	for(char *p = (char *)data; size;) {
		ssize_t n = read(fd, p, size);
		if(n < 0 && errno == EINTR)
			continue;
		if(n <= 0)
			return false;
		p += n;
		size -= n;
	}
	return true;
}

// --root-session: started by TreeSyncRoot through pkexec, with a Unix socket as stdin.
// Each request is a run: 4 bytes of length, then its arguments, each ending in a NUL,
// with two file descriptors attached (the run's stdin and stdout, pipes of the GUI).
// It runs this program again with those arguments and answers with the 4-byte exit
// code. It only ever runs itself, one run at a time, and ends when the GUI closes the
// socket (the window closed, or the GUI died).
static int RootSession()
{
	if(!IsRoot()) {
		Cerr() << "treesync-cli: --root-session is for TreeSyncRoot, through pkexec\n";
		return 2;
	}
	String self = GetExeFilePath();
	for(;;) {
		uint32 size = 0;
		alignas(cmsghdr) char ctl[CMSG_SPACE(2 * sizeof(int))];
		iovec iov = { &size, sizeof(size) };
		msghdr m = {};
		m.msg_iov = &iov;
		m.msg_iovlen = 1;
		m.msg_control = ctl;
		m.msg_controllen = sizeof(ctl);
		ssize_t n = recvmsg(0, &m, MSG_CMSG_CLOEXEC | MSG_WAITALL);
		if(n < 0 && errno == EINTR)
			continue;
		if(n != sizeof(size))
			return 0;   // the GUI is gone
		int fds[2] = { -1, -1 }, nfd = 0;
		for(cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c))
			if(c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS)
				for(size_t i = 0; i < (c->cmsg_len - CMSG_LEN(0)) / sizeof(int); i++) {
					int fd;
					memcpy(&fd, CMSG_DATA(c) + i * sizeof(int), sizeof(int));
					if(nfd < 2)
						fds[nfd++] = fd;
					else
						close(fd);
				}
		if(size > (1u << 24))
			return 0;   // not a request of ours
		StringBuffer buf((int)size);
		if(!ReadAll(0, buf.Begin(), size))
			return 0;
		bool ok = nfd == 2 && !(m.msg_flags & MSG_CTRUNC);
		int32 code = 2;
		if(ok) {
			String all = buf;
			Vector<String> args;   // not Split: it reads a C string, which ends at the first NUL
			for(int at = 0, end; at < all.GetCount(); at = end + 1) {
				end = all.Find('\0', at);
				if(end < 0)
					end = all.GetCount();
				args.Add(all.Mid(at, end - at));
			}
			std::vector<char *> argv;
			argv.push_back((char *)~self);
			for(const String& a : args)
				argv.push_back((char *)~a);
			argv.push_back(nullptr);
			pid_t pid = fork();
			if(pid == 0) {
				dup2(fds[0], 0);
				dup2(fds[1], 1);
				dup2(fds[1], 2);
				signal(SIGPIPE, SIG_DFL);
				execv(self, argv.data());
				_exit(127);
			}
			int st = 0;
			while(pid > 0 && waitpid(pid, &st, 0) < 0 && errno == EINTR)
				;
			code = pid < 0 ? 2 : WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
		}
		for(int fd : fds)
			if(fd >= 0)
				close(fd);
		if(send(1, &code, sizeof(code), MSG_NOSIGNAL) != sizeof(code))
			return 0;
	}
}

CONSOLE_APP_MAIN
{
	Vector<String> args = clone(CommandLine());
	if(args.GetCount() == 1 && args[0] == "--root-session") {
		SetExitCode(RootSession());
		return;
	}
	AsRootIfInstalled(args);
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
	bool home = false;   // only read before this point (AsRootIfInstalled)
	static bool gui_progress = false;
	static bool checking = false;   // Scan only / Sync attributes: the pre-scan counts folders
	for(int i = args.GetCount() - 1; i >= 0; i--) {
		bool *flag = args[i] == "--compare-ids" ? &compare_ids : args[i] == "--stop-on-eof" ? &stop_on_eof
		           : args[i] == "--gui-progress" ? &gui_progress : args[i] == "--scan-only" ? &scan_only
		           : args[i] == "--sync-attrs" ? &sync_attrs : args[i] == "--clear-old" ? &clear_old
		           : args[i] == "--home" ? &home : nullptr;
		if(flag) {
			*flag = true;
			args.Remove(i);
		}
	}

	// --edit=OP --edit-side=source|target --edit-path=REL [--edit-name=NAME]: one change
	// to a tree (the GUI's right-click menu); see TreeEdit.
	String edit, edit_side, edit_path, edit_name;
	for(int i = args.GetCount() - 1; i >= 0; i--) {
		String *v = args[i].StartsWith("--edit=") ? &edit : args[i].StartsWith("--edit-side=") ? &edit_side
		          : args[i].StartsWith("--edit-path=") ? &edit_path : args[i].StartsWith("--edit-name=") ? &edit_name
		          : nullptr;
		if(v) {
			*v = args[i].Mid(args[i].Find('=') + 1);
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

	checking = scan_only || sync_attrs;
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
			                      p.index, std::to_string(p.count) + (p.more ? "+" : "")) << name
			       << (p.verifying ? "\tverify" : p.check ? "\tcheck" : "") << "\n";
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
			Print(s.stage == 0 ? Format(checking ? "pre-scan done: %s folders listed" : "pre-scan done: %s entries", AsString(s.done))
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
	// The disk's real free space (at the start and the end) goes in the log; the worked-out
	// one in between only to the GUI.
	report.WhenFree = [](int side, int64 free, int64 total, bool exact) {
		if(gui_progress)
			Cout() << std::format("@free\t{}\t{}\t{}\t{}\n", side, free, total, exact ? 1 : 0);
		if(exact)
			Print(String(side ? "target" : "source") + " disk: " + FormatBytes(free) + " free of " + FormatBytes(total));
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
		run_log << LogHeader((int64)time(nullptr));
	}
	Vector<String> shown = ToArgs(setup);
	if(scan_only) shown.Add("--scan-only");
	if(sync_attrs) shown.Add("--sync-attrs");
	if(clear_old) shown.Add("--clear-old");
	if(edit.GetCount()) {
		shown.Add("--edit=" + edit);
		shown.Add("--edit-side=" + edit_side);
		shown.Add("--edit-path=" + edit_path);
		if(edit_name.GetCount())
			shown.Add("--edit-name=" + edit_name);
	}
	Print(CommandLineText(shown));
	Print("log: " + log_path);
	SyncResult result;
	run_started = NowMicros();
	int64 started = run_started / 1000000;
	// The start goes at the top too; stop and duration come in the table at the end.
	Print("started: " + DateTimeText(started));
	CheckResult check;
	ClearResult cleared;
	EditResult edited;
	if(edit.GetCount() && edit_side != "source" && edit_side != "target") {
		Print("treesync-cli: --edit-side must be source or target");
		SetExitCode(2);
		return;
	}
	bool ok = edit.GetCount() ? TreeEdit(setup, edit_side == "target", edit, edit_path, edit_name, report, edited, error)
	        : scan_only || sync_attrs ? RunCheck(setup, scan_only ? CheckMode::Scan : CheckMode::Attrs, report, check, error)
	        : clear_old ? ClearOld(setup, setup.flags.dry_run, report, error, &cleared)
	        : RunSync(setup, report, result, error);
	if(scan_only || sync_attrs) {
		result.run = check.run;
		result.stopped = check.stopped;
		result.failed = check.failed;
	}
	SetRunLog(setup.db, result.run, log_path);
	// The copies, as checked: bytes and SHA-256 of both sides. With --gui-progress each
	// line also goes out tagged @proof, for the GUI's end-of-run popup.
	auto copy_report = [&] {
		for(const String& l : CopyReport(result.copies, result.bytes)) {
			Print(l);
			if(gui_progress) {
				Cout() << "@proof\t" << l << "\n";
				Cout().Flush();
			}
		}
	};
	int64 ended = (int64)time(nullptr);
	int code = 0;
	String status, summary;
	if(!ok && result.no_room) {   // nothing copied; --no-precheck starts anyway
		Print("treesync-cli: " + error);
		code = 4;
		status = "May not fit";
		summary = Format("needs up to %s, %s free", FormatBytes(result.room_need), FormatBytes(result.room_free));
	}
	else if(!ok) {
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
	else if(edit.GetCount()) {
		status = "Good";
		summary = edit_side + ": " + edited.done;
	}
	else if(clear_old) {   // a count only is not a clear: the status says which it was
		status = cleared.found == 0 ? "Nothing to clear" : setup.flags.dry_run ? "Counted, not deleted" : "Good";
		summary = cleared.found == 0 ? String("no .old folder")
		        : Format("%d files, %d folders, %s %s", cleared.files, cleared.folders, FormatBytes(cleared.bytes),
		                 setup.flags.dry_run ? "would be deleted (--no-dry-run deletes them)" : "deleted");
	}
	else if(result.stopped) {
		if(result.run)
			Print(Format("stopped: %d of %d actions done; run treesync-cli again with the same options to resume",
			             result.done, result.planned));
		else
			Print("stopped while scanning; the hashes finished so far are kept, so the next run is quicker");
		copy_report();
		code = 3;
		status = result.disk_full ? "Stopped: disk full" : "Stopped";
		summary = Format("%d of %d actions done", result.done, result.planned);
		if(result.disk_full)
			summary << "; the disk was nearly full";
		if(result.copies.files)
			summary << "; " << CopyReportShort(result.copies);
	}
	else {
		code = result.failed || result.conflicts ? 1 : 0;
		status = code ? "Failures or conflicts" : "Good";
		summary = Format("%d actions, %d failed, %d conflicts, %s copied", result.done, result.failed,
		                 result.conflicts, FormatBytes(result.bytes));
		if(result.copies.files)
			summary << "; " << CopyReportShort(result.copies);
		if(!setup.flags.dry_run)
			Print(Format("finished run %d: %d of %d actions done, %d failed, %d conflicts, %s copied",
			             result.run, result.done, result.planned, result.failed, result.conflicts,
			             FormatBytes(result.bytes)), run_started);
		copy_report();
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
	String what = edit.GetCount() ? "Edit (" + edit + ")"
	            : scan_only ? "Scan only run " + AsString(result.run)
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
