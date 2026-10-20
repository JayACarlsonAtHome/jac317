// Runs treesync-cli (from the same folder as this program) and shows its output.
// The GUI never runs the engine in its own process, and never as root: TreeSyncRoot
// starts treesync-cli --root-session through pkexec once per window (one password),
// and that root process starts each run, so the GUI itself stays an ordinary Wayland
// client; TreeSyncHome starts it as the user, with --home. Stop closes the child's stdin (--stop-on-eof), which
// works even when the child is a root process the GUI may not signal.
#include "TreeSync.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

extern char **environ;

void TreeSyncWindow::Log(const String& line)
{
	LogText(line + "\n");
}

// The pane only shows what is happening now: the newest 100 lines. Every line is
// in the run's log file (View > Show log file), so the pane never grows and stays fast.
static const int LOG_KEEP = 100;

void TreeSyncWindow::LogText(const String& text)
{
	log.Insert(log.GetLength(), text);
	if(log.GetLineCount() > LOG_KEEP + LOG_KEEP / 2)   // trim in steps, not on every line
		log.Remove(0, log.GetPos(log.GetLineCount() - LOG_KEEP));
	log.SetCursor(log.GetLength());
}

// Takes everything waiting in the inbox at once: log lines in one append, and of
// the progress lines only the newest state of each bar (older ones were never seen).
void TreeSyncWindow::Drain()
{
	Vector<String> lines;
	{
		std::lock_guard<std::mutex> l(inbox_m);
		lines = pick(inbox);
		inbox_posted = false;
	}
	String text;
	VectorMap<String, String> latest;   // progress key -> its newest line
	Vector<String> finished;            // @file lines of files that finished: all of them count
	for(const String& line : lines) {
		if(line.StartsWith("@file\t")) {
			Vector<String> f = Split(line, '\t', false);
			if(f.GetCount() >= 4 && ScanInt64(f[2]) >= ScanInt64(f[3]) && !(f.GetCount() >= 9 && f[8] == "verify"))
				finished.Add(line);
			else if(f.GetCount() >= 2)
				latest.GetAdd("file " + f[1]) = line;
		}
		else if(line.StartsWith("@stage\t")) {
			Vector<String> f = Split(line, '\t', false);
			latest.GetAdd("stage " + (f.GetCount() > 1 ? f[1] : String())) = line;
		}
		else if(line.StartsWith("@overall\t"))
			latest.GetAdd("overall") = line;
		else if(line.StartsWith("@free\t"))   // only the newest of each side counts
			latest.GetAdd("free " + line.Mid(6, 1)) = line;
		else if(line.StartsWith("@proof\t"))   // already in the log as a plain line
			proof.Add(line.Mid(7));
		else {
			if(line.StartsWith("room check:"))
				room = line;
			if(line.StartsWith("treesync-cli: ") && line.Find("not running as root") < 0)
				cli_error = line.Mid(14);
			for(const char *done : { "made folder ", "renamed ", "moved ", "deleted " })
				if(line.StartsWith(done))
					edit_done = line;
			text << line << "\n";
		}
	}
	if(text.GetCount())
		LogText(text);
	for(const String& line : latest)
		ShowProgress(line);
	for(const String& line : finished)
		ShowProgress(line);   // after the in-flight updates, so finished rows go
}

