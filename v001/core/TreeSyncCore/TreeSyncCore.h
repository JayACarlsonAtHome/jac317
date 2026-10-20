#ifndef TREESYNC_CORE_H
#define TREESYNC_CORE_H

// TreeSync engine side: settings, command-line options, paths, disks, privileges.
// Uses U++ Core only, never CtrlLib, so it works without a GUI; the TreeSync
// GUI package sits on top of it.
#include <Core/Core.h>

#include <filesystem>

#include "Contracts.h"

#ifndef PLATFORM_LINUX
#error "TreeSync is Linux-only for now"
#endif

// U++'s stock build methods pass -std=c++17; build with v001/bm/*.bm (v001/build.sh).
static_assert(__cplusplus > 202302L, "TreeSync requires C++26: build with v001/build.sh");

using namespace Upp;

// One side of the sync: a directory tree, local or on a remote machine.
struct TreeEndpoint {
	String path;
	bool   remote = false;
	String host;
	int    port = 22;
	String user;
	String key_file;   // optional: a specific key; otherwise ssh-agent, then ~/.ssh/id_*
	// No password field on purpose: TreeSync logs in with keys only, so a remote
	// side needs passwordless SSH set up beforehand.
};

// Copy:   copy new/changed files source -> target, never delete    (--copy,   robocopy /E)
// Mirror: make target match source, deleting extras                (--mirror, robocopy /MIR)
// Merge:  copy changes both ways; changed on both sides = conflict (--merge)
enum class SyncMode { Copy, Mirror, Merge };

// Sync switches; Options.cpp maps each to its --long-option (and robocopy switch).
// Content is compared by SHA-256; size + time decide whether a cached hash can be
// reused and, in Merge mode, which side is newer, so the time switches still matter.
struct SyncFlags {
	SyncMode mode = SyncMode::Mirror;
	bool   dry_run = true;
	bool   dst_tolerance = false;    // allow 1-hour shifts, give or take time_tolerance
	bool   follow_links = false;     // write out what symlinks point to
	bool   hard_links = true;        // relink hard-linked files instead of writing each out
	bool   compress = false;         // zlib on SSH connections
	bool   preserve_owner = true;    // copy owner and group, by name (where we may)
	bool   numeric_ids = false;      // ...by number instead
	bool   safe_delete = true;       // move deleted and replaced files to <target>.old
	bool   low_priority = false;     // nice 19, ionice best-effort 7
	bool   split_files = true;       // idle copy threads help with a big file's chunks
	bool   include_same = false;
	bool   skip_older = false;
	bool   skip_newer = false;
	bool   skip_changed = false;
	bool   keep_extra = false;
	bool   skip_source_only = false;
	bool   wipe_target = false;      // empty the target first (to .old with safe delete)
	bool   precheck = true;          // before copying: whether the copies can fit on the target
	String exclude_files;            // globs, ';'-separated
	String exclude_dirs;             // globs, ';'-separated
	String include_files;            // globs, ';'-separated; blank: every file
	String include_dirs;             // globs, ';'-separated; blank: every folder
	String include_paths;            // --include-path: checked files and folders (relative to the
	                                 // source), one per line; blank: everything
	int    retries = 3;
	int    retry_wait = 5;           // seconds
	int    threads = 4;          // copy threads; remote sides get one SSH connection each
	int    big_file_mib = 1024;      // same-size files this big are compared side by side, not hashed
	int    chunk_mib = 64;           // copies go in chunks this big; more than one is resumable
	int    bwlimit_kib = 0;          // KiB/s over the network (local: writes); 0 = no limit
	int    time_tolerance = 3;       // seconds: times of the two sides this close count as equal
	int    network_wait = 1800;      // seconds to wait for a lost remote side to come back; 0 = stop at once
};

// --time-tolerance in nanoseconds. Clocks and file times differ slightly between
// machines and filesystems (network shares, drift, FAT's 2-second steps), so times
// from the two sides are compared with this much slack.
inline int64 ToleranceNs(const SyncFlags& f) { return (int64)f.time_tolerance * 1000000000; }

// The defaults differ by mode. Copy adds and updates, so it does not overwrite a
// newer file on the target with an older one (--skip-older on); Mirror makes an
// exact copy, so the source always wins; Merge decides by what changed since the
// last sync, so the time switches do not apply.
SyncFlags DefaultsFor(SyncMode mode);

// --wipe-target: refuses "/", the home folder itself and system folders.
bool WipeAllowed(const String& path, String& why);

struct BoolOption {
	const char *name;                // --name / --no-name
	bool SyncFlags::*field;
	const char *robocopy;
	const char *text;
	const char *help;                // plain-language explanation, shown in the GUI
};

struct IntOption {
	const char *name;                // --name=N
	int SyncFlags::*field;
	const char *robocopy;
	const char *text;
	int min, max;
	const char *help;
};

