// Console tests for TreeSyncCore. Exit code 0 = all passed.
// Builds a scratch tree under $TMPDIR with awkward names (spaces at either end,
// tab, newline, quotes, backslash, UTF-8) and a folder chain deeper than PATH_MAX.
#include <TreeSyncCore/TreeSyncCore.h>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstring>

static int checks, failures;

static String Show(const String& s)
{
	String r = "[";
	for(char c : s)
		r << (c == '\n' ? String("\\n") : c == '\t' ? String("\\t") : String(c, 1));
	return r + "]";
}

// EXPECT, not CHECK: U++ Core already defines CHECK.
#define EXPECT(cond, what) Check((cond), #cond, what)

static void Check(bool ok, const char *cond, const String& what)
{
	checks++;
	if(ok)
		return;
	failures++;
	Cout() << "FAIL " << what << "   (" << cond << ")\n";
}

// rm -rf that works below PATH_MAX by descending with openat/unlinkat.
static void RemoveAt(int parent, const char *name)
{
	int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if(fd >= 0) {
		if(DIR *d = fdopendir(fd)) {
			while(dirent *e = readdir(d))
				if(strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
					RemoveAt(dirfd(d), e->d_name);
			closedir(d);
		}
		unlinkat(parent, name, AT_REMOVEDIR);
	}
	else
		unlinkat(parent, name, 0);
}

static void Touch(const String& path)
{
	int fd = open(path, O_CREAT | O_WRONLY | O_CLOEXEC, 0644);
	if(fd >= 0)
		close(fd);
}

static void TestIsUnder()
{
	EXPECT(!IsUnder("/home", "/hom"), "/home is not under /hom");
	EXPECT(IsUnder("/home", "/home"), "a folder is under itself");
	EXPECT(IsUnder("/home/jay", "/"), "everything is under /");
	EXPECT(!IsUnder("/homework", "/home"), "/homework is not under /home");
	EXPECT(IsUnder("/a b/c d", "/a b"), "spaces in both");
	EXPECT(!IsUnder("/a bc", "/a b"), "prefix with a space is not a parent");
	EXPECT(!IsUnder("/home/jay/../other", "/home/jay"), ".. leaves the folder");
}

static void TestListFolder(const String& root)
{
	const char *names[] = { "dir with spaces", " leading space", "trailing space ", "café ✓" };
	const char *files[] = { "tab\tname", "new\nline", "quote\"name", "a'b", "back\\slash" };
	for(const char *n : names)
		mkdir(AppendFileName(root, n), 0755);
	for(const char *f : files)
		Touch(AppendFileName(root, f));
	Touch(root + "/dir with spaces/file with spaces.txt");
	symlink("dir with spaces", root + "/link with spaces");

	Vector<FolderEntry> e;
	String err;
	EXPECT(ListFolder(root, e, err), "list awkward names: " + err);
	Index<String> seen;
	for(const FolderEntry& f : e)
		seen.Add(f.name);
	for(const char *n : names)
		EXPECT(seen.Find(n) >= 0, "listed " + Show(n));
	for(const char *f : files)
		EXPECT(seen.Find(f) >= 0, "listed " + Show(f));
	for(const FolderEntry& f : e)
		if(f.name == "link with spaces")
			EXPECT(f.is_link && f.is_dir, "symlink to a folder is link + dir");

	// Folder chain deeper than PATH_MAX, made with mkdirat so no full path is needed.
	String deep = root + "/deep";
	mkdir(deep, 0755);
	int fd = open(deep, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	String name = String('d', 240);
	String path = deep;
	for(int i = 0; i < 26 && fd >= 0; i++) {
		String n = name + " x" + AsString(i);
		mkdirat(fd, n, 0755);
		int next = openat(fd, n, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		close(fd);
		fd = next;
		path << "/" << n;
	}
	if(fd >= 0) {
		int f = openat(fd, "bottom file.txt", O_CREAT | O_WRONLY | O_CLOEXEC, 0644);
		if(f >= 0)
			close(f);
		close(fd);
	}
	EXPECT(path.GetCount() > PATH_MAX, "deep path is longer than PATH_MAX");
	Vector<FolderEntry> bottom;
	EXPECT(ListFolder(path, bottom, err), Format("list %d-byte path: ", path.GetCount()) + err);
	EXPECT(bottom.GetCount() == 1 && bottom[0].name == "bottom file.txt", "bottom file found");
}

static void TestCleanPath(const String& root)
{
	String d = root + "/dir with spaces";
	String escaped = root;
	escaped.Replace(" ", "\\ ");
	escaped << "/dir\\ with\\ spaces";
	String uri = "file://" + d;
	uri.Replace(" ", "%20");
	struct { String in, expect; } cases[] = {
		{ d, d },
		{ "\"" + d + "\"", d },
		{ "'" + d + "'", d },
		{ d + "\n", d },
		{ escaped, d },
		{ uri, d },
		{ "~/x y", GetHomeDirectory() + "/x y" },
		{ root + "/trailing space ", root + "/trailing space " },
		{ root + "/back\\slash", root + "/back\\slash" },
	};
	for(auto& c : cases)
		EXPECT(CleanPath(c.in) == c.expect, "CleanPath " + Show(c.in) + " -> " + Show(CleanPath(c.in)));
}

// Splits a shell command line with a real sh, NUL-separated.
static Vector<String> ShellSplit(const String& args)
{
	Vector<String> out;
	FILE *f = popen("printf '%s\\0' " + args, "r");
	if(!f)
		return out;
	String all;
	char buf[4096];
	while(size_t n = fread(buf, 1, sizeof(buf), f))
		all.Cat(buf, (int)n);
	pclose(f);
	// Split by hand: U++ Split takes a C string and would stop at the first NUL.
	int start = 0;
	for(int i = 0; i < all.GetCount(); i++)
		if(all[i] == '\0') {
			out.Add(all.Mid(start, i - start));
			start = i + 1;
		}
	return out;
}

static void TestOptions()
{
	Vector<Vector<String>> lines = {
		{ "--source=/home/jay/my dir", "--target=/mnt/it's here", "--merge", "--time-tolerance=5",
		  "--no-dry-run", "--threads", "16", "--exclude-file", "*.tmp", "--exclude-file",
		  "Thumbs db", "--exclude-dir=.git" },
		{ "--source=/data", "--target-host=nas.local", "--target-user=jay", "--target-port=2222",
		  "--target-key=/home/jay/.ssh/id ed25519", "--target=/backup", "--copy", "--retries=0" },
		{ "--mirror", "--follow-links", "--no-hard-links", "--skip-older", "--skip-newer", "--keep-extra",
		  "--skip-source-only", "--include-same", "--dst-tolerance", "--skip-changed",
		  "--retry-wait", "60" },
		{},
	};
	for(const Vector<String>& args : lines) {
		SyncSetup a, b;
		String err;
		EXPECT(ParseCommandLine(args, a, err) == PARSE_OK, "parse: " + err);
		Vector<String> out = ToArgs(a);
		EXPECT(ParseCommandLine(out, b, err) == PARSE_OK, "reparse: " + err);
		EXPECT(ToArgs(b) == out, "round trip: " + Join(out, " "));

		String line = CommandLineText(out);
		Vector<String> split = ShellSplit(line.Mid(line.Find(' ') + 1));
		EXPECT(split == out, "shell quoting survives sh: " + line);
	}

	const char *bad[] = { "--bogus", "--threads=0", "--threads=abc", "--threads", "--dry-run=yes",
	                      "--target-password=x", "stray", "--source-port=70000" };
	for(const char *arg : bad) {
		SyncSetup s;
		String err;
		EXPECT(ParseCommandLine(Vector<String>{ arg }, s, err) == PARSE_ERROR && err.GetCount(),
		      String("rejects ") + arg);
	}
	SyncSetup s;
	String err;
	EXPECT(ParseCommandLine(Vector<String>{ "--help" }, s, err) == PARSE_HELP, "--help");

	// --time-tolerance (default 3 s): times of the two sides this close are the same time.
	SyncFlags ff;
	ff.skip_changed = true;   // "same time" decides the skip, so TimeSkip shows the tolerance
	Entry a, b;
	b.mtime_ns = 1000000000000LL;
	a.mtime_ns = b.mtime_ns + 2500000000LL;
	EXPECT(TimeSkip(ff, a, b), "2.5 s apart: same time (default tolerance 3 s)");
	a.mtime_ns = b.mtime_ns + 3000000000LL;
	EXPECT(TimeSkip(ff, a, b), "3.0 s apart: still the same time");
	a.mtime_ns = b.mtime_ns + 3500000000LL;
	EXPECT(!TimeSkip(ff, a, b), "3.5 s apart: different times");
	ff.time_tolerance = 0;
	a.mtime_ns = b.mtime_ns + 1;
	EXPECT(!TimeSkip(ff, a, b), "--time-tolerance=0: 1 ns apart is different");

	// --dst-tolerance: one hour apart, give or take the tolerance.
	SyncFlags dst;
	dst.dst_tolerance = true;
	dst.skip_changed = true;
	a.mtime_ns = b.mtime_ns + 3600000000000LL + 2500000000LL;
	EXPECT(TimeSkip(dst, a, b), "1 h + 2.5 s: same time with --dst-tolerance");
	a.mtime_ns = b.mtime_ns - 3600000000000LL + 1000000000LL;
	EXPECT(TimeSkip(dst, a, b), "-1 h + 1 s: same time");
	a.mtime_ns = b.mtime_ns + 3600000000000LL + 3500000000LL;
	EXPECT(!TimeSkip(dst, a, b), "1 h + 3.5 s: different");
	dst.time_tolerance = 0;
	a.mtime_ns = b.mtime_ns + 3600000000000LL;
	EXPECT(TimeSkip(dst, a, b), "--time-tolerance=0: exactly 1 h is still a DST shift");
	a.mtime_ns = b.mtime_ns + 3601000000000LL;
	EXPECT(!TimeSkip(dst, a, b), "--time-tolerance=0: 1 h + 1 s is different");

	// Defaults differ by mode: Copy does not overwrite newer target files.
	EXPECT(DefaultsFor(SyncMode::Copy).skip_older && !DefaultsFor(SyncMode::Mirror).skip_older &&
	       !DefaultsFor(SyncMode::Merge).skip_older, "Copy defaults to --skip-older, Mirror and Merge do not");
	SyncSetup c1;
	ParseCommandLine(Vector<String>{ "--copy" }, c1, err);
	EXPECT(c1.flags.skip_older, "--copy starts from Copy's defaults");
	SyncSetup c2;
	ParseCommandLine(Vector<String>{ "--copy", "--no-skip-older" }, c2, err);
	Vector<String> c2a = FlagArgs(c2.flags);
	EXPECT(!c2.flags.skip_older && FindIndex(c2a, String("--no-skip-older")) >= 0,
	       "--copy --no-skip-older: off, and kept on the command line");
	SyncSetup c3;
	ParseCommandLine(Vector<String>{ "--skip-older", "--mirror" }, c3, err);
	EXPECT(c3.flags.skip_older, "a switch given before the mode still counts");

	// Owners are one choice: --numeric-ids switches owner copying on, and switching
	// it off leaves no numbers.
	SyncSetup o1;
	ParseCommandLine(Vector<String>{ "--no-preserve-owner", "--numeric-ids" }, o1, err);
	EXPECT(o1.flags.preserve_owner && o1.flags.numeric_ids, "--numeric-ids means owners by number");
	SyncSetup o2;
	ParseCommandLine(Vector<String>{ "--numeric-ids", "--no-preserve-owner" }, o2, err);
	EXPECT(!o2.flags.preserve_owner && !o2.flags.numeric_ids, "--no-preserve-owner: no owners at all");
}

// Saved settings: a round trip through the file, one-shot options left out, the
// old file kept if a save cannot be written, a bad file reported, not half-used.
static void TestSettings(const String& root)
{
	String path = root + "/config/treesync/settings";
	SyncSetup s;
	String error;
	EXPECT(!LoadSettings(path, s, error) && error.IsEmpty(), "no settings file: defaults, no error");
	const char *argv[] = { "--source=/home/jay/my files", "--target-host=192.168.1.194", "--target-user=jay",
	                       "--target=/vm/slow/test", "--mirror", "--threads=6", "--network-wait=600",
	                       "--exclude-file=*.tmp", "--dry-run", "--fresh" };
	Vector<String> args;
	for(const char *a : argv)
		args.Add(a);
	EXPECT(ParseCommandLine(args, s, error) == PARSE_OK, "settings: parse " + error);
	EXPECT(SaveSettings(path, s, error), "settings saved: " + error);
	String text = LoadFile(path);
	EXPECT(text.Find("--dry-run") < 0 && text.Find("--fresh") < 0, "one-shot options not saved");
	EXPECT(!FileExists(path + ".new"), "no leftover temporary file");
	SyncSetup back;
	EXPECT(LoadSettings(path, back, error), "settings loaded: " + error);
	back.flags.dry_run = s.flags.dry_run;
	back.fresh = s.fresh;
	EXPECT(ToArgs(back) == ToArgs(s), "settings round trip: " + Join(ToArgs(back), " "));
	SaveFile(path, "--mirror\n--no-such-option\n");
	SyncSetup bad = clone(back);
	EXPECT(!LoadSettings(path, bad, error) && error.Find("no-such-option") >= 0 && ToArgs(bad) == ToArgs(back),
	       "bad settings file reported, setup left alone: " + error);
}

static void TestMessages()
{
	String m = NotRootMessage();
	EXPECT(Split(m, '\n').GetCount() == 3, "not-root message has 3 lines: " + Show(m));
	EXPECT(m.Find("run TreeSync as root") >= 0, "not-root message says how to fix it: " + Show(m));
	String r = RefusedPathMessage("/my dir");
	EXPECT(r.StartsWith("/my dir is outside"), "refused message names the path: " + Show(r));
}

// Shaped like the two real machines: jay is 1001 here and 1000 there, where 1000
// here is someone else.
static void TestIds()
{
	IdTable here, there;
	here.Parse(false, "root:x:0:0::/root:/bin/bash\njcarlson:x:1000:1000::/home/jcarlson:/bin/bash\n"
	                  "jay:x:1001:1001::/home/jay:/bin/bash\nchrony:x:996:996::/:/sbin/nologin\n");
	there.Parse(false, "root:x:0:0::/root:/bin/bash\njay:x:1000:1000::/home/jay:/bin/bash\n"
	                   "chrony:x:983:983::/:/sbin/nologin\nsystemd-oom:x:996:996::/:/sbin/nologin\n");
	here.Parse(true, "root:x:0:\njcarlson:x:1000:\njay:x:1001:\ngfs:x:31300:jay\n");
	there.Parse(true, "root:x:0:\njay:x:1000:\ngfs:x:31300:jay\n");

	String unknown;
	EXPECT(MapId(here, there, false, 1001, false, nullptr) == 1000, "jay 1001 here is 1000 there");
	EXPECT(MapId(there, here, false, 1000, false, nullptr) == 1001, "jay 1000 there is 1001 here");
	EXPECT(MapId(here, there, false, 996, false, nullptr) == 983, "system account mapped by name");
	EXPECT(MapId(here, there, true, 31300, false, nullptr) == 31300, "gfs group same on both");
	EXPECT(MapId(here, there, false, 1000, false, &unknown) == 1000 && unknown == "jcarlson",
	       "name unknown there keeps its number and is reported");
	EXPECT(MapId(here, there, false, 1001, true, nullptr) == 1001, "--numeric-ids keeps the number");
	EXPECT(MapId(here, there, false, 4242, false, nullptr) == 4242, "number without a name is kept");

	String r = CompareIds(here, there, "local", "remote");
	EXPECT(r.Find("jay                  local 1001     remote 1000   -> suggest 1001 on both") >= 0,
	       "suggests the free number 1001 for jay");
	EXPECT(r.Find("### on remote\n# user jay: 1000 -> 1001\nusermod -u 1001 'jay'") >= 0,
	       "commands only on the side that changes");
	EXPECT(r.Find("### on local") < 0, "nothing to run on the side that keeps its number");
	EXPECT(r.Find("usermod -u 983") < 0 && r.Find("usermod -u 996") < 0, "system accounts never renumbered");
	EXPECT(r.Find("1000     local: jcarlson") >= 0, "number clash reported");
	EXPECT(r.StartsWith("TreeSync is experimental testing software."), "report starts with the disclaimer");
}

static void TestPathAllowed()
{
	if(IsRoot()) {
		EXPECT(PathAllowed("/etc"), "root may use any path");
		return;
	}
	String home = HomeDir();
	EXPECT(PathAllowed(home), "home allowed");
	EXPECT(PathAllowed(home + "/some new folder"), "below home allowed");
	EXPECT(!PathAllowed(home + "/../other"), ".. out of home refused");
	EXPECT(!PathAllowed("/etc"), "/etc refused");
	EXPECT(!PathAllowed(home + "x"), "home + suffix refused");
}

static void TestProgress()
{
	EXPECT(FormatBytes(0) == "0 B", "0 B");
	EXPECT(FormatBytes(1536) == "1.5 KiB", "1.5 KiB");
	EXPECT(FormatBytes(1024LL * 1024 * 1024 * 3) == "3.0 GiB", "3.0 GiB");

	CopyProgress lines[] = {
		{ 3, 108, "photos/2024/beach day.jpg", 3250585, 3250585, 0.25 },
		{ 4, 108, "music/Sigur Rós/Ágætis byrjun/01 Intro.flac", 1048576, 4194304, 0.5 },
		{ 5, 108, "a/very/long/path/" + String('x', 150) + "/the file name.txt", 0, 0, 0 },
		{ 108, 108, "Total", 314782105, 314782105, 21.4 },
	};
	Cout() << "\nProgress lines (width 100):\n";
	for(const CopyProgress& p : lines) {
		String l = ProgressLine(p);
		Cout() << l << "\n";
		EXPECT(l.ToWString().GetCount() == 100, "line is 100 columns: " + l);
	}
	EXPECT(ProgressLine(lines[2]).Find("the file name.txt") >= 0, "long name keeps its end");
	CopyProgress half = { 1, 2, "half way.bin", 500, 1000, 1.0 };
	String withbar = ProgressLine(half, 100, true);
	Cout() << withbar << "\n";
	EXPECT(withbar.ToWString().GetCount() == 100, "line with thermometer is 100 columns");
	EXPECT(withbar.Find(" 50% [==========          ]") >= 0, "thermometer half full at 50%");
	EXPECT(ProgressLine(lines[1]).Find(" 25% ") >= 0, "partial file shows 25%");
	EXPECT(ProgressLine(lines[3]).Find("00m21s") >= 0, "elapsed time");

	// Run durations, in the YYY:DDD:HH:MM:SS of Speed_Tests' getDuration().
	EXPECT(DurationClock(2 * 3600 + 5 * 60 + 9) == "000:000:02:05:09", "duration clock: " + DurationClock(7509));
	// Log stamps to the microsecond, every field padded: the message always starts at column 63.
	{
		int64 t = 1792300000LL * 1000000;
		String a = LogStampFine(t + 412, t + 6931), b = LogStampFine(t, t + 7509LL * 1000000 + 28106);
		EXPECT(a.GetCount() == 63 && b.GetCount() == 63 && a.Mid(9, 7) == ".000412" && a.Mid(27, 7) == ".006931",
		       "log stamp padded to the microsecond: " + Show(a));
		EXPECT(a.Mid(37, 23) == "000:000:00:00:00.006519" && b.Mid(37, 23) == "000:000:02:05:09.028106",
		       "log stamp duration: " + Show(b));
	}
	EXPECT(DurationWords(2 * 3600 + 5 * 60 + 9) == " 2 hours 05 minutes 09 seconds", "duration words: " + DurationWords(7509));
	EXPECT(DurationWords(61) == " 0 hours 01 minute  01 second", "duration words, singular: " + DurationWords(61));
	EXPECT(DurationWords(12 * 3600 + 10 * 60 + 11).GetCount() == DurationWords(3600 + 60).GetCount(),
	       "duration words line up: \"" + DurationWords(43811) + "\" / \"" + DurationWords(3660) + "\"");
	EXPECT(DurationWords(3 * 86400 + 3600) == "  3 days  1 hour  00 minutes 00 seconds", "duration with days: " + DurationWords(3 * 86400 + 3600));
	EXPECT(DurationClock(-90) == "000:000:00:01:30", "duration always positive");

	// The j5c build Results.txt layout.
	int64 t0 = 1760731378;   // some afternoon
	String rl = ResultsLine(t0, t0 + 7509, "Mirror run 6", 0, "Good", "src -> dst");
	Cout() << rl << "\n";
	EXPECT(rl.Find(" : 000:000:02:05:09 : Mirror run 6............................ : Result = ....0 : "
	               "Good..................... : src -> dst") == 20, "results line layout: " + rl);
	EXPECT(ResultsLine(t0, t0, "Copy run 7", 3, "Stopped", "").Find("Result = ....3 : Stopped..................") > 0 &&
	       !ResultsLine(t0, t0, "Copy run 7", 3, "Stopped", "").EndsWith(" : "), "results line, no comment");
	EXPECT(DateTimeText(t0).StartsWith("20") && DateTimeText(t0).Find(ClockTime(t0) + "  UTC = ") == 11,
	       "date and time text: " + DateTimeText(t0));
	String rh = ResultsHeader(t0);
	EXPECT(rh.StartsWith(" Status started on 20") && rh.Find("UTC = ") > 0 &&
	       rh.Find(" Start    : Stop     : Duration         : Command / Comments\n") > 0, "results header");
	EXPECT(DurationClock(400LL * 31556952 + 86400) == "400:001:00:00:00", "duration in years: " + DurationClock(400LL * 31556952 + 86400));
	Cout() << "\n";
}

// ---- engine ------------------------------------------------------------------

static void Put(const String& path, const String& data)
{
	std::error_code ec;
	std::filesystem::create_directories(ToFs(GetFileFolder(path)), ec);
	SaveFile(path, data);
}

static String Big(int64 bytes, int seed)
{
	String s;
	s.Reserve((int)bytes);
	for(int64 i = 0; i < bytes; i++)
		s.Cat((char)((i * 31 + seed) % 251));
	return s;
}

// Folder hash of a whole tree, from a scan with its own database.
static String TreeHash(const String& root, const String& db_path, bool follow_links = false)
{
	Db db;
	String err, hash;
	if(!db.Open(db_path, err))
		return "db: " + err;
	SyncFlags f;
	f.follow_links = follow_links;
	SyncReport r;
	Vector<Entry> e;
	ScanStats st;
	return ScanTree(db, root, f, r, e, hash, st, err) ? hash : "scan: " + err;
}

static String Sha256Sum(const String& path)
{
	FILE *f = popen("sha256sum -- '" + path + "'", "r");
	char buf[65] = {};
	if(f) {
		if(fread(buf, 1, 64, f) != 64)
			buf[0] = 0;
		pclose(f);
	}
	return buf;
}

static SyncSetup Setup(const String& src, const String& dst, const String& db)
{
	SyncSetup s;
	s.source.path = src;
	s.target.path = dst;
	s.db = db;
	s.flags.dry_run = false;
	s.flags.big_file_mib = 1;   // so a 3.5 MiB file is "big" and copied in 1 MiB chunks
	s.flags.chunk_mib = 1;
	s.flags.retries = 0;
	return s;
}

static bool Sync(const SyncSetup& s, SyncResult& r, Vector<String>* lines = nullptr,
                 std::atomic<bool> *stop = nullptr, Event<const CopyProgress&> on_progress = Null)
{
	SyncReport report;
	report.stop = stop;
	report.WhenLine = [&](const String& l) { if(lines) lines->Add(l); };
	report.WhenProgress = [&](const CopyProgress& p) { if(on_progress) on_progress(p); };
	String err;
	bool ok = RunSync(s, report, r, err);
	EXPECT(ok, "RunSync: " + err);
	return ok;
}

static void TestEngineMirror(const String& root)
{
	String src = root + "/src", dst = root + "/dst", db = root + "/sync.db", check = root + "/check.db";
	Put(src + "/a b.txt", "hello\n");
	Put(src + "/dir with spaces/x y.txt", "x\n");
	Put(src + "/ leading/trailing ", "spaces at both ends\n");
	Put(src + "/café ✓/naïve.txt", "utf-8\n");
	Put(src + "/big.bin", Big(3 * 1024 * 1024 + 512 * 1024, 1));
	Put(src + "/hard 1", "hard-linked\n");
	link(src + "/hard 1", src + "/hard 2");
	symlink("a b.txt", src + "/link to a");
	// A folder chain deeper than PATH_MAX, with a file at the bottom.
	{
		mkdir(src + "/deep", 0755);
		int fd = open(src + "/deep", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		for(int i = 0; i < 20 && fd >= 0; i++) {
			String n = String('d', 240) + " " + AsString(i);
			mkdirat(fd, n, 0755);
			int next = openat(fd, n, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
			close(fd);
			fd = next;
		}
		int f = fd >= 0 ? openat(fd, "bottom.txt", O_CREAT | O_WRONLY | O_CLOEXEC, 0644) : -1;
		if(f >= 0) {
			if(write(f, "deep\n", 5) != 5)
				EXPECT(false, "write deep file");
			close(f);
		}
		if(fd >= 0)
			close(fd);
	}

	// An extended attribute, where the filesystem takes user.* ones (tmpfs before Linux 6.6 does not).
	bool xattrs = setxattr(src + "/a b.txt", "user.treesync-test", "kept", 4, 0) == 0;

	// Dry run: plans, changes nothing.
	SyncSetup s = Setup(src, dst, db);
	s.flags.dry_run = true;
	SyncResult r;
	Sync(s, r);
	EXPECT(r.plan.GetCount() > 10 && !DirectoryExists(dst), Format("dry run plans %d actions, creates nothing", r.plan.GetCount()));

	// Real run.
	s.flags.dry_run = false;
	Vector<String> lines;
	Sync(s, r, &lines);
	EXPECT(r.failed == 0 && r.conflicts == 0, Format("mirror: %d failed, %d conflicts", r.failed, r.conflicts));
	EXPECT(TreeHash(src, check) == TreeHash(dst, check), "mirror: target tree hash equals source");
	struct stat a, b;
	EXPECT(stat(dst + "/hard 1", &a) == 0 && stat(dst + "/hard 2", &b) == 0 && a.st_ino == b.st_ino,
	       "hard links relinked at target");
	char buf[64] = {};
	EXPECT(readlink(dst + "/link to a", buf, 63) > 0 && String(buf) == "a b.txt", "symlink copied as a link");
	if(xattrs) {
		char x[16] = {};
		EXPECT(getxattr(dst + "/a b.txt", "user.treesync-test", x, 15) == 4 && String(x) == "kept",
		       "extended attribute copied");
	}
	else
		Cout() << "(skipped: " << GetTempDirectory() << " does not take user.* extended attributes)\n";
	EXPECT(Sha256Sum(src + "/big.bin") == Sha256Sum(dst + "/big.bin"), "big file: sha256sum matches");
	String h;
	String err;
	int sfd = OpenDir(src);
	EXPECT(HashAt(sfd, "big.bin", h, err) && h == Sha256Sum(src + "/big.bin"), "HashAt equals sha256sum");
	close(sfd);

	// Second run: nothing to do, nothing re-hashed.
	Sync(s, r);
	EXPECT(r.planned == 0, Format("second run plans nothing (%d)", r.planned));
	EXPECT(r.source.hashed == 0 && r.source.reused > 0, Format("second run re-hashes no source file (%d hashed)", r.source.hashed));
	EXPECT(r.target.hashed == 0, Format("verified copies are cached: no target file re-hashed (%d)", r.target.hashed));

	// Extra file at target: mirror deletes it; --keep-extra and --copy keep it.
	Put(dst + "/extra.txt", "extra\n");
	s.flags.keep_extra = true;
	Sync(s, r);
	EXPECT(FileExists(dst + "/extra.txt"), "--keep-extra keeps extra target file");
	s.flags.keep_extra = false;
	s.flags.mode = SyncMode::Copy;
	Sync(s, r);
	EXPECT(FileExists(dst + "/extra.txt"), "--copy keeps extra target file");
	s.flags.mode = SyncMode::Mirror;
	Sync(s, r);
	EXPECT(!FileExists(dst + "/extra.txt"), "--mirror removes extra target file");
	EXPECT(LoadFile(dst + ".old/extra.txt") == "extra\n", "--safe-delete moved it to <target>.old");

	// A changed file is copied again; the version it replaces moves out.
	Put(src + "/a b.txt", "changed\n");
	Sync(s, r);
	EXPECT(LoadFile(dst + "/a b.txt") == "changed\n", "changed file copied");
	EXPECT(LoadFile(dst + ".old/a b.txt") == "hello\n", "--safe-delete kept the replaced version");

	// The same name again: numbered, nothing overwritten in .old.
	Put(dst + "/extra.txt", "extra 2\n");
	Sync(s, r);
	EXPECT(LoadFile(dst + ".old/extra.txt") == "extra\n" && LoadFile(dst + ".old/extra.txt.1") == "extra 2\n",
	       "second extra.txt moved to extra.txt.1");

	// --no-safe-delete really deletes.
	Put(dst + "/extra.txt", "extra 3\n");
	s.flags.safe_delete = false;
	Sync(s, r);
	EXPECT(!FileExists(dst + "/extra.txt") && !FileExists(dst + ".old/extra.txt.2"), "--no-safe-delete deletes");
	s.flags.safe_delete = true;

	// The .old folder may not be inside either tree.
	{
		SyncSetup bad = s;
		bad.safe_delete_dir = dst + "/old";
		SyncReport rep;
		String err;
		EXPECT(!RunSync(bad, rep, r, err) && err.Find("outside both trees") >= 0, "--safe-delete-dir inside a tree refused: " + err);
	}

	// --no-hard-links writes each name out as its own file.
	String dst2 = root + "/dst-no-links";
	SyncSetup s2 = Setup(src, dst2, db);
	s2.flags.hard_links = false;
	Sync(s2, r);
	EXPECT(stat(dst2 + "/hard 1", &a) == 0 && stat(dst2 + "/hard 2", &b) == 0 && a.st_ino != b.st_ino,
	       "--no-hard-links writes separate copies");

	// --follow-links writes out the file a symlink points to.
	String dst3 = root + "/dst-follow";
	SyncSetup s3 = Setup(src, dst3, db);
	s3.flags.follow_links = true;
	Sync(s3, r);
	struct stat l;
	EXPECT(lstat(dst3 + "/link to a", &l) == 0 && S_ISREG(l.st_mode) && LoadFile(dst3 + "/link to a") == "changed\n",
	       "--follow-links writes out the symlink target");
}

static void TestEngineResume(const String& root)
{
	String src = root + "/rsrc", dst = root + "/rdst", db = root + "/sync.db", check = root + "/check.db";
	int64 size = 3 * 1024 * 1024 + 512 * 1024;
	Put(src + "/a big.bin", Big(size, 7));
	Put(src + "/z small 1.txt", "one\n");
	Put(src + "/z small 2.txt", "two\n");

	// Stop as soon as the first chunk of the big file is recorded. (With several copy
	// threads the small files may finish first, so watch the big file itself.) Not on
	// the first progress: on a slow run (valgrind) that comes mid-chunk, or while the
	// chunk is read back (verifying), before it is recorded; the second run would then
	// rightly start the file over.
	SyncSetup s = Setup(src, dst, db);
	int64 chunk = (int64)s.flags.chunk_mib << 20;
	std::atomic<bool> stop = false;
	SyncResult r1;
	Sync(s, r1, nullptr, &stop, [&](const CopyProgress& p) {
		if(p.name == "a big.bin" && !p.verifying && p.done >= chunk)
			stop = true;
	});
	EXPECT(r1.stopped, "first run stopped");
	EXPECT(!FileExists(dst + "/a big.bin") && FileExists(dst + "/a big.bin.treesync-part"),
	       "stopped mid-file: only the .treesync-part exists");

	// Second run resumes the same run and copies only what is left.
	stop = false;
	SyncResult r2;
	Vector<String> lines;
	Sync(s, r2, &lines, &stop);
	EXPECT(r2.resumed && r2.run == r1.run, Format("resumed run %d (first was %d)", r2.run, r1.run));
	EXPECT(r2.bytes < size, Format("resume copied %s, less than the %s file", FormatBytes(r2.bytes), FormatBytes(size)));
	bool said = false;
	String about;   // what the run said about the big file, shown when the check fails
	for(const String& l : lines) {
		said = said || l.StartsWith("resuming a big.bin at chunk 2");
		if(l.Find("a big.bin") >= 0)
			about << (about.GetCount() ? " | " : "") << l;
	}
	EXPECT(said, "reports resuming at chunk 2" + (said ? String() : " (said: " + about + ")"));
	EXPECT(TreeHash(src, check) == TreeHash(dst, check), "resumed copy: target equals source");
	EXPECT(!FileExists(dst + "/a big.bin.treesync-part"), "no .treesync-part left");

	// --fresh ignores an unfinished run.
	SyncResult r3;
	Put(src + "/new.txt", "new\n");
	s.fresh = true;
	Sync(s, r3);
	EXPECT(!r3.resumed && r3.run != r1.run, "--fresh starts a new run");
}

static void TestEngineMerge(const String& root)
{
	String a = root + "/ma", b = root + "/mb", db = root + "/sync.db";
	Put(a + "/same.txt", "same\n");
	Put(b + "/same.txt", "same\n");
	Put(a + "/only a.txt", "a\n");
	Put(b + "/only b/x.txt", "b\n");
	SyncSetup s = Setup(a, b, db);
	s.flags.mode = SyncMode::Merge;
	SyncResult r;
	Sync(s, r);
	EXPECT(FileExists(b + "/only a.txt") && FileExists(a + "/only b/x.txt"), "merge copies new files both ways");

	Put(a + "/same.txt", "changed in a\n");
	Sync(s, r);
	EXPECT(LoadFile(b + "/same.txt") == "changed in a\n", "merge: change in source goes to target");

	Put(b + "/same.txt", "changed in b\n");
	Sync(s, r);
	EXPECT(LoadFile(a + "/same.txt") == "changed in b\n", "merge: change in target goes to source");

	Put(a + "/same.txt", "both 1\n");
	Put(b + "/same.txt", "both 2\n");
	Sync(s, r);
	EXPECT(r.conflicts == 1 && LoadFile(a + "/same.txt") == "both 1\n" && LoadFile(b + "/same.txt") == "both 2\n",
	       Format("merge: changed on both sides is a conflict, nothing overwritten (%d)", r.conflicts));
	EXPECT(LoadFile(b + ".old/same.txt") == "same\n" && LoadFile(a + ".old/same.txt") == "changed in a\n",
	       "merge: replaced versions on both sides kept in their .old folders");
}

// Include and exclude: blank boxes filter nothing, every filled box narrows, exclude
// wins, folders are made only where something included goes, and Mirror deletes only
// what the filters would include.
static void TestFilters(const String& root)
{
	String src = root + "/fsrc", db = root + "/sync.db";
	Put(src + "/vms/a.qcow2", "vm a\n");
	Put(src + "/vms/b.qcow2", "vm b\n");
	Put(src + "/vms/notes.txt", "notes\n");
	Put(src + "/other/c.txt", "c\n");
	Put(src + "/photos/2024/p.jpg", "p\n");
	Put(src + "/photos/x.txt", "x\n");
	Put(src + "/deep/home/Documents/d.txt", "d\n");
	Put(src + "/deep/home/z.txt", "z\n");
	RealizeDirectory(src + "/empty");
	symlink("a.qcow2", src + "/vms/link.qcow2");
	auto run = [&](const String& dst, auto set) {
		SyncSetup s = Setup(src, dst, db);
		set(s.flags);
		SyncResult r;
		Sync(s, r);
	};
	auto has = [](const String& p) { return FileExists(p) || DirectoryExists(p); };

	String d1 = root + "/f1";   // one VM out of a folder of them
	run(d1, [](SyncFlags& f) { f.include_files = " b.qcow2 "; });
	EXPECT(LoadFile(d1 + "/vms/b.qcow2") == "vm b\n" && !has(d1 + "/vms/a.qcow2") && !has(d1 + "/vms/notes.txt") &&
	       !has(d1 + "/vms/link.qcow2"), "--include-file: only that file");
	EXPECT(!has(d1 + "/other") && !has(d1 + "/photos") && !has(d1 + "/empty") && !has(d1 + "/deep"),
	       "--include-file: no folders that hold nothing included");

	String d2 = root + "/f2";   // folder names at any depth, with everything in them
	run(d2, [](SyncFlags& f) { f.include_dirs = "photos;Documents"; });
	EXPECT(has(d2 + "/photos/2024/p.jpg") && has(d2 + "/photos/x.txt") && has(d2 + "/deep/home/Documents/d.txt"),
	       "--include-dir: what is inside the folders, at any depth");
	EXPECT(!has(d2 + "/deep/home/z.txt") && !has(d2 + "/vms") && !has(d2 + "/empty"),
	       "--include-dir: nothing outside them");

	String d3 = root + "/f3";   // both: each narrows
	run(d3, [](SyncFlags& f) { f.include_dirs = "photos"; f.include_files = "*.jpg"; });
	EXPECT(has(d3 + "/photos/2024/p.jpg") && !has(d3 + "/photos/x.txt") && !has(d3 + "/deep"),
	       "--include-dir with --include-file: both narrow");

	String d4 = root + "/f4";   // exclude wins
	run(d4, [](SyncFlags& f) { f.include_files = "*.qcow2"; f.exclude_files = "a.qcow2;link.qcow2"; });
	EXPECT(has(d4 + "/vms/b.qcow2") && !has(d4 + "/vms/a.qcow2") && !has(d4 + "/vms/link.qcow2"),
	       "exclude wins over include");

	String d5 = root + "/f5";   // blank boxes: everything, empty folder too
	run(d5, [](SyncFlags& f) { f.include_files = " ; "; f.include_dirs = ""; });
	EXPECT(has(d5 + "/vms/a.qcow2") && has(d5 + "/deep/home/z.txt") && DirectoryExists(d5 + "/empty"),
	       "blank include boxes filter nothing");

	// Mirror deletes only what the filters include, also inside extra folders.
	String d6 = root + "/f6";
	Put(d6 + "/keep.txt", "keep\n");
	Put(d6 + "/vms/a.qcow2", "old vm a\n");
	Put(d6 + "/gone/b.qcow2", "old\n");
	Put(d6 + "/mixed/b.qcow2", "old\n");
	Put(d6 + "/mixed/keep.me", "keep\n");
	run(d6, [](SyncFlags& f) { f.mode = SyncMode::Mirror; f.include_files = "b.qcow2"; });
	EXPECT(LoadFile(d6 + "/keep.txt") == "keep\n" && LoadFile(d6 + "/vms/a.qcow2") == "old vm a\n" &&
	       LoadFile(d6 + "/mixed/keep.me") == "keep\n", "Mirror leaves what is not included alone");
	EXPECT(!has(d6 + "/gone") && !has(d6 + "/mixed/b.qcow2") && has(d6 + "/vms/b.qcow2"),
	       "Mirror deletes included extras, and a folder left empty by them");

	// Merge: the same filters on both sides.
	String a = root + "/fma", b = root + "/fmb";
	Put(a + "/vms/b.qcow2", "vm b\n");
	Put(a + "/vms/a.qcow2", "vm a\n");
	Put(b + "/other/y.txt", "y\n");
	SyncSetup s = Setup(a, b, db);
	s.flags.mode = SyncMode::Merge;
	s.flags.include_files = "b.qcow2";
	SyncResult r;
	Sync(s, r);
	EXPECT(has(b + "/vms/b.qcow2") && !has(b + "/vms/a.qcow2") && !has(a + "/other"),
	       "Merge: only included files go either way");

	// On the command line and back.
	SyncSetup c;
	String err;
	EXPECT(ParseCommandLine(Vector<String>{ "--include-file=b.qcow2", "--include-file", "*.iso", "--include-dir=vms" },
	                        c, err) == PARSE_OK && c.flags.include_files == "b.qcow2;*.iso" && c.flags.include_dirs == "vms",
	       "--include-file / --include-dir parse: " + err);
	Vector<String> args = FlagArgs(c.flags);
	EXPECT(FindIndex(args, String("--include-file=*.iso")) >= 0 && FindIndex(args, String("--include-dir=vms")) >= 0,
	       "include lists written back as arguments");
}

static void TestThreads(const String& root)
{
	String src = root + "/th-src", dst = root + "/th-dst", db = root + "/sync.db", check = root + "/check.db";
	for(int i = 0; i < 300; i++)
		Put(Format("%s/folder %d/sub %d/file %d.txt", src, i % 7, i % 3, i), Big(1000 + i * 37, i));
	Put(src + "/big one.bin", Big(3 * 1024 * 1024 + 7, 99));   // chunked, alongside the small ones

	// Stop once about 50 files are done: several copies are in flight at that moment.
	SyncSetup s = Setup(src, dst, db);
	s.flags.threads = 8;
	std::atomic<bool> stop = false;
	std::atomic<int> finished = 0;
	SyncResult r1;
	Sync(s, r1, nullptr, &stop, [&](const CopyProgress& p) {
		if(p.done >= p.size && ++finished >= 50)
			stop = true;
	});
	EXPECT(r1.stopped && r1.done < r1.planned, Format("8 threads stopped part-way (%d of %d done)", r1.done, r1.planned));

	// A different thread count (and bandwidth limit) still resumes the same run.
	stop = false;
	SyncResult r2;
	Vector<String> lines;
	SyncSetup s3 = s;
	s3.flags.threads = 3;
	s3.flags.bwlimit_kib = 1048576;
	Sync(s3, r2, &lines, &stop);
	EXPECT(r2.resumed && r2.run == r1.run && r2.failed == 0 && r2.done == r2.planned,
	       Format("resumed with 3 threads instead of 8: %d of %d done, %d failed", r2.done, r2.planned, r2.failed));
	bool said = false;
	for(const String& l : lines)
		said = said || l.Find("copy (3 threads)") >= 0;
	EXPECT(said, "reports 3 copy threads");
	EXPECT(TreeHash(src, check) == TreeHash(dst, check), "threads: target tree hash equals source");

	// --threads=1 gives the same result.
	String dst1 = root + "/th-dst1";
	SyncSetup one = Setup(src, dst1, db);
	one.flags.threads = 1;
	SyncResult r3;
	Sync(one, r3);
	EXPECT(r3.failed == 0 && TreeHash(src, check) == TreeHash(dst1, check), "1 thread: same result");
}

// The streaming engine: copies start while the pre-scan is still walking, and the
// post-scan checks every folder the pre-scan found.
static void TestStreaming(const String& root)
{
	String src = root + "/st-src", dst = root + "/st-dst", db = root + "/sync.db", check = root + "/check.db";
	for(int i = 0; i < 2000; i++)
		Put(Format("%s/d%02d/e%d/f%d.txt", src, i % 20, i % 2, i), Big(200 + i % 50, i));
	Put(dst + "/d07/e1/extra in the target.txt", "extra\n");

	SyncSetup s = Setup(src, dst, db);
	s.flags.threads = 4;
	SyncReport report;
	String err;
	SyncResult r;
	auto t0 = std::chrono::steady_clock::now();
	std::atomic<double> first_copy = -1, scan_end = -1;
	int64 found = 0, checked = 0;
	int ahead = 0, bad_count = 0, last_count = -1;
	auto since = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
	report.WhenProgress = [&](const CopyProgress& p) {
		if(p.done >= p.size && first_copy < 0)
			first_copy = since();
		if(p.done >= p.size && !p.verifying) {   // the file counter: calls are serialized
			ahead += p.count > p.index;
			bad_count += p.index > p.count;
			if(p.index == 2000)
				last_count = p.count;
		}
	};
	report.WhenStage = [&](const StageProgress& st) {
		if(st.stage == 0 && st.finished)
			scan_end = since();
		if(st.stage == 2 && st.finished) {
			checked = st.done;
			found = st.total;
		}
	};
	EXPECT(RunSync(s, report, r, err), "streaming run: " + err);
	// A race between threads; valgrind runs one thread at a time (build.sh --valgrind).
	if(GetEnv("TREESYNC_VALGRIND").GetCount())
		Cout() << "(skipped under valgrind: first copy finished before the pre-scan did)\n";
	else
		EXPECT(first_copy >= 0 && scan_end >= 0 && first_copy < scan_end,
		       Format("first copy finished (%.3f s) before the pre-scan did (%.3f s)", (double)first_copy, (double)scan_end));
	EXPECT(ahead > 0 && bad_count == 0, Format("file counter runs ahead of the copies (%d of 2000 files)", ahead));
	EXPECT(last_count == 2000, Format("last file shows 2000/2000 (count %d)", last_count));
	EXPECT(found == 41 && checked == found, Format("post-scan checked all %d folders (%d)", found, checked));
	EXPECT(!FileExists(dst + "/d07/e1/extra in the target.txt") &&
	       FileExists(dst + ".old/d07/e1/extra in the target.txt"), "post-scan moved the extra file out");
	EXPECT(TreeHash(src, check) == TreeHash(dst, check), "streamed copy equals the source");

	// Every copy keeps both SHA-256 for the user to see (file_compare), its folder stored once by id,
	// and its attributes on both sides, which must agree.
	{
		Db d;
		String derr;
		EXPECT(d.Open(db, derr), "open the run's database: " + derr);
		Stmt q(d, "SELECT count(*), sum(match = 'true'), sum(length(src_sha256) = 64 AND src_sha256 = dst_sha256) "
		          "FROM file_compare WHERE run = ? AND kind = 'copy'");
		q.Int(1, r.run);
		EXPECT(q.Step() && q.Int(0) == 2000 && q.Int(1) == 2000 && q.Int(2) == 2000,
		       Format("file_compare: all 2000 copies with matching hashes (%lld, %lld match)", q.Int(0), q.Int(1)));
		Stmt n(d, "SELECT path, file_name FROM file_compare WHERE run = ? AND file_name = 'f7.txt'");
		n.Int(1, r.run);
		EXPECT(n.Step() && n.Text(0) == "d07/e1" && n.Text(1) == "f7.txt",
		       "file_compare splits d07/e1/f7.txt into path and file name");
		Stmt f(d, "SELECT count(DISTINCT folder_id), count(DISTINCT path) FROM file_compare WHERE run = ?");
		f.Int(1, r.run);
		int64 ids = f.Step() ? f.Int(0) : -1, paths = f.Int(1);
		EXPECT(ids == 20 && paths == 20,
		       Format("file_compare: 20 folders, each stored once (%lld ids, %lld paths)", ids, paths));
		Stmt a(d, "SELECT count(*) FROM file_compare WHERE run = ? AND attrs_match = 'true'");
		a.Int(1, r.run);
		EXPECT(a.Step() && a.Int(0) == 2000, "every copy kept its attributes (attrs_match)");
	}

	// The AI exports of that run: organized (the sections) and complete (every action).
	String out_logs, out_sql, ai_err;
	EXPECT(ExportAiLogs(db, r.run, out_logs, ai_err) && ExportAiSql(db, r.run, out_sql, ai_err), "AI export: " + ai_err);
	String l = LoadFile(out_logs), q = LoadFile(out_sql);
	EXPECT(l.Find("## How to read this") >= 0 && l.Find("## 7. The logs in full") >= 0, "AI logs export has its sections");
	int rows = 0;
	for(int at = q.Find("## 8. This run: every action"); (at = q.Find("| copy |", at + 1)) >= 0;)
		rows++;
	EXPECT(q.Find("CREATE TABLE") >= 0 && rows == 2000, Format("AI SQL export: schema and all 2000 copies (%d)", rows));
}

// Scan only, Sync attributes, attribute-only fixes in a sync, and --clear-old.
static int64 Count(Db& d, const String& sql, int64 run)
{
	Stmt q(d, sql);
	q.Int(1, run);
	return q.Step() ? q.Int(0) : -1;
}

static void TestChecks(const String& root)
{
	String src = root + "/ck-src", dst = root + "/ck-dst", db = root + "/checks.db";
	for(int i = 0; i < 30; i++)
		Put(Format("%s/d%d/f%d.txt", src, i % 3, i), Big(1000 + i, i));
	SyncSetup s = Setup(src, dst, db);
	SyncResult r;
	Sync(s, r);
	EXPECT(r.failed == 0 && r.done > 0, "checks: first copy");

	// Changes: a source file's mode and another's time (same content), a target file's
	// content, a target file gone, a source folder's mode, an extended attribute.
	chmod(src + "/d0/f0.txt", 0600);
	struct timespec old_times[2] = { { 1600000000, 0 }, { 1600000000, 0 } };
	utimensat(AT_FDCWD, src + "/d1/f1.txt", old_times, 0);
	Put(dst + "/d2/f2.txt", "changed on the target\n");
	unlink(dst + "/d0/f3.txt");
	chmod(src + "/d1", 0700);
	bool xattrs = setxattr(src + "/d2/f5.txt", "user.treesync-test", "x", 1, 0) == 0;

	SyncReport report;
	String err;
	CheckResult c;
	EXPECT(RunCheck(s, CheckMode::Scan, report, c, err), "scan only: " + err);
	// (Each message is built after its call: Format's arguments may be evaluated first.)
	bool ok = c.files == 30 && c.same == 28 && c.differ == 1 && c.missing == 1 && c.failed == 0;
	EXPECT(ok, Format("scan only: 30 files, 28 same, 1 differs, 1 missing (%d, %d, %d, %d)", c.files, c.same, c.differ, c.missing));
	EXPECT(c.attr_diffs == 4 + xattrs, Format("scan only: %d attribute differences (%d)", 4 + xattrs, c.attr_diffs));
	struct stat st;
	EXPECT(stat(dst + "/d0/f0.txt", &st) == 0 && (st.st_mode & 07777) == 0644, "scan only changed nothing");
	{
		Db d;
		d.Open(db, err);
		EXPECT(Count(d, "SELECT count(*) FROM hash_mismatches WHERE run = ?", c.run) == 2, "hash_mismatches: 2 rows");
		EXPECT(Count(d, "SELECT count(*) FROM hash_mismatches WHERE run = ? AND problem = 'missing on the target' "
		                "AND file_name = 'f3.txt'", c.run) == 1, "hash_mismatches: f3.txt missing on the target");
		EXPECT(Count(d, "SELECT count(*) FROM attr_mismatches WHERE run = ? AND file_name = 'f0.txt' "
		                "AND attr_diff = 'mode 0644 -> 0600'", c.run) == 1, "attr_mismatches: f0.txt mode");
		EXPECT(Count(d, "SELECT count(*) FROM attr_mismatches WHERE run = ? AND type = 'd' AND file_name = 'd1'", c.run) == 1,
		       "attr_mismatches: folder d1 mode");
	}

	// Sync attributes: the dry run sets nothing; the real one sets the same-size ones.
	s.flags.dry_run = true;
	ok = RunCheck(s, CheckMode::Attrs, report, c, err);
	EXPECT(ok && c.fixed == 3 + xattrs, Format("sync attributes, dry run: %d would be set (%d)", 3 + xattrs, c.fixed));
	EXPECT(stat(dst + "/d0/f0.txt", &st) == 0 && (st.st_mode & 07777) == 0644, "attributes dry run changed nothing");
	s.flags.dry_run = false;
	ok = RunCheck(s, CheckMode::Attrs, report, c, err);
	EXPECT(ok && c.fixed == 3 + xattrs && c.failed == 0,
	       Format("sync attributes: %d set (%d), %d failed", 3 + xattrs, c.fixed, c.failed));
	EXPECT(stat(dst + "/d0/f0.txt", &st) == 0 && (st.st_mode & 07777) == 0600, "sync attributes: mode set");
	EXPECT(stat(dst + "/d1/f1.txt", &st) == 0 && st.st_mtim.tv_sec == 1600000000, "sync attributes: time set");
	EXPECT(stat(dst + "/d1", &st) == 0 && (st.st_mode & 07777) == 0700, "sync attributes: folder mode set");
	EXPECT(stat(dst + "/d2/f2.txt", &st) == 0 && st.st_size == 22, "sync attributes: changed content left for a sync");
	if(xattrs) {
		char x[4] = {};
		EXPECT(getxattr(dst + "/d2/f5.txt", "user.treesync-test", x, 3) == 1, "sync attributes: extended attribute set");
	}

	// A sync copies what differs and sets attribute-only changes as "attrs" steps.
	chmod(src + "/d2/f8.txt", 0640);
	Sync(s, r);
	EXPECT(stat(dst + "/d2/f8.txt", &st) == 0 && (st.st_mode & 07777) == 0640, "sync: attribute-only change set");
	{
		Db d;
		d.Open(db, err);
		EXPECT(Count(d, "SELECT count(*) FROM actions WHERE run = ? AND kind = 'attrs' AND state = 'done'", r.run) == 1,
		       "sync: one attrs step");
	}
	ok = RunCheck(s, CheckMode::Scan, report, c, err);
	EXPECT(ok && c.same == 30 && c.attr_diffs == 0,
	       Format("scan after the sync: all 30 the same, no attribute differences (%d, %d)", c.same, c.attr_diffs));

	// --clear-old: the replaced f2.txt went to <target>.old.
	EXPECT(DirectoryExists(dst + ".old"), "safe delete made " + dst + ".old");
	EXPECT(ClearOld(s, true, report, err) && DirectoryExists(dst + ".old"), "clear .old, counting only: " + err);
	EXPECT(ClearOld(s, false, report, err) && !DirectoryExists(dst + ".old"), "clear .old deletes it: " + err);
	SyncSetup bad = s;
	bad.safe_delete_dir = src;
	EXPECT(!ClearOld(bad, false, report, err) && err.Find("refuses") >= 0 && DirectoryExists(src),
	       "clear .old refuses a tree: " + err);
}

// --wipe-target: the target is emptied first, into <target>.old with safe delete.
static void TestWipe(const String& root)
{
	String src = root + "/w-src", dst = root + "/w-dst", db = root + "/sync.db", check = root + "/check.db";
	Put(src + "/keep/a.txt", "a\n");
	Put(src + "/b.txt", "b\n");
	auto fill_target = [&] {
		Put(dst + "/keep/a.txt", "a\n");               // the same as the source
		Put(dst + "/old folder/x.txt", "x\n");         // only on the target
		Put(dst + "/b.txt", "an older b\n");          // different
	};
	fill_target();

	SyncSetup s = Setup(src, dst, db);
	s.flags.wipe_target = true;
	s.flags.dry_run = true;
	SyncResult r;
	Vector<String> lines;
	Sync(s, r, &lines);
	int wipes = 0, copies = 0;
	for(const String& l : r.plan) {
		wipes += l.StartsWith("wipe");
		copies += l.StartsWith("copy");
	}
	EXPECT(wipes == 3 && copies == 2 && FileExists(dst + "/old folder/x.txt"),
	       Format("dry run lists 3 wipes and 2 copies as if empty (%d, %d), changes nothing", wipes, copies));

	s.flags.dry_run = false;
	Sync(s, r);
	EXPECT(r.failed == 0 && TreeHash(src, check) == TreeHash(dst, check), "after the wipe the target equals the source");
	EXPECT(FileExists(dst + ".old/old folder/x.txt") && LoadFile(dst + ".old/b.txt") == "an older b\n" &&
	       FileExists(dst + ".old/keep/a.txt"), "with safe delete, everything that was there is in .old");

	fill_target();
	s.flags.safe_delete = false;
	Sync(s, r);
	EXPECT(!FileExists(dst + "/old folder/x.txt") && !FileExists(dst + ".old/old folder/x.txt.1") &&
	       TreeHash(src, check) == TreeHash(dst, check), "without safe delete the old contents are deleted");

	// Stopped part-way and resumed: the resumed run must not empty the target again
	// (that would move the files it already copied into .old).
	String src2 = root + "/w2-src", dst2 = root + "/w2-dst";
	for(int i = 0; i < 60; i++)
		Put(Format("%s/f%02d.txt", src2, i), Big(5000 + i, i));
	Put(dst2 + "/junk.txt", "junk\n");
	SyncSetup w = Setup(src2, dst2, db);
	w.flags.wipe_target = true;
	w.flags.threads = 1;
	std::atomic<bool> halt = false;
	std::atomic<int> done = 0;
	SyncResult r1, r2;
	Sync(w, r1, nullptr, &halt, [&](const CopyProgress& p) {
		if(p.done >= p.size && ++done >= 10)
			halt = true;
	});
	halt = false;
	Vector<String> wl;
	Sync(w, r2, &wl, &halt);
	bool said = false;
	for(const String& l : wl)
		said = said || l.Find("not emptying the target again") >= 0;
	int moved_again = 0;
	for(int i = 0; i < 60; i++)
		moved_again += FileExists(Format("%s.old/f%02d.txt", dst2, i));
	EXPECT(r1.stopped && r2.resumed && said && moved_again == 0 && FileExists(dst2 + ".old/junk.txt") &&
	       TreeHash(src2, check) == TreeHash(dst2, check),
	       Format("resumed wipe run does not wipe again (%d copied files moved to .old)", moved_again));

	String why;
	EXPECT(!WipeAllowed("/", why) && !WipeAllowed("/home", why) && !WipeAllowed("/etc/", why) &&
	       !WipeAllowed(HomeDir(), why) && WipeAllowed(root + "/w-dst", why), "wipe refused for / /home /etc and home");
	SyncSetup m = Setup(src, dst, db);
	m.flags.mode = SyncMode::Merge;
	m.flags.wipe_target = true;
	SyncReport rep;
	EXPECT(!RunSync(m, rep, r, why) && why.Find("not allowed with --merge") >= 0, "wipe refused with --merge");
}

// treesync-cli run the way the GUI runs it (--stop-on-eof, stdin held open):
// it must exit cleanly, without U++'s debug "Heap leaks detected!".
static void TestCliLikeGui(const String& root)
{
	String cli = AppendFileName(GetExeFolder(), "treesync-cli");
	if(!FileExists(cli)) {
		Cout() << "(skipped: no treesync-cli next to this test, e.g. in the ThreadSanitizer build)\n";
		return;
	}
	Put(root + "/gui-src/a.txt", "a\n");
	String cmd = "(sleep 1) | '" + cli + "' --source='" + root + "/gui-src' --target='" + root +
	             "/gui-dst' --db='" + root + "/sync.db' --stop-on-eof --gui-progress 2>&1; echo EXIT=$?";
	String out;
	if(FILE *f = popen(cmd, "r")) {
		char buf[4096];
		while(size_t n = fread(buf, 1, sizeof(buf), f))
			out.Cat(buf, (int)n);
		pclose(f);
	}
	EXPECT(out.Find("Heap leaks") < 0, "treesync-cli --stop-on-eof: no heap leak report");
	EXPECT(out.Find("EXIT=0") >= 0, "treesync-cli --stop-on-eof exits with 0: " + out.Right(80));
}

static void TestBandwidthAndPriority(const String& root)
{
	String src = root + "/bw-src", db = root + "/sync.db";
	Put(src + "/three mib.bin", Big(3 * 1024 * 1024, 3));
	auto timed = [&](const String& dst, int limit) {
		SyncSetup s = Setup(src, dst, db);
		s.flags.big_file_mib = 1024;   // a small file: without a limit it goes through cp
		s.flags.bwlimit_kib = limit;
		SyncResult r;
		auto t0 = std::chrono::steady_clock::now();
		Sync(s, r);
		EXPECT(r.failed == 0 && LoadFile(dst + "/three mib.bin").GetCount() == 3 * 1024 * 1024,
		       Format("copy with --bwlimit=%d arrived whole", limit));
		return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	};
	double fast = timed(root + "/bw-fast", 0);
	double slow = timed(root + "/bw-slow", 1024);
	EXPECT(slow >= 1.6 && slow < 6, Format("--bwlimit=1024 KiB/s: 3 MiB took %.2f s (about 2 s expected)", slow));
	EXPECT(fast < slow, Format("no limit is faster (%.2f s)", fast));

	SyncSetup s = Setup(src, root + "/bw-prio", db);
	s.flags.low_priority = true;
	SyncResult r;
	Sync(s, r);
	errno = 0;
	int nice = getpriority(PRIO_PROCESS, 0);
	long io = syscall(SYS_ioprio_get, 1, 0);
	EXPECT(nice == 19, Format("--low-priority sets nice 19 (%d)", nice));
	EXPECT(io == ((2 << 13) | 7), Format("--low-priority sets I/O best-effort 7 (0x%x)", (int)io));
}

static void TestEngineRefusals(const String& root)
{
	SyncReport report;
	SyncResult r;
	String err;
	SyncSetup s = Setup(root + "/src", root + "/src/inside", root + "/sync.db");
	EXPECT(!RunSync(s, report, r, err) && err.Find("overlap") >= 0, "refuses overlapping trees: " + err);
	if(!IsRoot()) {
		SyncSetup t = Setup("/etc", root + "/etc-copy", root + "/sync.db");
		EXPECT(!RunSync(t, report, r, err) && err.Find("outside your home") >= 0, "refuses /etc without root: " + err);
	}
}

CONSOLE_APP_MAIN
{
	Cout() << ToolchainReport() << "\n";

	String root = AppendFileName(GetTempDirectory(), Format("treesync-test-%d", (int)getpid()));
	mkdir(root, 0755);

	TestIsUnder();
	TestListFolder(root);
	TestCleanPath(root);
	TestOptions();
	TestPathAllowed();
	TestSettings(root);
	TestMessages();
	TestIds();

	// The engine refuses paths outside home without root, so its tests run there.
	String eroot = IsRoot() ? root + "-engine"
	                        : AppendFileName(HomeDir(), Format(".cache/treesync-test-%d", (int)getpid()));
	RealizeDirectory(eroot);
	TestEngineMirror(eroot);
	TestEngineResume(eroot);
	TestEngineMerge(eroot);
	TestFilters(eroot);
	TestEngineRefusals(eroot);
	TestThreads(eroot);
	TestCliLikeGui(eroot);
	TestStreaming(eroot);
	TestChecks(eroot);
	TestWipe(eroot);
	TestBandwidthAndPriority(eroot);   // last: it lowers this process's priority
	int efd = open(GetFileFolder(eroot), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	RemoveAt(efd, GetFileName(eroot));
	close(efd);
	TestProgress();

	int fd = open(GetFileFolder(root), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	RemoveAt(fd, GetFileName(root));
	close(fd);

	Cout() << (failures ? "FAILED: " : "passed: ") << checks - failures << "/" << checks << " checks\n";
	SetExitCode(failures ? 1 : 0);
}