// --bench-log (a developer aid): feeds n lines, shaped like a big run's output, through
// the same path as treesync-cli's (inbox, Drain, log pane, progress), from a thread,
// and runs the event loop until they are all shown.
double TreeSyncWindow::BenchLog(int n)
{
	ProgressVisible(true);
	auto t0 = std::chrono::steady_clock::now();
	std::atomic<bool> fed = false;
	std::thread feeder([this, n, &fed] {
		for(int i = 0; i < n; i++) {
			String line;
			switch(i % 4) {
			case 0: line = Format("[%5d/%d] some/folder/file %d.txt                       100%% |  12.0 MiB/s |  1.0 KiB |  00m00s", i, n, i); break;
			case 1: line = Format("@file\t%d\t512\t1024\t0.10\t%d\t%d\tsome/folder/file %d.txt", i, i, n, i); break;
			case 2: line = Format("@stage\t0\t%d\t%d\t0\tsome/folder", i, n); break;
			default: line = Format("@overall\t%lld\t%lld", i * 1024LL, n * 1024LL); break;   // %d would cut an int64 to int
			}
			std::lock_guard<std::mutex> l(inbox_m);
			inbox.Add(line);
			if(!inbox_posted) {
				inbox_posted = true;
				PostCallback([this] { Drain(); });
			}
		}
		fed = true;
	});
	for(;;) {
		Ctrl::ProcessEvents();
		bool empty;
		{
			std::lock_guard<std::mutex> l(inbox_m);
			empty = inbox.IsEmpty() && !inbox_posted;
		}
		if(fed && empty)
			break;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	feeder.join();
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

// The boxes and the list say what this run does: a scan copies nothing.
void TreeSyncWindow::StageLabels(const String& extra)
{
	run_extra = extra;
	bool scan = extra == "--scan-only", check = scan || extra == "--sync-attrs";
	stage_box[0].SetLabel(check ? "Pre-scan: list both trees" : "Pre-scan: source");
	stage_box[1].SetLabel(scan ? "Check: SHA-256 of both sides" : extra == "--sync-attrs" ? "Attributes" : "Copy");
	stage_box[2].SetLabel(check ? "Post-scan: folders checked" : "Post-scan: target");
	active.HeaderTab(0).SetText(scan ? "Checking" : "Copying");
	active.HeaderTab(2).SetText(scan ? "Read" : "Copied");   // the bytes column after the bar
}

// --screenshot: a Scan only run part-way.
void TreeSyncWindow::DemoScanProgress()
{
	StageLabels("--scan-only");
	ProgressVisible(true);
	active.Clear();
	for(const char *l : {
		"@stage\t0\t412\t412\t1\t",
		"@overall\t2576980377\t8589934592",
		"@file\t3\t0\t4294967296\t3.1\t3\t140\tVMs/win11.qcow2\tcheck",
		"@file\t7\t0\t209715200\t0.4\t7\t140\tVMs/notes/install.log\tcheck",
		"@stage\t2\t180\t413\t0\t" })
		ShowProgress(l);
}

void TreeSyncWindow::DemoProgress()
{
	StageLabels(String());
	ProgressVisible(true);
	eta_t0 = std::chrono::steady_clock::now() - std::chrono::seconds(754);
	eta_samples.Clear();
	eta_samples.Add(MakeTuple(694.0, (int64)600000000));
	eta_samples.Add(MakeTuple(754.0, (int64)734003200));
	for(const char *l : {
		"@stage\t0\t31540\t51381\t0\tzig-x86_64-linux/lib/std/os",
		"@overall\t734003200\t1288490188",
		"@file\t12\t52428800\t134217728\t1.7\t12\t140\tzig-x86_64-linux/zig",
		"@file\t15\t1048576\t4194304\t0.2\t15\t140\tzig-x86_64-linux/lib/zig.h",
		"@file\t16\t3145728\t3355443\t0.4\t16\t140\tzig-x86_64-linux.tar.xz",
		"@stage\t2\t96\t412\t0\tzig-x86_64-linux/lib/std/crypto",
		"@free\t0\t412316860416\t998579896320\t1",
		"@free\t1\t384829069312\t491773755392\t0" })
		ShowProgress(l);
	ShowEta();
}

// One stage box: a bar (done of total; empty while the total is unknown) and a line.
static String MinSec(double s)
{
	int n = (int)s;
	return Format("%d:%02d", n / 60, n % 60);
}

void TreeSyncWindow::Tick()
{
	if(child <= 0)
		return;
	auto now = std::chrono::steady_clock::now();
	for(int i = 0; i < active.GetCount(); i++) {
		int q = copy_started.Find((int)active.Get(i, 0));
		if(q >= 0)
			active.Set(i, 6, MinSec(std::chrono::duration<double>(now - copy_started[q]).count()));   // Time
	}
	double quiet = std::chrono::duration<double>(now - last_progress).count();
	stage_text[1].SetInk(net_down ? LtRed() : SColorText());
	if(net_down) {
		double gone = std::chrono::duration<double>(now - net_since).count();
		// One line: the box has room for one. The log has the details.
		stage_text[1] = "NETWORK UNAVAILABLE: no connection to " + net_host + " for " + MinSec(gone) +
		                ". Retrying every 5 s; carries on by itself when it is back (gives up at " +
		                MinSec(net_wait) + ").";
	}
	else if(quiet >= 3 && active.GetCount())
		stage_text[1] = Format("Still working, please wait: %d file%s in progress, no new progress for %d s. "
		                       "Big files over a slow link can take minutes; nothing is stuck while this counts.",
		                       active.GetCount(), active.GetCount() == 1 ? "" : "s", (int)quiet);
	else
		stage_text[1] = copy_text;
	ShowEta();
	SetTimeCallback(1000, [this] { Tick(); }, 77);
}

static String HourMinSec(double s)
{
	int64 n = (int64)s;
	return Format("%d:%02d:%02d", (int)(n / 3600), (int)(n / 60 % 60), (int)(n % 60));
}

void TreeSyncWindow::EtaSample(int64 done, int64 total)
{
	eta_done = done;
	eta_total = total;
	double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - eta_t0).count();
	if(eta_samples.GetCount() && t - eta_samples.Top().a < 1)
		return;
	eta_samples.Add(MakeTuple(t, done));
	while(eta_samples.GetCount() > 2 && eta_samples[1].a < t - 60)   // keep the last minute
		eta_samples.Remove(0);
}

void TreeSyncWindow::ShowEta()
{
	double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - eta_t0).count();
	String s = "Elapsed time: " + HourMinSec(now);
	double rate = 0;   // bytes per second over the last minute
	if(eta_samples.GetCount() >= 2) {
		const Tuple<double, int64>& a = eta_samples[0];
		const Tuple<double, int64>& b = eta_samples.Top();
		if(b.a - a.a >= 5)
			rate = (b.b - a.b) / (b.a - a.a);
	}
	if(eta_total > eta_done && rate > 0) {
		double left = (eta_total - eta_done) / rate;
		Time end = GetSysTime() + (int64)left;
		String when = Format("%02d:%02d", end.hour, end.minute);
		if(Date(end) != GetSysDate())
			when << Format(" on %d-%02d-%02d", end.year, end.month, end.day);
		// While the pre-scan still finds files the total grows: at least this long.
		s << "    Total elapsed time: " << (prescan_done ? "~" : "at least ~") << HourMinSec(now + left)
		  << "    Estimated completion time: " << when;
	}
	else if(eta_total > eta_done)
		s << "    Total elapsed time: estimating...";
	eta = s + "  ";
}

