// TreeSync engine: scan, plan and copy, with every step recorded in SQLite so a
// stopped run resumes where it left off. Design: v001/docs/engine.md.
#ifndef TREESYNC_ENGINE_H
#define TREESYNC_ENGINE_H

#include <atomic>
#include <chrono>
#include <mutex>

struct sqlite3;
struct sqlite3_stmt;

// Db.cpp ---------------------------------------------------------------------

class Db {
	sqlite3 *db = nullptr;

public:
	bool    Open(const String& path, String& error);
	void    Close();
	bool    Exec(const char *sql);
	String  Error() const;
	int64   LastId() const;
	sqlite3 *Handle() const    { return db; }

	Db() = default;
	Db(const Db&) = delete;
	~Db()                      { Close(); }
};

// RAII prepared statement. Columns and parameters are 0- and 1-based, as in SQLite.
class Stmt {
	sqlite3_stmt *st = nullptr;

public:
	Stmt&  Int(int i, int64 v);
	Stmt&  Text(int i, const String& v);
	bool   Step();               // true while a row is available
	bool   Run();                // execute; true on success
	void   Reset();
	int64  Int(int col) const;
	String Text(int col) const;
	int    Columns() const;      // of the result
	String Name(int col) const;  // column name

	Stmt(Db& db, const char *sql);
	Stmt(const Stmt&) = delete;
	~Stmt();
};

String DefaultDbPath();         // $XDG_DATA_HOME/treesync/treesync.db
String LogFolder(const String& db_path);   // "logs" next to the database
void   SetRunLog(const String& db_path, int64 run, const String& log);

// Scan.cpp -------------------------------------------------------------------

// One file, folder or symlink in a tree. rel is relative to the tree root.
struct Entry : Moveable<Entry> {
	String rel;
	char   type = 'f';      // 'f' file, 'd' folder, 'l' symlink
	int64  dev = 0, ino = 0, nlink = 1, size = 0, mtime_ns = 0, ctime_ns = 0;
	int    mode = 0;
	int64  uid = -1, gid = -1;   // filled by TreeFs::Stat, not stored
	String link;            // symlink target
	String hash;            // SHA-256 hex; folders: hash of their contents
};

String SortKey(const String& rel);   // keeps each folder's subtree contiguous

struct ScanStats {
	int   entries = 0;
	int   hashed = 0;         // files read and hashed
	int   reused = 0;         // files whose cached hash was still valid
	int64 hashed_bytes = 0;
	int64 estimate = 0;       // from statvfs inode count, 0 = unknown
	int   warnings = 0;
};

// Engine.cpp/Sync.cpp --------------------------------------------------------

// --bwlimit: a token bucket with at most one second of burst. Waits in short
// steps so Stop still works.
class Throttle {
	int64  rate = 0;          // bytes per second, 0 = no limit
	double tokens = 0;
	std::chrono::steady_clock::time_point last;
	std::mutex m;             // shared by the copy threads
	const std::atomic<bool> *stop = nullptr;

public:
	void Account(int64 bytes);
	bool Limited() const      { return rate > 0; }

	Throttle(int64 bytes_per_second, const std::atomic<bool> *stop);
};

// Where a streamed run is: 0 pre-scan (entries found, of an estimate if known),
// 2 post-scan (folders checked, of folders found). Copying is WhenOverall.
struct StageProgress {
	int    stage = 0;
	int64  done = 0, total = 0;   // total 0: not known
	bool   finished = false;
	String detail;               // the folder being worked on
};

// What the engine tells the terminal or the GUI as it works.
struct SyncReport {
	Event<const String&>       WhenLine;      // a finished line: keep it
	Event<const String&>       WhenStatus;    // a passing status: may overwrite
	Event<const CopyProgress&> WhenProgress;  // copy progress; done == size when a file finishes
	Event<int64, int64>        WhenOverall;   // bytes of the run's copies done, of total
	Event<const StageProgress&> WhenStage;    // pre-scan and post-scan of a streamed run
	Event<bool, const String&> WhenNetwork;   // true: lost the connection to host, waiting; false: back
	const std::atomic<bool>   *stop = nullptr;
	std::atomic<bool>          halted = false;   // the engine stopped the run itself (network gone too long)
	Throttle                  *throttle = nullptr;   // set by RunSync for --bwlimit

