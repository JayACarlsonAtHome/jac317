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
	bool   use_key = true;
	String key_file;
	String password;   // held in memory only, never written to the SQLite cache
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
	bool   fat_file_times = false;   // 2-second time tolerance
	bool   dst_tolerance = false;    // allow exact 1-hour shifts
	bool   follow_links = false;     // write out what symlinks point to
	bool   hard_links = true;        // relink hard-linked files instead of writing each out
	bool   compress = false;         // zlib on SSH connections
	bool   preserve_owner = true;    // copy owner and group, by name (where we may)
	bool   numeric_ids = false;      // ...by number instead
	bool   safe_delete = true;       // move deleted and replaced files to <target>.old
	bool   include_same = false;
	bool   skip_older = false;
	bool   skip_newer = false;
	bool   skip_changed = false;
	bool   keep_extra = false;
	bool   skip_source_only = false;
	String exclude_files;            // globs, ';'-separated
	String exclude_dirs;             // globs, ';'-separated
	int    retries = 3;
	int    retry_wait = 5;           // seconds
	int    threads = 8;
	int    big_file_mib = 1024;      // files this big or bigger are copied in chunks
	int    chunk_mib = 64;           // chunk size for big files
};

struct BoolOption {
	const char *name;                // --name / --no-name
	bool SyncFlags::*field;
	const char *robocopy;
	const char *text;
};

struct IntOption {
	const char *name;                // --name=N
	int SyncFlags::*field;
	const char *robocopy;
	const char *text;
	int min, max;
};

constexpr int BOOL_OPTION_COUNT = 15;
constexpr int INT_OPTION_COUNT = 5;
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
};

int                   OpenDir(const String& dir);   // O_RDONLY dir fd, any path length; -1 + errno
std::filesystem::path ToFs(const String& p);
String                FromFs(const std::filesystem::path& p);
bool                  ListFolder(const String& dir, Vector<FolderEntry>& out, String& error);
String                CleanPath(String p);

// Progress.cpp: one dnf5-style line per copied file, plus a "Total" line.
struct CopyProgress {
	int    index = 0;      // this file, 1-based
	int    count = 0;      // files in the run
	String name;           // path relative to the tree root, or "Total"
	int64  done = 0;       // bytes copied so far
	int64  size = 0;       // bytes in total
	double seconds = 0;    // elapsed
};

String ProgressLine(const CopyProgress& p, int width = 100);

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