void TreeSyncWindow::StageShow(int stage, int64 done, int64 total, const String& text)
{
	stage_bar[stage].Set(total > 0 ? (int)(min(done, total) * 1000 / total) : 0, 1000);
	if(stage == 1) {
		copy_text = text;
		if(net_down)   // the network message stays until the connection is back
			return;
	}
	stage_text[stage] = text;
}

// @file id done size seconds index count name / @overall done total /
// @stage stage done total finished detail, tab-separated.
void TreeSyncWindow::ShowProgress(const String& line)
{
	Vector<String> f = Split(line, '\t', false);
	last_progress = std::chrono::steady_clock::now();
	if(f[0] == "@net" && f.GetCount() >= 4) {
		net_down = f[1] == "1";
		net_host = f[2];
		net_wait = ScanInt(f[3]);
		net_since = std::chrono::steady_clock::now();
		Tick();
		return;
	}
	if(f[0] == "@free" && f.GetCount() >= 5) {
		(f[1] == "0" ? source_panel : target_panel).ShowFree(ScanInt64(f[2]), ScanInt64(f[3]), f[4] == "1");
		return;
	}
	if(f[0] == "@overall" && f.GetCount() >= 3) {
		int64 done = ScanInt64(f[1]), total = ScanInt64(f[2]);
		// While the pre-scan runs the total is what it has found so far; files that turn
		// out the same as the target's drop out of it again.
		EtaSample(done, total);
		StageShow(1, done, total, FormatBytes(done) + " of " + FormatBytes(total) +
		          (run_extra == "--scan-only" ? String(" read  (reading files only, nothing is copied)")
		           : prescan_done ? String() : String("  (so far: the pre-scan is still finding files)")));
		return;
	}
	if(f[0] == "@stage" && f.GetCount() >= 6) {
		int stage = ScanInt(f[1]);
		int64 done = ScanInt64(f[2]), total = ScanInt64(f[3]);
		bool finished = f[4] == "1";
		String detail = f.GetCount() > 5 ? f[5] : String();
		if(stage == 0 && finished)
			prescan_done = true;
		if(stage == 0 && run_extra.GetCount())   // Scan only / Sync attributes: folders of both trees listed
			StageShow(0, done, total,
			          finished ? Format("done: %s folders listed", AsString(done))
			                   : Format("%s of %s folders listed  ", AsString(done), AsString(total)) + detail);
		else if(stage == 0)
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
	bool verifying = f.GetCount() >= 9 && f[8] == "verify";
	bool checking = f.GetCount() >= 9 && f[8] == "check";   // Scan only: hashed on both sides
	int row = active.Find(id);
	if(done >= size && !verifying) {   // finished: its line is in the log
		if(row >= 0)
			active.Remove(row);
		copy_started.RemoveKey(id);
		return;
	}
	if(copy_started.Find(id) < 0)
		copy_started.Add(id, std::chrono::steady_clock::now());
	String elapsed = MinSec(std::chrono::duration<double>(std::chrono::steady_clock::now() - copy_started.Get(id)).count());
	String rate = verifying || checking ? String("checking") : seconds > 0 ? FormatBytes((int64)(done / seconds)) + "/s" : String();
	String what = Format("[%s/%s] ", f[5], f[6]) + f[7] +
	              (verifying ? "   (verifying the copy)" : checking ? "   (hashing both sides)" : "");
	int permille = size > 0 ? (int)(done * 1000 / size) : 0;
	// id | Copying | bar | Copied (bytes so far) | Rate | Size | Time
	if(row < 0)
		active.Add(id, what, permille, FormatBytes(done), rate, FormatBytes(size), elapsed);
	else
		active.Set(row, Vector<Value>{ id, what, permille, FormatBytes(done), rate, FormatBytes(size), elapsed });
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
	// Where safe delete puts things: next to the target, or inside it on a whole disk.
	String old_dir = s.safe_delete_dir.GetCount() ? s.safe_delete_dir
	               : s.target.remote ? where(s.target) + ".old" : SafeDeleteFolder(CanonicalPath(s.target.path), true);
	String old = s.flags.safe_delete
	           ? "Removed and replaced files are moved to [C " + DeQtf(old_dir) + "] (safe delete)."
	           : "[* Safe delete is off: removed and replaced files are deleted.]";
	String wipe;
	if(s.flags.wipe_target)
		wipe = s.flags.safe_delete
		     ? "[*@6 The target is emptied first]: everything in it is moved to [C " +
		       DeQtf(old_dir) + "], then all files are copied fresh.&"
		     : "[*@6 The target is emptied first and everything in it is DELETED], then all files "
		       "are copied fresh.&";
	return PromptYesNo("[* Run the sync for real?]&&"
	                   "From: [C " + DeQtf(where(s.source)) + "]&"
	                   "To: [C " + DeQtf(where(s.target)) + "]&&" + wipe +
	                   "This will " + DeQtf(what) + ".&" + old + "&&"
	                   "Use Sync > Dry run first to see the list of changes.");
}

// Sync attributes and Clear .old change the target: ask first. Clear .old counts what
// it would delete (local folders) so the question says how much.
bool TreeSyncWindow::ConfirmExtra(const SyncSetup& s, const String& extra)
{
	if(s.source.path.IsEmpty() || s.target.path.IsEmpty()) {
		Exclamation("Choose a source and a target first.");
		return false;
	}
	auto where = [](const TreeEndpoint& e) {
		return e.remote ? Nvl(e.user, GetUserName()) + "@" + e.host + ":" + e.path : e.path;
	};
	if(extra == "--sync-attrs")
		return PromptYesNo("[* Sync attributes?]&&From: [C " + DeQtf(where(s.source)) + "]&To: [C " +
		                   DeQtf(where(s.target)) + "]&&Files and folders whose mode, time, owner or extended "
		                   "attributes differ get the source's on the target. No file content is copied; files "
		                   "whose size differs are left for a sync.");
	if(extra == "--clear-old") {
		String counted;
		if(!s.target.remote && !s.source.remote) {
			SyncReport rep;
			rep.WhenLine = [&](const String& l) { if(l.StartsWith("would delete") || l.StartsWith("no ")) counted << DeQtf(l) << "&"; };
			String error;
			if(!ClearOld(s, true, rep, error))
				counted << "[@6 " << DeQtf(error) << "]&";
		}
		return PromptYesNo("[*@6 Delete the .old folders for good?]&&" +
		                   Nvl(counted, "[C " + DeQtf(where(s.target)) + ".old]&") +
		                   "&They hold the files safe delete moved out of the way. This cannot be undone." +
		                   String(as_root ? "" : "&(Folders written by a root run need TreeSyncRoot.)"));
	}
	return true;
}

// Root must not run a program the user can change: anything running as the user could
// swap it, and then run as root. So TreeSyncRoot uses a treesync-cli that belongs to root
// and only root may write (install.sh puts one in /usr/local/bin).
static bool RootOwned(const String& path)
{
	struct stat st;
	return stat(path, &st) == 0 && st.st_uid == 0 && !(st.st_mode & 022);
}

static String FileHash(const String& path)
{
	return Sha256Hex(LoadFile(path));
}

String OwnCli()
{
	String installed = AppendFileName(GetExeFolder(), "treesync-cli-" + ToLower(CompilerTag()));
	return FileExists(installed) ? installed : AppendFileName(GetExeFolder(), "treesync-cli");
}

// The installed treesync-cli of this program's compiler: a GCC build runs the GCC engine.
String RootCli(String& note)
{
	note.Clear();
	String own = OwnCli();
	if(RootOwned(own))   // this program is the installed one
		return own;
	String name = "treesync-cli-" + ToLower(CompilerTag());
	for(const char *dir : { "/usr/local/bin", "/usr/bin" })
		if(String p = AppendFileName(dir, name); RootOwned(p)) {
			if(FileExists(own) && FileHash(own) != FileHash(p))
				note = String("note: running the installed ") + p + " as root, which is not this build; "
				       "TreeSyncInstall installs this one";
			return p;
		}
	note = "warning: no " + name + " installed for root; running " + own + " as root, which your user can "
	       "change. TreeSyncInstall installs a root-owned copy (v001/install.sh)";
	return own;
}

// TreeSyncRoot asks for the password once per window: the first root run starts
// treesync-cli --root-session through pkexec, and that one root process runs every
// sync of the window (the CLI's RootSession). Started without waiting: the run's
// request waits in the socket while polkit asks.
bool TreeSyncWindow::SessionStart(const String& cli)
{
	if(session_fd >= 0)
		return true;
	int sv[2];
	if(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) {
		Log(String("socketpair: ") + strerror(errno));
		return false;
	}
	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, sv[1], 0);
	posix_spawn_file_actions_adddup2(&fa, sv[1], 1);
	const char *argv[] = { "/usr/bin/pkexec", ~cli, "--root-session", nullptr };
	pid_t pid;
	int rc = posix_spawn(&pid, argv[0], &fa, nullptr, (char **)argv, environ);
	posix_spawn_file_actions_destroy(&fa);
	close(sv[1]);
	if(rc != 0) {
		close(sv[0]);
		Log(String("cannot start /usr/bin/pkexec: ") + strerror(rc));
		return false;
	}
	session = pid;
	session_fd = sv[0];
	return true;
}