	bool Stopping() const      { return halted || (stop && *stop); }
	void Count(int64 bytes)    { if(throttle) throttle->Account(bytes); }
};

bool ScanTree(Db& db, const String& root, const SyncFlags& flags, SyncReport& report,
              Vector<Entry>& out, String& root_hash, ScanStats& stats, String& error);

// The files table as a hash cache for one tree (root = TreeFs::Id()). Used by the
// local and the SFTP scanner, so both treat cached hashes the same way.
class HashCache {
	Db&              db;
	String           root;
	const SyncFlags& flags;
	bool             unstable;   // inode numbers not usable: match on size and time only
	Stmt             find, store;
	int              pending = 0;

public:
	int64 stamp;

	bool Lookup(const Entry& e, String& hash);
	void Store(const Entry& e);
	void Finish();               // forget entries not seen in this scan, commit

	HashCache(Db& db, const String& root, const SyncFlags& flags, bool unstable);
};

// One line of a folder's listing; the folder hash is SHA-256 of all its lines,
// built the same way locally and over SFTP so equal trees hash equal.
inline void ListingLine(String& listing, char type, const String& hash, const String& name)
{
	listing << type << ' ' << hash << ' ' << name << '\n';
}

bool SkipName(const String& name);   // ".", "..", our own .treesync-part files
bool UnstableInodes(int fd);         // FAT, FUSE, network filesystems
bool GlobMatch(const String& patterns, const String& name);   // name matches one of the ';' globs
bool AnyPattern(const String& patterns);                      // not blank: something to match

// The name filters (--exclude-file/-dir, --include-file/-dir) and the checked paths
// (--include-path). Blank lists filter nothing, and exclude wins. With include names,
// a file (or symlink) is synced only inside an included folder (anywhere when
// --include-dir is blank) and, with --include-file, only if its name matches. With
// checked paths, only what is at or below a checked path is synced; the folders above
// a checked path are walked to reach it. Folders are made only where something
// included goes, except an included or checked folder itself. The same rule decides
// what Mirror may delete. Every call takes the path relative to the tree root.
struct NameFilter {
	const SyncFlags& f;
	bool             include;      // an include list (names or paths) is given
	bool             names;        // include names are given
	Index<String>    paths;        // the checked paths
	Index<String>    on_the_way;   // the folders above them

	explicit NameFilter(const SyncFlags& f);

	static String Name(const String& rel)              { return rel.Mid(rel.ReverseFind('/') + 1); }
	bool Checked(const String& rel) const;             // rel or a folder above it is checked (no paths: true)
	bool RootInside() const                            { return !AnyPattern(f.include_dirs); }
	bool SkipDir(const String& rel) const
	{
		return GlobMatch(f.exclude_dirs, Name(rel)) || (paths.GetCount() && on_the_way.Find(rel) < 0 && !Checked(rel));
	}
	bool Inside(bool parent, const String& rel) const  { return parent || GlobMatch(f.include_dirs, Name(rel)); }
	bool KeepEmptyDir(bool inside, const String& rel) const
	{
		return !include || ((!names || (inside && AnyPattern(f.include_dirs))) && Checked(rel));
	}
	bool KeepFile(bool inside, const String& rel) const
	{
		String name = Name(rel);
		return inside && !GlobMatch(f.exclude_files, name) &&
		       (!AnyPattern(f.include_files) || GlobMatch(f.include_files, name)) && Checked(rel);
	}
};

// Plan.cpp -------------------------------------------------------------------

enum class ActionKind { Mkdir, Copy, Link, Delete, Conflict, Attrs };   // Attrs: same content, set attributes only

struct Action : Moveable<Action> {
	ActionKind kind = ActionKind::Copy;
	int    dir = 0;           // 0: source -> target, 1: target -> source (merge)
	Entry  from;              // the entry being copied, created or deleted
	String link_to;           // Link: rel of the first copy of this hard-linked file
	String note;              // Conflict: why
	bool   replaces = false;  // Copy/Link: overwrites something already at the destination
};

const char *KindName(ActionKind k);

// Mirror/Copy need only the scans; Merge also uses the hashes both sides had after
// the last sync (rel -> hash) to tell which side changed.
// The time switches (--skip-older, --skip-newer, --skip-changed, with --time-tolerance
// and --dst-tolerance): true when s (source) should not replace d (target).
bool TimeSkip(const SyncFlags& flags, const Entry& s, const Entry& d);

