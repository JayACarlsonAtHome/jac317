// treesync-cli: runs a sync in the terminal, no GUI. Same options as treesync.
// Ctrl+C once: stop after the current file or chunk (the run can be resumed).
// Ctrl+C twice: quit at once.
// --stop-on-eof (used by the GUI): stop the same way when stdin is closed. The GUI
// can always close a pipe, even when pkexec made this a root process it may not signal.
// Exit codes: 0 in sync, 1 failures or conflicts, 2 usage or setup error, 3 stopped.
#include <TreeSyncCore/TreeSyncCore.h>

#include <csignal>
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

static void Print(const String& line)
{
	if(status_shown)
		Cout() << "\r\x1b[K";
	status_shown = false;
	Cout() << line << "\n";
	Cout().Flush();
}

static void Status(const String& s)
{
	if(!tty)
		return;
	Cout() << "\r\x1b[K" << s.Left(150);
	Cout().Flush();
	status_shown = true;
}

CONSOLE_APP_MAIN
{
	Vector<String> args = clone(CommandLine());
	bool stop_on_eof = false;
	for(int i = args.GetCount() - 1; i >= 0; i--)
		if(args[i] == "--stop-on-eof") {
			stop_on_eof = true;
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
	if(stop_on_eof)
		std::thread([] {
			char c;
			while(read(0, &c, 1) > 0)
				;
			OnSignal(0);
		}).detach();

	SyncReport report;
	report.stop = &stop_requested;
	report.WhenLine = [](const String& s) { Print(s); };
	report.WhenStatus = [](const String& s) { Status(s); };
	report.WhenProgress = [](const CopyProgress& p) {
		if(p.done >= p.size)
			Print(ProgressLine(p));
		else
			Status(ProgressLine(p));
	};

	Print(CommandLineText(ToArgs(setup)));
	SyncResult result;
	if(!RunSync(setup, report, result, error)) {
		Print("treesync-cli: " + error);
		SetExitCode(2);
		return;
	}
	if(result.stopped) {
		if(result.run)
			Print(Format("stopped: %d of %d actions done; run treesync-cli again with the same options to resume",
			             result.done, result.planned));
		else
			Print("stopped while scanning; the hashes finished so far are kept, so the next run is quicker");
		SetExitCode(3);
		return;
	}
	if(!setup.flags.dry_run)
		Print(Format("finished run %d: %d of %d actions done, %d failed, %d conflicts, %s copied",
		             result.run, result.done, result.planned, result.failed, result.conflicts,
		             FormatBytes(result.bytes)));
	SetExitCode(result.failed || result.conflicts ? 1 : 0);
}
