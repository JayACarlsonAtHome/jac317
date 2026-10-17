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

void TreeSyncWindow::DemoProgress()
{
	ProgressVisible(true);
	for(const char *l : {
		"@stage\t0\t31540\t51381\t0\tzig-x86_64-linux/lib/std/os",
		"@overall\t734003200\t1288490188",
		"@file\t12\t52428800\t134217728\t1.7\t12\t140\tzig-x86_64-linux/zig",
		"@file\t15\t1048576\t4194304\t0.2\t15\t140\tzig-x86_64-linux/lib/zig.h",
		"@file\t16\t3145728\t3355443\t0.4\t16\t140\tzig-x86_64-linux.tar.xz",
		"@stage\t2\t96\t412\t0\tzig-x86_64-linux/lib/std/crypto" })
		ShowProgress(l);
}

// One stage box: a bar (done of total; empty while the total is unknown) and a line.
void TreeSyncWindow::StageShow(int stage, int64 done, int64 total, const String& text)
{
	stage_bar[stage].Set(total > 0 ? (int)(min(done, total) * 1000 / total) : 0, 1000);
	stage_text[stage] = text;
}

// @file id done size seconds index count name / @overall done total /
// @stage stage done total finished detail, tab-separated.
void TreeSyncWindow::ShowProgress(const String& line)
{
	Vector<String> f = Split(line, '\t', false);
	if(f[0] == "@overall" && f.GetCount() >= 3) {
		int64 done = ScanInt64(f[1]), total = ScanInt64(f[2]);
		StageShow(1, done, total, FormatBytes(done) + " of " + FormatBytes(total) + " so far");
		return;
	}
	if(f[0] == "@stage" && f.GetCount() >= 6) {
		int stage = ScanInt(f[1]);
		int64 done = ScanInt64(f[2]), total = ScanInt64(f[3]);
		bool finished = f[4] == "1";
		String detail = f.GetCount() > 5 ? f[5] : String();
		if(stage == 0)
			StageShow(0, done, finished ? done : total,
			          finished ? Format("done: %s entries", AsString(done))
			                   : AsString(done) + (total ? " of ~" + AsString(total) : String()) + " entries  " + detail);
		else if(stage == 2)
			StageShow(2, done, total,
			          finished ? Format("done: %s folders checked", AsString(done))
			                   : Format("%s of %s folders  ", AsString(done), AsString(total)) + detail);
		return;
	}
	if(f[0] != "@file" || f.GetCount() < 8)
		return;
	int id = ScanInt(f[1]);
	int64 done = ScanInt64(f[2]), size = ScanInt64(f[3]);
	double seconds = ScanDouble(f[4]);
	int row = active.Find(id);
	if(done >= size) {   // finished: its line is in the log
		if(row >= 0)
			active.Remove(row);
		return;
	}
	String rate = seconds > 0 ? FormatBytes((int64)(done / seconds)) + "/s" : String();
	String what = Format("[%s/%s] ", f[5], f[6]) + f[7];
	int permille = size > 0 ? (int)(done * 1000 / size) : 0;
	if(row < 0)
		active.Add(id, what, permille, rate, FormatBytes(size));
	else
		active.Set(row, Vector<Value>{ id, what, permille, rate, FormatBytes(size) });
}

// What a real run will do to the target, in plain words, before it starts.
bool TreeSyncWindow::ConfirmRun(const SyncSetup& s)
{
	auto where = [](const TreeEndpoint& e) {
		return e.remote ? Nvl(e.user, GetUserName()) + "@" + e.host + ":" + e.path : e.path;
	};
	if(s.source.path.IsEmpty() || s.target.path.IsEmpty()) {
		Exclamation("Choose a source and a target first.");
		return false;
	}
	String what;
	switch(s.flags.mode) {
	case SyncMode::Copy:   what = "copy new and changed files into it"; break;
	case SyncMode::Mirror: what = "make it match the source, removing files the source does not have"; break;
	case SyncMode::Merge:  what = "copy changes both ways, so the source can change too"; break;
	}
	String old = s.flags.safe_delete
	           ? "Removed and replaced files are moved to [C " + DeQtf(where(s.target)) + ".old] (safe delete)."
	           : "[* Safe delete is off: removed and replaced files are deleted.]";
	String wipe;
	if(s.flags.wipe_target)
		wipe = s.flags.safe_delete
		     ? "[*@6 The target is emptied first]: everything in it is moved to [C " +
		       DeQtf(where(s.target)) + ".old], then all files are copied fresh.&"
		     : "[*@6 The target is emptied first and everything in it is DELETED], then all files "
		       "are copied fresh.&";
	return PromptYesNo("[* Run the sync for real?]&&"
	                   "From: [C " + DeQtf(where(s.source)) + "]&"
	                   "To: [C " + DeQtf(where(s.target)) + "]&&" + wipe +
	                   "This will " + DeQtf(what) + ".&" + old + "&&"
	                   "Use Sync > Dry run first to see the list of changes.");
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
	// The menu decides, not the Dry run switch: Run is real, Dry run previews.
	SyncSetup s = setup;
	s.flags.dry_run = dry_run;
	if(!dry_run && !ConfirmRun(s))
		return;
	Vector<String> args;
	if(as_root)
		args.Add("/usr/bin/pkexec");
	args.Add(cli);
	args.Append(ToArgs(s));
	args.Add("--stop-on-eof");
	args.Add("--gui-progress");

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
	active.Clear();
	for(int i = 0; i < 3; i++)
		StageShow(i, 0, 0, String());
	ProgressVisible(true);

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
				if(line.StartsWith("@file\t") || line.StartsWith("@overall\t") || line.StartsWith("@stage\t"))
					PostCallback([this, line] { ShowProgress(line); });
				else
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
	active.Clear();
	ProgressVisible(false);
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
