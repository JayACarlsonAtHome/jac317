// TreeSync engine: scan, plan and copy, with every step recorded in SQLite so a
// stopped run resumes where it left off. Design: v001/docs/engine.md.
#ifndef TREESYNC_ENGINE_H
#define TREESYNC_ENGINE_H

#include <atomic>

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

	Stmt(Db& db, const char *sql);
	Stmt(const Stmt&) = delete;
	~Stmt();
};

String DefaultDbPath();         // $XDG_DATA_HOME/treesync/treesync.db

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

// What the engine tells the terminal or the GUI as it works.
struct SyncReport {
	Event<const String&>       WhenLine;      // a finished line: keep it
	Event<const String&>       WhenStatus;    // a passing status: may overwrite
	Event<const CopyProgress&> WhenProgress;  // copy progress; done == size when a file finishes
	const std::atomic<bool>   *stop = nullptr;

	bool Stopping() const      { return stop && *stop; }
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
bool Excluded(const String& patterns, const String& name);

// Plan.cpp -------------------------------------------------------------------

enum class ActionKind { Mkdir, Copy, Link, Delete, Conflict };

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
Vector<Action> PlanSync(const Vector<Entry>& src, const Vector<Entry>& dst, const SyncFlags& flags,
                        const VectorMap<String, String>& last_synced);

// Copy.cpp -------------------------------------------------------------------

bool   HashAt(int dirfd, const String& name, String& hash, String& error,
              const std::atomic<bool> *stop = nullptr, int64 *bytes = nullptr, bool follow = false);
bool   CopySmall(int src_dir, const String& name, int dst_dir, const String& part, String& error);

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
	bool         Hash(int64 size, String& hash, String& error, const std::atomic<bool> *stop);
	virtual ~TreeFile() {}
};

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
	// --safe-delete: moves rel to dest (an absolute path, same filesystem), creating
	// its folders; if dest is taken, dest.1, dest.2, ... Gone already counts as done.
	virtual bool   MoveOut(const String& rel, const String& dest, String& moved_to, String& error) = 0;
	virtual String RootPath() const = 0;     // absolute root path on its own machine
	virtual bool   CanChown() const = 0;     // root here, or a root login there
	virtual bool   Chown(const String& rel, int64 uid, int64 gid, String& error) = 0;   // never follows symlinks
	virtual String Name() const = 0;         // for reports: "local" or user@host
	virtual ~TreeFs() {}
};

class LocalFs : public TreeFs {
	String root;

public:
	int    ParentFd(const String& rel) const;   // for the cp fast path
	String Id() const override           { return root; }
	bool   IsLocal() const override      { return true; }
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
	bool   MoveOut(const String& rel, const String& dest, String& moved_to, String& error) override;
	String RootPath() const override;
	bool   CanChown() const override;
	bool   Chown(const String& rel, int64 uid, int64 gid, String& error) override;
	String Name() const override;

	SftpFs(const TreeEndpoint& e, const String& known_hosts, bool compress);
	~SftpFs();
};

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
