// Console tests for TreeSyncCore. Exit code 0 = all passed.
// Builds a scratch tree under $TMPDIR with awkward names (spaces at either end,
// tab, newline, quotes, backslash, UTF-8) and a folder chain deeper than PATH_MAX.
#include <TreeSyncCore/TreeSyncCore.h>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
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
		{ "--source=/home/jay/my dir", "--target=/mnt/it's here", "--merge", "--fat-file-times",
		  "--no-dry-run", "--threads", "16", "--exclude-file", "*.tmp", "--exclude-file",
		  "Thumbs db", "--exclude-dir=.git" },
		{ "--source=/data", "--target-host=nas.local", "--target-user=jay", "--target-port=2222",
		  "--target-key=/home/jay/.ssh/id ed25519", "--target=/backup", "--copy", "--retries=0" },
		{ "--mirror", "--no-skip-links", "--skip-older", "--skip-newer", "--keep-extra",
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
}

static void TestMessages()
{
	String m = NotRootMessage();
	EXPECT(Split(m, '\n').GetCount() == 3, "not-root message has 3 lines: " + Show(m));
	EXPECT(m.Find("run TreeSync as root") >= 0, "not-root message says how to fix it: " + Show(m));
	String r = RefusedPathMessage("/my dir");
	EXPECT(r.StartsWith("/my dir is outside"), "refused message names the path: " + Show(r));
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
	EXPECT(ProgressLine(lines[1]).Find(" 25% ") >= 0, "partial file shows 25%");
	EXPECT(ProgressLine(lines[3]).Find("00m21s") >= 0, "elapsed time");
	Cout() << "\n";
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
	TestMessages();
	TestProgress();

	int fd = open(GetFileFolder(root), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	RemoveAt(fd, GetFileName(root));
	close(fd);

	Cout() << (failures ? "FAILED: " : "passed: ") << checks - failures << "/" << checks << " checks\n";
	SetExitCode(failures ? 1 : 0);
}