// Help for the parts of the Flags tab that are not in the two tables.
extern const char *HELP_MODE[3];          // Copy, Mirror, Merge
extern const char *HELP_EXCLUDE_FILES;
extern const char *HELP_EXCLUDE_DIRS;
extern const char *HELP_INCLUDE_FILES;
extern const char *HELP_INCLUDE_DIRS;
extern const char *HELP_OWNERS;           // the Owners choice: off, by name, by number

constexpr int BOOL_OPTION_COUNT = 18;
constexpr int INT_OPTION_COUNT = 8;
extern const BoolOption BOOL_OPTIONS[];
extern const IntOption  INT_OPTIONS[];

struct SyncSetup {
	TreeEndpoint source;
	TreeEndpoint target;
	SyncFlags    flags;
	String       db;              // --db=PATH; empty = DefaultDbPath()
	String       known_hosts;     // --known-hosts=FILE; empty = ~/.ssh/known_hosts
	String       safe_delete_dir; // --safe-delete-dir=PATH on the target side; empty = <target>.old
	bool         fresh = false;   // --fresh: do not resume an unfinished run
};

// Shown in --help, the GUI's About box and --compare-ids output; the binding text
// is the Apache License 2.0, sections 7 (no warranty) and 8 (no liability).
extern const char *DISCLAIMER;

// Options.cpp
enum { PARSE_OK, PARSE_HELP, PARSE_ERROR };

int            ParseCommandLine(const Vector<String>& args, SyncSetup& s, String& error);
Vector<String> FlagArgs(const SyncFlags& f);
Vector<String> ToArgs(const SyncSetup& s);
String         CommandLineText(const Vector<String>& args);   // shell-quoted, ready to paste
String         ShellQuote(const String& arg);                 // one argument, safe for sh
String         Usage();

// Saved settings: command-line options, one per line ('#' starts a comment), in
// ~/.config/treesync/settings ($XDG_CONFIG_HOME). The GUI saves on every change
// (written to a new file, flushed, renamed over the old: a crash leaves the old
// settings or the new ones) and loads them at start. One-shot options (--dry-run,
// --fresh) are not kept. LoadSettings: false and no error when there is no file.
String         SettingsPath();
bool           SaveSettings(const String& path, const SyncSetup& s, String& error);
bool           LoadSettings(const String& path, SyncSetup& s, String& error);

// Disks.cpp
struct DiskInfo : Moveable<DiskInfo> {          // a physical disk (sda, nvme0n1)
	String id;
	String label;
};

struct MountInfo : Moveable<MountInfo> {        // a mounted filesystem
	String dir;
	String disk;    // DiskInfo::id or DISK_NETWORK
	String label;
};

inline const char *DISK_NETWORK = "*network";

Vector<DiskInfo>  ListDisks();
Vector<MountInfo> ListMounts();
bool              IsUnder(const String& p, const String& dir);
String            FormatBytes(int64 n);   // 1024-based: "953.9 GiB"

// FsPath.cpp: long-path safe folder listing and path cleanup.
struct FolderEntry : Moveable<FolderEntry> {
	String name;
	bool   is_dir = false;    // follows symlinks
	bool   is_link = false;
	// The entry itself (not what a symlink points to), as lstat sees it. SFTP has
	// no dev/ino/ctime (0) and whole-second times.
	char   type = 'f';        // 'f', 'd', 'l', or 's' for anything special
	int64  size = 0, mtime_ns = 0, ctime_ns = 0, dev = 0, ino = 0, nlink = 1;
	int    mode = 0;
	int64  uid = -1, gid = -1;
	String link;              // symlink target
};

int                   OpenDir(const String& dir);   // O_RDONLY dir fd, any path length; -1 + errno

// The checked files and folders of a tree (the GUI's checkboxes, --include-path), as
// paths relative to its root, "" = all of it. A checked folder means everything below
// it, so the set stays minimal: checking replaces what is checked below; unchecking
// something checked through a folder above replaces that folder by everything else in
// it, down to the unchecked one (children(folder): the names in a folder).
class CheckedPaths {
	Index<String> paths;

public:
	enum { NO, YES, PART };                      // PART: something below is checked
	void   Set(const String& include_paths);     // "" = everything
	String Get() const;                          // sorted, one per line; "" = everything
	bool   Nothing() const                       { return paths.IsEmpty(); }
	int    State(const String& rel) const;
	void   Toggle(const String& rel, Function<Vector<String>(const String&)> children);
};
std::filesystem::path ToFs(const String& p);
String                FromFs(const std::filesystem::path& p);
bool                  ListFolder(const String& dir, Vector<FolderEntry>& out, String& error);
String                CleanPath(String p);