// Closing the socket ends the session (it is idle between runs).
void TreeSyncWindow::SessionEnd()
{
	if(session_fd >= 0)
		close(session_fd);
	session_fd = -1;
	if(session > 0)
		while(waitpid(session, nullptr, 0) < 0 && errno == EINTR)
			;
	session = 0;
}

// A run through the session: its arguments and the two pipe ends it gets as stdin and
// stdout. False when the session is gone.
static bool SessionSend(int fd, const Vector<String>& args, int run_in, int run_out)
{
	String all;
	for(const String& a : args)
		all << a << '\0';
	uint32 size = all.GetCount();
	alignas(cmsghdr) char ctl[CMSG_SPACE(2 * sizeof(int))] = {};
	iovec iov = { &size, sizeof(size) };
	msghdr m = {};
	m.msg_iov = &iov;
	m.msg_iovlen = 1;
	m.msg_control = ctl;
	m.msg_controllen = sizeof(ctl);
	cmsghdr *c = CMSG_FIRSTHDR(&m);
	c->cmsg_level = SOL_SOCKET;
	c->cmsg_type = SCM_RIGHTS;
	c->cmsg_len = CMSG_LEN(2 * sizeof(int));
	int fds[2] = { run_in, run_out };
	memcpy(CMSG_DATA(c), fds, sizeof(fds));
	ssize_t n;
	while((n = sendmsg(fd, &m, MSG_NOSIGNAL)) < 0 && errno == EINTR)
		;
	if(n != sizeof(size))
		return false;
	for(const char *p = all.Begin(), *e = all.End(); p < e;) {
		n = send(fd, p, e - p, MSG_NOSIGNAL);
		if(n < 0 && errno == EINTR)
			continue;
		if(n <= 0)
			return false;
		p += n;
	}
	return true;
}

