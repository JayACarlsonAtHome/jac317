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
	bool   include_same = false;
	bool   skip_older = false;
	bool   skip_newer = false;
	bool   skip_changed = false;
	bool   keep_extra = false;
	bool   skip_source_only = false;
	bool   wipe_target = false;      // empty the target first (to .old with safe delete)
	String exclude_files;            // globs, ';'-separated
	String exclude_dirs;             // globs, ';'-separated
	int    retries = 3;
	int    retry_wait = 5;           // seconds
	int    threads = 4;          // copy threads; remote sides get one SSH connection each
	int    big_file_mib = 1024;      // files this big or bigger are copied in chunks
	int    chunk_mib = 64;           // chunk size for big files
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
extern const char *HELP_OWNERS;           // the Owners choice: off, by name, by number

constexpr int BOOL_OPTION_COUNT = 16;
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
};

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

// The run's start, stop and duration as a table in the layout of the j5c build
// Results.txt: a header ("Status started on ...", the column titles) and one line
// per run, " 14:02:58 : 16:08:07 : 000:000:02:05:09 : Mirror run 6.... : Result =
// ....0 : Good.... : comment".
String ResultsHeader(int64 unix_time);
String ResultsLine(int64 start, int64 stop, const String& what, int result, const String& status,
                   const String& comment);

// Toolchain.cpp: compiler, library and C++26 feature report.
String ToolchainReport();

// Root.cpp. The messages are plain text, one paragraph per line; the GUI shows
// them in a dialog and also prints them to the terminal.
bool   IsRoot();
String HomeDir();
bool   PathAllowed(const String& p);   // always true as root; else only inside HomeDir()
String CanonicalPath(const String& p);  // symlinks and ".." resolved
String NotRootMessage();
String RefusedPathMessage(const String& p);

#include "Engine.h"

#endif
