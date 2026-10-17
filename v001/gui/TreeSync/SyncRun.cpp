// Runs treesync-cli (from the same folder as this program) and shows its output.
// The GUI never runs the engine in its own process, and never as root: with
// "Run as root" it starts treesync-cli through pkexec, so the GUI itself stays an
// ordinary Wayland client. Stop closes the child's stdin (--stop-on-eof), which
// works even when the child is a root process the GUI may not signal.
#include "TreeSync.h"

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

extern char **environ;

void TreeSyncWindow::Log(const String& line)
{
	log.Insert(log.GetLength(), line + "\n");
	log.SetCursor(log.GetLength());
}

void TreeSyncWindow::StartSync(bool dry_run)
{
	if(child > 0)
		return;
	String cli = AppendFileName(GetExeFolder(), "treesync-cli");
	if(!FileExists(cli)) {
		Log("cannot find " + cli + "; build it with v001/build.sh");
		return;
	}
	SyncSetup s = setup;
	if(dry_run)
		s.flags.dry_run = true;
	Vector<String> args;
	if(as_root)
		args.Add("/usr/bin/pkexec");
	args.Add(cli);
	args.Append(ToArgs(s));
	args.Add("--stop-on-eof");

	int out[2], in[2];
	if(pipe2(out, O_CLOEXEC) != 0 || pipe2(in, O_CLOEXEC) != 0) {
		Log(String("pipe: ") + strerror(errno));
		return;
	}
	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, in[0], 0);
	posix_spawn_file_actions_adddup2(&fa, out[1], 1);
	posix_spawn_file_actions_adddup2(&fa, out[1], 2);
	Vector<const char *> argv;
	for(const String& a : args)
		argv.Add(a);
	argv.Add(nullptr);
	pid_t pid;
	int rc = posix_spawn(&pid, args[0], &fa, nullptr, (char **)argv.begin(), environ);
	posix_spawn_file_actions_destroy(&fa);
	close(out[1]);
	close(in[0]);
	if(rc != 0) {
		close(out[0]);
		close(in[1]);
		Log("cannot start " + args[0] + ": " + strerror(rc));
		return;
	}
	child = pid;
	child_stdin = in[1];
	Log(String(as_root ? "[as root] " : "") + CommandLineText(ToArgs(s)));
	status = "Running...";

	// Reader thread: lines go to the log on the GUI thread; exit status at the end.
	int fd = out[0];
	if(reader.joinable())
		reader.join();
	reader = std::thread([this, fd, pid] {
		String pending;
		char buf[4096];
		for(ssize_t n; (n = read(fd, buf, sizeof(buf))) != 0;) {
			if(n < 0) {
				if(errno == EINTR)
					continue;
				break;
			}
			pending.Cat(buf, (int)n);
			int nl;
			while((nl = pending.Find('\n')) >= 0) {
				String line = pending.Left(nl);
				pending.Remove(0, nl + 1);
				PostCallback([this, line] { Log(line); });
			}
		}
		close(fd);
		int st = 0;
		while(waitpid(pid, &st, 0) < 0 && errno == EINTR)
			;
		int code = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
		PostCallback([this, code] { SyncFinished(code); });
	});
}

void TreeSyncWindow::StopSync()
{
	if(child_stdin >= 0) {
		close(child_stdin);
		child_stdin = -1;
		Log("stop requested: finishing the current file...");
		status = "Stopping...";
	}
}

void TreeSyncWindow::SyncFinished(int code)
{
	if(child_stdin >= 0)
		close(child_stdin);
	child_stdin = -1;
	child = 0;
	const char *what = code == 0 ? "in sync" : code == 1 ? "finished with failures or conflicts"
	                 : code == 2 ? "could not run" : code == 3 ? "stopped; Run again to resume"
	                 : code == 126 || code == 127 ? "pkexec refused or was cancelled" : "ended abnormally";
	Log(Format("treesync-cli exit code %d: %s", code, what));
	status = Format("Last run: %s", what);
	LoadTrees();
}

void TreeSyncWindow::Close()
{
	if(child > 0) {
		if(!PromptYesNo("A sync is running. Stop it (it can be resumed later) and quit?"))
			return;
		StopSync();
		if(reader.joinable())
			reader.join();   // waits for the current file or chunk to finish
	}
	TopWindow::Close();
}

TreeSyncWindow::~TreeSyncWindow()
{
	if(child_stdin >= 0)
		close(child_stdin);
	if(reader.joinable())
		reader.join();
}
