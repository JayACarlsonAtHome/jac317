// SQLite access: the system SQLite plugin/sqlite3 links (U++ no longer bundles one), raw C
// API, WAL mode.
#include "TreeSyncCore.h"

#include <sqlite3.h>

static const char *SCHEMA = R"(
PRAGMA journal_mode = WAL;
PRAGMA synchronous = NORMAL;
CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT);
INSERT OR IGNORE INTO meta VALUES ('schema', '4');
CREATE TABLE IF NOT EXISTS files (
	root TEXT, rel TEXT, type TEXT,
	dev INTEGER, ino INTEGER, nlink INTEGER, size INTEGER,
	mtime_ns INTEGER, ctime_ns INTEGER, mode INTEGER, link TEXT, sha256 TEXT,
	seen INTEGER,
	PRIMARY KEY (root, rel));
CREATE TABLE IF NOT EXISTS runs (
	id INTEGER PRIMARY KEY, started INTEGER, finished INTEGER,
	source TEXT, target TEXT, args TEXT, state TEXT, log TEXT);
CREATE TABLE IF NOT EXISTS actions (
	run INTEGER, seq INTEGER, kind TEXT, dir INTEGER, rel TEXT, type TEXT, link TEXT, link_to TEXT,
	size INTEGER, sha256 TEXT, dev INTEGER, ino INTEGER, mtime_ns INTEGER, ctime_ns INTEGER,
	mode INTEGER, note TEXT, state TEXT, bytes INTEGER, error TEXT, replaces INTEGER DEFAULT 0,
	PRIMARY KEY (run, seq));
CREATE INDEX IF NOT EXISTS actions_rel ON actions (run, rel);
CREATE INDEX IF NOT EXISTS actions_state ON actions (run, state);
CREATE TABLE IF NOT EXISTS chunks (
	run INTEGER, seq INTEGER, idx INTEGER, offset INTEGER, length INTEGER, sha256 TEXT,
	PRIMARY KEY (run, seq, idx));
CREATE TABLE IF NOT EXISTS run_logs (
	run INTEGER, log TEXT,
	PRIMARY KEY (run, log));
CREATE TABLE IF NOT EXISTS folders (
	id INTEGER PRIMARY KEY, rel TEXT UNIQUE);
CREATE TABLE IF NOT EXISTS file_checks (
	run INTEGER, seq INTEGER, folder INTEGER, name TEXT, kind TEXT, type TEXT,
	src_sha256 TEXT, dst_sha256 TEXT, src_size INTEGER, dst_size INTEGER,
	src_mode INTEGER, dst_mode INTEGER, src_uid INTEGER, dst_uid INTEGER, src_gid INTEGER, dst_gid INTEGER,
	src_mtime_ns INTEGER, dst_mtime_ns INTEGER, src_xattrs TEXT, dst_xattrs TEXT,
	attr_diff TEXT, result TEXT,
	PRIMARY KEY (run, seq));
CREATE TABLE IF NOT EXISTS synced (
	source TEXT, target TEXT, rel TEXT, sha256 TEXT,
	PRIMARY KEY (source, target, rel));
)";

static const char *VIEWS = R"(
DROP VIEW IF EXISTS run_summary;
CREATE VIEW run_summary AS
SELECT r.id AS run,
       datetime(r.started, 'unixepoch', 'localtime') AS started,
       datetime(r.finished, 'unixepoch', 'localtime') AS finished,
       r.state, r.source, r.target,
       count(CASE WHEN a.kind = 'mkdir' AND a.state = 'done' THEN 1 END) AS folders,
       count(CASE WHEN a.kind = 'copy' AND a.state = 'done' THEN 1 END) AS copied,
       count(CASE WHEN a.kind = 'link' AND a.state = 'done' THEN 1 END) AS linked,
       count(CASE WHEN a.kind = 'delete' AND a.state = 'done' THEN 1 END) AS deleted,
       count(CASE WHEN a.kind = 'attrs' AND a.state = 'done' THEN 1 END) AS attrs_fixed,
       count(CASE WHEN a.state = 'failed' THEN 1 END) AS failed,
       count(CASE WHEN a.kind = 'conflict' THEN 1 END) AS conflicts,
       coalesce(sum(CASE WHEN a.kind = 'copy' AND a.state = 'done' THEN a.size END), 0) AS bytes,
       r.args, r.log
FROM runs r LEFT JOIN actions a ON a.run = r.id
GROUP BY r.id;
DROP VIEW IF EXISTS failures;
CREATE VIEW failures AS
SELECT run, seq, kind, rel, error FROM actions WHERE state = 'failed';
DROP VIEW IF EXISTS run_actions;
CREATE VIEW run_actions AS
SELECT run, seq, kind, rel, state, size, replaces, error FROM actions;
DROP VIEW IF EXISTS file_hashes;
DROP VIEW IF EXISTS file_compare;
CREATE VIEW file_compare AS
SELECT c.run, c.seq, c.kind, c.type, c.folder AS folder_id, f.rel AS path, c.name AS file_name,
       c.src_sha256, c.dst_sha256,
       CASE WHEN c.type <> 'f' OR c.src_sha256 IS NULL THEN NULL
            WHEN c.src_sha256 = c.dst_sha256 THEN 'true' ELSE 'false' END AS match,
       CASE WHEN c.dst_mode IS NULL THEN NULL WHEN c.attr_diff = '' THEN 'true' ELSE 'false' END AS attrs_match,
       c.attr_diff, c.result, c.src_size, c.dst_size,
       printf('%04o', c.src_mode) AS src_mode,
       CASE WHEN c.dst_mode IS NULL THEN NULL ELSE printf('%04o', c.dst_mode) END AS dst_mode,
       c.src_uid, c.dst_uid, c.src_gid, c.dst_gid, c.src_mtime_ns, c.dst_mtime_ns, c.src_xattrs, c.dst_xattrs