// Progress.cpp: one dnf5-style line per copied file, plus a "Total" line.
struct CopyProgress {
	int    index = 0;      // this file, 1-based
	int    count = 0;      // files in the run (while streaming: decided plus still to check)
	String name;           // path relative to the tree root, or "Total"
	int64  done = 0;       // bytes copied so far
	int64  size = 0;       // bytes in total
	double seconds = 0;    // elapsed
	int    id = 0;         // which copy (its step in the run), to tell parallel copies apart
	bool   verifying = false;   // done counts bytes read back and checked, not written
	bool   more = false;   // count still growing: the pre-scan is still finding files
	bool   check = false;  // Scan only: the file is being hashed on both sides, nothing copied
	String state;          // check, when done: same, DIFFERS, missing, failed
};

// SHA-256 through OpenSSL (libcrypto, already linked for SSH), which uses the CPU's
// SHA instructions: about 5 times U++'s own on a CPU that has them. Lowercase hex,
// the same as sha256sum and as the hashes already stored.
class Sha256 {
	void *ctx;   // EVP_MD_CTX

public:
	void   Put(const void *data, int64 len);
	void   Put(const String& s)          { Put(~s, s.GetCount()); }
	String FinishString();               // once

	Sha256();
	Sha256(const Sha256&) = delete;
	~Sha256();
};

String Sha256Hex(const String& data);   // of a short string (symlink targets, listings)

// bar: a text thermometer after the percentage, for a file still in progress.
String ProgressLine(const CopyProgress& p, int width = 100, bool bar = false);

// How long a run took, like getDuration() in Speed_Tests: always positive, years by
// the average Gregorian year. DurationClock: "YYY:DDD:HH:MM:SS"; DurationWords:
// " 2 hours 05 minutes 09 seconds", fixed width so lines line up (days and years
// in front when there are any);
// ClockTime: local "HH:MM:SS".
String DurationClock(int64 seconds);
String DurationWords(int64 seconds);
String ClockTime(int64 unix_time);
String DateTimeText(int64 unix_time);   // "2026-10-17 14:13:09  UTC = -7"
// The three time columns of a results line: " 14:02:58 : 14:03:01 : 000:000:00:00:03 : ".
String LogStamp(int64 start, int64 stop);
// The same columns for a run's log lines, to the microsecond, every field padded so
// the lines stay aligned: " 14:02:58.000412 : 14:02:58.006931 : 000:000:00:00:00.006519 : ".
// Times are in microseconds since the epoch (NowMicros).
String LogStampFine(int64 start_us, int64 stop_us);
// The top of a run's log: what it is, when, where and who, then column titles lined
// up with LogStampFine.
String LogHeader(int64 unix_time);
int64  NowMicros();

// The run's start, stop and duration as a table in the layout of the j5c build
// Results.txt: a header ("Status started on ...", the column titles) and one line
// per run, " 14:02:58 : 16:08:07 : 000:000:02:05:09 : Mirror run 6.... : Result =
// ....0 : Good.... : comment".
String ResultsHeader(int64 unix_time);
String ResultsLine(int64 start, int64 stop, const String& what, int result, const String& status,
                   const String& comment);

// Toolchain.cpp: compiler, library and C++26 feature report.
String ToolchainReport();
String ToolchainFacts();   // "key<TAB>value" per line, for comparing compilers

// Bench.cpp: how fast this build does TreeSync's own work, "key<TAB>value" per line:
// SHA-256 throughput, progress-line formatting, path sorting. Best of 3 rounds.
String BenchFacts();

// AiExport.cpp: a run as Markdown for analysis by an AI (or a person), organized but
// complete. logs: every log of the run (one per start) with a guide, the summary,
// problems, the slowest copies and throughput per minute, then each log in full. sql:
// the schema, all runs, and this run's actions, chunks, hash cache and synced files.
// Written to <logs>/ai/run-N-logs.AI.md or run-N-sql.AI.md; run 0 = the newest.
bool ExportAiLogs(const String& db_path, int64 run, String& out_path, String& error);
bool ExportAiSql(const String& db_path, int64 run, String& out_path, String& error);
Vector<String> RunLogs(const String& db_path, int64 run);   // in order

// Root.cpp. The messages are plain text, one paragraph per line; the GUI shows
// them in a dialog and also prints them to the terminal.
bool   IsRoot();
// An administrator: root, or a member of the group that may use sudo and polkit's
// admin authentication (wheel on RHEL and Fedora; sudo or admin on Debian, Ubuntu,
// Mint). The installed TreeSync syncs as root only for them; anyone else runs it as
// themselves, inside their own home.
bool   IsAdmin();
bool   IsAdminGroup(const String& group);
String HomeDir();
bool   PathAllowed(const String& p);   // always true as root; else only inside HomeDir()
String CanonicalPath(const String& p);  // symlinks and ".." resolved
String NotRootMessage();
String RefusedPathMessage(const String& p);

#include "Engine.h"

#endif