Vector<Action> PlanSync(const Vector<Entry>& src, const Vector<Entry>& dst, const SyncFlags& flags,
                        const VectorMap<String, String>& last_synced);

// Copy.cpp -------------------------------------------------------------------

bool   HashAt(int dirfd, const String& name, String& hash, String& error,
              const std::atomic<bool> *stop = nullptr, int64 *bytes = nullptr, bool follow = false);

// Ids.cpp: users and groups, matched by name ------------------------------------

struct IdTable {
	VectorMap<String, int64> uid, gid;            // name -> number
	VectorMap<int64, String> user_of, group_of;   // number -> name

	void Add(bool group, const String& name, int64 id);
	void Parse(bool group, const String& text);   // /etc/passwd or /etc/group lines
};

bool   LoadLocalIds(IdTable& t, String& error);
// The number on the other side for the same name; the same number when there is
// no name for it, the name is unknown there (*unknown is set), or numeric is true.
int64  MapId(const IdTable& from, const IdTable& to, bool group, int64 id, bool numeric, String *unknown);
String CompareIds(const IdTable& a, const IdTable& b, const String& name_a, const String& name_b);

// TreeFs.cpp: one side of a sync, local or over SFTP ----------------------------

struct FileAttrs {
	int   mode = 0644;
	int64 mtime_ns = 0;
	int64 uid = -1, gid = -1;   // -1: leave the owner alone
};

// An open file for chunked copying.
struct TreeFile {
	virtual bool ReadAt(int64 off, void *buf, int64 len, String& error) = 0;
	virtual bool WriteAt(int64 off, const void *buf, int64 len, String& error) = 0;
	virtual bool Truncate(int64 size, String& error) = 0;
	virtual void Flush() = 0;
	virtual bool SetAttrs(const FileAttrs& a, String& error) = 0;
	virtual int  Fd() const   { return -1; }   // local files: the descriptor
	virtual ~TreeFile() {}
};

// Extended attributes (ACLs, SELinux label, capabilities, user.*) from one local file
// to another, best effort: what cannot be set is left out, as cp --preserve=all does.
// Returns the source's XattrDigest, worked out from the same read.
String CopyXattrs(TreeFile& from, TreeFile& to);

// A local open file's Entry (type, dev, ino, size, times, mode, owner) from fstat.
bool   FdEntry(int fd, Entry& e);

// All bytes zero: a slice the sparse copy leaves as a hole.
inline bool AllZero(const void *p, int64 n)
{
	const byte *b = (const byte *)p;
	return n <= 0 || (b[0] == 0 && memcmp(b, b + 1, (size_t)n - 1) == 0);
}

// Paths are relative to the tree root ("" is the root itself).
class TreeFs {
public:
	virtual String Id() const = 0;        // key in the database: path, or sftp://user@host:port/path
	virtual bool   IsLocal() const        { return false; }
	virtual bool   CanHardLink() const    { return false; }
	virtual bool   RootExists() = 0;
	virtual bool   MakeRoot(String& error) = 0;
	virtual bool   Scan(Db& db, const SyncFlags& flags, SyncReport& report, Vector<Entry>& out,
	                    String& root_hash, ScanStats& stats, String& error) = 0;
	virtual bool   Stat(const String& rel, bool follow, Entry& e, String& error) = 0;
	virtual bool   Hash(const String& rel, bool follow, String& hash, String& error,
	                    const std::atomic<bool> *stop) = 0;
	virtual One<TreeFile> Open(const String& rel, bool write, String& error) = 0;   // write: create, keep content
	virtual bool   SetAttrs(const String& rel, const FileAttrs& a, String& error) = 0;
	virtual bool   Mkdir(const String& rel, int mode, String& error) = 0;           // fine if it exists
	virtual int    Remove(const String& rel, bool dir, String& error) = 0;          // 0 ok/gone, 1 not empty, -1 error
	virtual bool   Symlink(const String& rel, const String& target, String& error) = 0;
	virtual bool   Rename(const String& from, const String& to, String& error) = 0; // replaces to
	virtual bool   HardLink(const String& existing, const String& rel, String& error) = 0;
	virtual bool   LoadIds(IdTable& t, String& error) = 0;
	// One folder's entries, for browsing (no hashing). is_dir follows symlinks.
	virtual bool   List(const String& rel, Vector<FolderEntry>& out, String& error) = 0;
	// --safe-delete: moves rel to dest (an absolute path, same filesystem), creating
	// its folders; if dest is taken, dest.1, dest.2, ... Gone already counts as done.
	virtual bool   MoveOut(const String& rel, const String& dest, String& moved_to, String& error) = 0;
	virtual String RootPath() const = 0;     // absolute root path on its own machine
	virtual bool   CanChown() const = 0;     // root here, or a root login there
	virtual bool   Chown(const String& rel, int64 uid, int64 gid, String& error) = 0;   // never follows symlinks
	virtual String Name() const = 0;         // for reports: "local" or user@host
	virtual bool   StableIds()               { return false; }   // inode numbers usable by the hash cache
	virtual int64  EntryEstimate()           { return 0; }       // likely entry count (statvfs), 0 = unknown
	// A remote side whose connection broke (network down, server gone): every call
	// fails until Reconnect succeeds. Reconnect only with no files open.
	virtual bool   Lost()                    { return false; }
	virtual bool   Reconnect(String& error)  { error = "not a remote side"; return false; }
	virtual String Host() const              { return String(); }   // remote: the machine, for messages
	virtual ~TreeFs() {}
};