static bool SessionCode(int fd, int& code)
{
	int32 c;
	for(char *p = (char *)&c, *e = p + sizeof(c); p < e;) {
		ssize_t n = recv(fd, p, e - p, 0);
		if(n < 0 && errno == EINTR)
			continue;
		if(n <= 0)
			return false;
		p += n;
	}
	code = c;
	return true;
}

void TreeSyncWindow::StartSync(bool dry_run, const String& extra)
{
	if(child > 0)
		return;
	String cli = OwnCli();
	String root_note;
	if(as_root)
		cli = RootCli(root_note);
	if(!FileExists(cli)) {
		Log("cannot find " + cli + "; build it with v001/build.sh");
		return;
	}
	bool editing = extra == "--edit";   // a right-click change (Edit.cpp): confirmed already
	if(source_panel.tree.NothingChecked() && extra != "--clear-old" && !editing) {
		Exclamation("Nothing is checked in the source tree: check what to sync (the top box checks everything).");
		return;
	}
	// The menu decides, not the Dry run switch: Run is real, Dry run previews.
	SyncSetup s = setup;
	s.flags.dry_run = dry_run;
	if(!start_anyway && !editing && (extra.GetCount() ? !ConfirmExtra(s, extra) : !dry_run && !ConfirmRun(s)))
		return;   // (start anyway: confirmed already, then asked again)
	StageLabels(extra);
	bool via_session = as_root && !IsRoot();
	Vector<String> args;   // via the session: without the program (it runs itself)
	if(!via_session)
		args.Add(cli);
	args.Append(ToArgs(s));
	if(!as_root)   // TreeSyncHome: as the user, even for an administrator's installed copy
		args.Add("--home");
	if(editing)
		args.Append(edit_args);
	else if(extra.GetCount())
		args.Add(extra);
	if(start_anyway)   // asked for after "may not fit"
		args.Add("--no-precheck");
	start_anyway = false;
	args.Add("--stop-on-eof");
	args.Add("--gui-progress");

	int out[2], in[2];
	if(pipe2(out, O_CLOEXEC) != 0 || pipe2(in, O_CLOEXEC) != 0) {
		Log(String("pipe: ") + strerror(errno));
		return;
	}
	pid_t pid = 0;
	if(via_session) {
		bool fresh = session_fd < 0;
		if(!SessionStart(cli)) {
			for(int fd : { out[0], out[1], in[0], in[1] })
				close(fd);
			return;
		}
		pid = session;
		if(fresh)
			Log("[as root: " + cli + "] asking for the password once for this window (pkexec)");
	}
	else {
		posix_spawn_file_actions_t fa;
		posix_spawn_file_actions_init(&fa);
		posix_spawn_file_actions_adddup2(&fa, in[0], 0);
		posix_spawn_file_actions_adddup2(&fa, out[1], 1);
		posix_spawn_file_actions_adddup2(&fa, out[1], 2);
		Vector<const char *> argv;
		for(const String& a : args)
			argv.Add(a);
		argv.Add(nullptr);
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
	}
	child = pid;
	child_stdin = in[1];
	sync_started = (int64)time(nullptr);
	sync_dry = dry_run;
	Log(String(as_root ? "[as root: " + cli + "] " : "") + CommandLineText(ToArgs(s)) +
	    (editing ? " " + Join(edit_args, " ") : extra.GetCount() ? " " + extra : String()));
	if(root_note.GetCount())
		Log(root_note);
	status = editing ? "Changing..." : "Running...";
	proof.Clear();
	room.Clear();
	cli_error.Clear();
	edit_done.Clear();
	active.Clear();
	copy_started.Clear();
	net_down = false;
	prescan_done = false;
	eta_samples.Clear();
	eta_done = eta_total = 0;
	eta_t0 = std::chrono::steady_clock::now();
	ShowEta();
	for(int i = 0; i < 3; i++)
		StageShow(i, 0, 0, String());
	ProgressVisible(!editing);
	last_progress = std::chrono::steady_clock::now();
	SetTimeCallback(1000, [this] { Tick(); }, 77);

	// Reader thread: lines go to the log on the GUI thread; exit status at the end.
	// Through the session it first hands the run over (that waits while polkit asks),
	// and the exit code comes back over the socket; a session that is gone (password
	// refused, or it died) gives pkexec's code, and the next run starts a new one.
	int fd = out[0], sfd = via_session ? session_fd : -1;
	if(reader.joinable())
		reader.join();
	reader = std::thread([this, fd, pid, sfd, args = pick(args), run_in = in[0], run_out = out[1]] {
		bool sent = true;
		if(sfd >= 0) {
			sent = SessionSend(sfd, args, run_in, run_out);
			close(run_in);    // the session has its own copies now
			close(run_out);
		}
		String pending;
		char buf[4096];
		for(ssize_t n; sent && (n = read(fd, buf, sizeof(buf))) != 0;) {
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
				std::lock_guard<std::mutex> l(inbox_m);
				inbox.Add(line);
				if(!inbox_posted) {   // one callback for the whole batch
					inbox_posted = true;
					PostCallback([this] { Drain(); });
				}
			}
		}
		close(fd);
		int code = 0;
		bool lost = false;
		if(sfd < 0 || !sent || !SessionCode(sfd, code)) {
			lost = sfd >= 0;
			int st = 0;
			while(waitpid(pid, &st, 0) < 0 && errno == EINTR)
				;
			code = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
		}
		PostCallback([this, code, lost] {
			if(lost) {   // reaped above: only the socket is left
				close(session_fd);
				session_fd = -1;
				session = 0;
			}
			SyncFinished(code);
		});
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
	Drain();   // whatever is still waiting
	if(child_stdin >= 0)
		close(child_stdin);
	child_stdin = -1;
	child = 0;
	bool scan = run_extra == "--scan-only", attrs = run_extra == "--sync-attrs", clear = run_extra == "--clear-old";
	const char *what = code == 0 ? (scan ? "the trees are the same" : attrs ? "attributes in sync" : clear || run_extra == "--edit" ? "done" : "in sync")
	                 : code == 1 ? (scan ? "found differences (History > SHA-256 checks)" : "finished with failures or conflicts")
	                 : code == 2 ? "could not run" : code == 3 ? (run_extra.GetCount() ? "stopped" : "stopped; Run again to resume")
	                 : code == 4 ? "may not fit on the target; nothing was copied"
	                 : code == 126 || code == 127 ? "pkexec refused or was cancelled" : "ended abnormally";
	String name = scan ? "Scan only" : attrs ? "Sync attributes" : clear ? "Clear .old" : sync_dry ? "Dry run" : "Sync";
	Log(Format("treesync-cli exit code %d: %s", code, what));
	int64 ended = (int64)time(nullptr), took = ended - sync_started;
	Log("took " + DurationWords(took) + " (" + DurationClock(took) + ")");   // padded: the log lines up
	// The popup and status bar show one duration on its own: no padding needed.
	String took_text = TrimBoth(DurationWords(took));
	while(took_text.Find("  ") >= 0)
		took_text.Replace("  ", " ");
	KillTimeCallback(77);
	eta = String();
	net_down = false;
	stage_text[1].SetInk(SColorText());
	active.Clear();
	copy_started.Clear();
	ProgressVisible(false);
	status = "Last run: " + String(what) + ", took " + took_text;
	LoadTrees();
	if(run_extra == "--edit") {   // a right-click change: the tree shows it; a refusal says why
		status = code == 0 ? Nvl(edit_done, String("done")) : "Not changed: " + Nvl(cli_error, String(what));
		if(code != 0 && code != 126 && code != 127 && !closing)
			Exclamation("[* Not changed.]&&" + DeQtf(Nvl(cli_error, String(what))));
		return;
	}
	// How long it took, in a popup (not when pkexec was refused or the window is closing).
	// May not fit: say how much, and offer to start anyway (files of zeros need less).
	if(code == 4 && !closing) {
		if(PromptYesNo("[* The copies may not fit on the target.]&&" + DeQtf(room) + "&&"
		               "Nothing was copied. The check counts every file at its full size, but a file with "
		               "long runs of zeros (a VM image) takes much less disk than that, which is only known "
		               "when it is read. If you start anyway, the run stops by itself, and can be resumed, "
		               "before the target is full.&&[* Start anyway?]")) {
			start_anyway = true;
			StartSync(sync_dry, run_extra);
		}
		return;
	}
	// With the copies as checked: the numbers that show source and copies match.
	String checked;
	for(const String& l : proof)
		checked << "[C1 " << DeQtf(l) << "]&";
	if(sync_dry && room.GetCount()) {   // a dry run: whether the run it previews has room
		bool fits = room.EndsWith("room to copy");
		checked << (fits ? "[@2 " : "[*@6 ") << DeQtf("Room check: " + room.Mid(room.Find(':') + 2)) << "]&";
	}
	if(!closing && code != 126 && code != 127)
		PromptOK("[* " + DeQtf(name + ": " + what) + ".]&&"
		         "It took [* " + DeQtf(took_text) + "]&"
		         "([C " + DurationClock(took) + "], from " + ClockTime(sync_started) + " to " + ClockTime(ended) + ")" +
		         (checked.GetCount() ? "&&" + checked : String()));
}

void TreeSyncWindow::Close()
{
	if(child > 0) {
		if(!PromptYesNo("A sync is running. Stop it (it can be resumed later) and quit?"))
			return;
		closing = true;
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
	SessionEnd();
}