FROM file_checks c JOIN folders f ON f.id = c.folder;
DROP VIEW IF EXISTS hash_mismatches;
CREATE VIEW hash_mismatches AS
SELECT run, seq, kind, path, file_name, src_sha256, dst_sha256, src_size, dst_size,
       CASE WHEN dst_size IS NULL THEN 'missing on the target' ELSE 'content differs' END AS problem
FROM file_compare WHERE type = 'f' AND src_sha256 IS NOT NULL AND match = 'false';
DROP VIEW IF EXISTS attr_mismatches;
CREATE VIEW attr_mismatches AS
SELECT run, seq, kind, type, path, file_name, attr_diff, result, src_mode, dst_mode,
       src_uid, dst_uid, src_gid, dst_gid, src_mtime_ns, dst_mtime_ns, src_xattrs, dst_xattrs
FROM file_compare WHERE attrs_match = 'false';
)";

bool Db::Open(const String& path, String& error)
{
	Close();
	RealizePath(path);
	if(sqlite3_open(path, &db) != SQLITE_OK) {
		error = "cannot open database " + path + ": " + Error();
		Close();
		return false;
	}
	sqlite3_busy_timeout(db, 10000);
	if(!Exec(SCHEMA)) {
		error = "cannot create tables in " + path + ": " + Error();
		Close();
		return false;
	}
	// Schema 2: actions.replaces; schema 3: runs.log; schema 4: folders and file_checks
	// (new tables, made above). Older databases get the columns.
	String version;
	{
		Stmt v(*this, "SELECT value FROM meta WHERE key = 'schema'");
		if(v.Step())
			version = v.Text(0);
	}   // finalize before altering the schema
	if(version == "1")
		Exec("ALTER TABLE actions ADD COLUMN replaces INTEGER DEFAULT 0");
	if(version == "1" || version == "2")
		Exec("ALTER TABLE runs ADD COLUMN log TEXT");
	Exec("UPDATE meta SET value = '4' WHERE key = 'schema'");
	// Views for looking at the history (the GUI's History window, or sqlite3 directly).
	// Recreated each time so they always match this version of TreeSync.
	if(!Exec(VIEWS)) {
		error = "cannot create views in " + path + ": " + Error();
		Close();
		return false;
	}
	return true;
}

void Db::Close()
{
	if(db)
		sqlite3_close(db);
	db = nullptr;
}

bool Db::Exec(const char *sql)
{
	return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

String Db::Error() const
{
	return db ? String(sqlite3_errmsg(db)) : String("no database");
}

int64 Db::LastId() const
{
	return sqlite3_last_insert_rowid(db);
}

Stmt::Stmt(Db& db, const char *sql)
{
	if(sqlite3_prepare_v2(db.Handle(), sql, -1, &st, nullptr) != SQLITE_OK)
		st = nullptr;
	TS_ASSERT(st);
}

Stmt::~Stmt()
{
	sqlite3_finalize(st);
}

Stmt& Stmt::Int(int i, int64 v)
{
	sqlite3_bind_int64(st, i, v);
	return *this;
}

Stmt& Stmt::Text(int i, const String& v)
{
	sqlite3_bind_text(st, i, v, v.GetCount(), SQLITE_TRANSIENT);
	return *this;
}

bool Stmt::Step()
{
	return sqlite3_step(st) == SQLITE_ROW;
}

bool Stmt::Run()
{
	int rc = sqlite3_step(st);
	sqlite3_reset(st);
	sqlite3_clear_bindings(st);
	return rc == SQLITE_DONE || rc == SQLITE_ROW;
}

void Stmt::Reset()
{
	sqlite3_reset(st);
	sqlite3_clear_bindings(st);
}

int64 Stmt::Int(int col) const
{
	return sqlite3_column_int64(st, col);
}

String Stmt::Text(int col) const
{
	const char *s = (const char *)sqlite3_column_text(st, col);
	return s ? String(s, sqlite3_column_bytes(st, col)) : String();
}

int Stmt::Columns() const
{
	return sqlite3_column_count(st);
}

String Stmt::Name(int col) const
{
	const char *s = sqlite3_column_name(st, col);
	return s ? String(s) : String();
}

String LogFolder(const String& db_path)
{
	return AppendFileName(GetFileFolder(Nvl(db_path, DefaultDbPath())), "logs");
}

void SetRunLog(const String& db_path, int64 run, const String& log)
{
	Db db;
	String error;
	if(run > 0 && db.Open(Nvl(db_path, DefaultDbPath()), error)) {
		Stmt(db, "UPDATE runs SET log = ? WHERE id = ?").Text(1, log).Int(2, run).Run();
		// Every log of the run: one per start, a resumed run has several.
		Stmt(db, "INSERT OR IGNORE INTO run_logs VALUES (?, ?)").Int(1, run).Text(2, log).Run();
	}
}

String DefaultDbPath()
{
	String data = GetEnv("XDG_DATA_HOME");
	if(data.IsEmpty())
		data = AppendFileName(GetHomeDirectory(), ".local/share");
	return AppendFileName(data, "treesync/treesync.db");
}