class LocalFs : public TreeFs {
	String root;
	int    stable = -1;   // StableIds, worked out once

public:
	int    ParentFd(const String& rel) const;   // the folder holding rel, opened
	String Id() const override           { return root; }
	bool   IsLocal() const override      { return true; }
	bool   StableIds() override;
	int64  EntryEstimate() override;
	bool   CanHardLink() const override  { return true; }
	bool   RootExists() override;
	bool   MakeRoot(String& error) override;
	bool   Scan(Db& db, const SyncFlags& flags, SyncReport& report, Vector<Entry>& out,
	            String& root_hash, ScanStats& stats, String& error) override;
	bool   Stat(const String& rel, bool follow, Entry& e, String& error) override;
	bool   Hash(const String& rel, bool follow, String& hash, String& error,
	            const std::atomic<bool> *stop) override;
	One<TreeFile> Open(const String& rel, bool write, String& error) override;
	bool   SetAttrs(const String& rel, const FileAttrs& a, String& error) override;
	bool   Mkdir(const String& rel, int mode, String& error) override;
	int    Remove(const String& rel, bool dir, String& error) override;
	bool   Symlink(const String& rel, const String& target, String& error) override;
	bool   Rename(const String& from, const String& to, String& error) override;
	bool   HardLink(const String& existing, const String& rel, String& error) override;
	bool   LoadIds(IdTable& t, String& error) override   { return LoadLocalIds(t, error); }
	bool   List(const String& rel, Vector<FolderEntry>& out, String& error) override
	                                                     { return ListFolder(rel.IsEmpty() ? root : AppendFileName(root, rel), out, error); }
	bool   MoveOut(const String& rel, const String& dest, String& moved_to, String& error) override;
	String RootPath() const override                     { return root; }
	bool   CanChown() const override                     { return IsRoot(); }
	bool   Chown(const String& rel, int64 uid, int64 gid, String& error) override;
	String Name() const override                         { return "local"; }

	explicit LocalFs(const String& root) : root(root) {}
};

// Over SFTP (libssh2). The host key must already be in known_hosts. SFTP has no
// inode numbers and only whole-second times, so the hash cache matches on size and
// time, and hard links are written out as copies.
class SftpFs : public TreeFs {
public:
	struct Impl;      // defined in SftpFs.cpp only

private:
	One<Impl> impl;

public:
	bool   Connect(String& error);
	String Id() const override;
	bool   RootExists() override;
	bool   MakeRoot(String& error) override;
	bool   Scan(Db& db, const SyncFlags& flags, SyncReport& report, Vector<Entry>& out,
	            String& root_hash, ScanStats& stats, String& error) override;
	bool   Stat(const String& rel, bool follow, Entry& e, String& error) override;
	bool   Hash(const String& rel, bool follow, String& hash, String& error,
	            const std::atomic<bool> *stop) override;
	One<TreeFile> Open(const String& rel, bool write, String& error) override;
	bool   SetAttrs(const String& rel, const FileAttrs& a, String& error) override;
	bool   Mkdir(const String& rel, int mode, String& error) override;
	int    Remove(const String& rel, bool dir, String& error) override;
	bool   Symlink(const String& rel, const String& target, String& error) override;
	bool   Rename(const String& from, const String& to, String& error) override;
	bool   HardLink(const String& existing, const String& rel, String& error) override;
	bool   LoadIds(IdTable& t, String& error) override;
	bool   List(const String& rel, Vector<FolderEntry>& out, String& error) override;
	bool   MoveOut(const String& rel, const String& dest, String& moved_to, String& error) override;
	String RootPath() const override;
	bool   CanChown() const override;
	bool   Chown(const String& rel, int64 uid, int64 gid, String& error) override;
	String Name() const override;
	bool   Lost() override;
	bool   Reconnect(String& error) override;
	String Host() const override;

