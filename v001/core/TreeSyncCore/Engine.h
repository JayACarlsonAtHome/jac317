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

// Plan.cpp -------------------------------------------------------------------

enum class ActionKind { Mkdir, Copy, Link, Delete, Conflict };

struct Action : Moveable<Action> {
	ActionKind kind = ActionKind::Copy;
	int    dir = 0;           // 0: source -> target, 1: target -> source (merge)
	Entry  from;              // the entry being copied, created or deleted
	String link_to;           // Link: rel of the first copy of this hard-linked file
	String note;              // Conflict: why
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

// Runs (or resumes) a sync. Local trees only for now.
bool RunSync(const SyncSetup& setup, SyncReport& report, SyncResult& result, String& error);

#endif