	SftpFs(const TreeEndpoint& e, const String& known_hosts, bool compress);
	~SftpFs();
};

// Check.cpp: one row of file_checks, per file and run -------------------------
// Copies record what they checked; Scan only and Sync attributes record every file.
// The folder is stored once, in folders, and referred to by its id.

class FolderIds {
	Db&                      db;
	VectorMap<String, int64> ids;   // rel -> folders.id

public:
	int64 Get(const String& rel);
	FolderIds(Db& db) : db(db) {}
};

struct CheckRow {
	int    seq = 0;
	String rel;
	String kind;                 // copy, scan, attrs
	char   type = 'f';           // 'f' file, 'd' folder
	bool   side[2] = {};         // [0] source, [1] target: present
	String sha[2];               // empty: not hashed
	int64  size[2] = {}, uid[2] = { -1, -1 }, gid[2] = { -1, -1 }, mtime_ns[2] = {};
	int    mode[2] = {};
	String xattrs[2];            // XattrDigest ("" = none), when xattrs_known
	bool   xattrs_known[2] = {}; // false: not read (remote, or a sync's attribute check)
	String attr_diff;            // "" when the attributes agree
	String result;               // fixed, failed: ..., or empty
};

// Writes rows of file_checks, its statement prepared once.
class CheckWriter {
	FolderIds folders;
	Stmt      insert;

public:
	void Write(int64 run, const CheckRow& row);
	CheckWriter(Db& db);
};
// The SHA-256 of a local file's extended attributes (sorted, names and values), "" when
// it has none. The SELinux label is left out: it is set by policy where the file is.
String XattrDigest(TreeFile& f);
// What differs between the two sides' attributes, "" when nothing: mode, the time
// (files; exact when both sides are local, else whole seconds), owner and group
// when want_uid >= 0 (the source's, as the target's numbers), extended attributes
// when both are known.
String AttrDiff(const CheckRow& row, int64 want_uid, int64 want_gid, bool exact_time);

// Scan only (--scan-only): every file read and hashed afresh on both sides, the
// attributes compared; nothing is changed. Sync attributes (--sync-attrs): the
// attributes only (no hashing), then only those that differ are set on the target,
// for files of the same size and for folders (mode and owner).
enum class CheckMode { Scan, Attrs };

struct CheckResult {
	int64 run = 0;
	bool  stopped = false;
	int   files = 0, folders = 0, same = 0, differ = 0, missing = 0, attr_diffs = 0, fixed = 0, failed = 0;
	int64 bytes = 0;            // read and hashed
};

bool RunCheck(const SyncSetup& setup, CheckMode mode, SyncReport& report, CheckResult& result, String& error);

// --clear-old: deletes the --safe-delete folders (<source>.old, <target>.old or
// --safe-delete-dir) for good. dry: only counts.
bool ClearOld(const SyncSetup& setup, bool dry, SyncReport& report, String& error);

// Sync.cpp -------------------------------------------------------------------

struct SyncResult {
	int64  run = 0;
	bool   resumed = false;
	bool   stopped = false;
	int    planned = 0, done = 0, failed = 0, conflicts = 0;
	int64  bytes = 0;
	ScanStats source, target;
	Vector<String> plan;      // dry run: one line per action
};

// Runs (or resumes) a sync.
bool RunSync(const SyncSetup& setup, SyncReport& report, SyncResult& result, String& error);

// --compare-ids: the user and group report for the two sides (one must be remote).
bool CompareSides(const SyncSetup& setup, SyncReport& report, String& out, String& error);

#endif
