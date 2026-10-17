// SQLite access: the bundled SQLite (plugin/sqlite3), raw C API, WAL mode.
#include "TreeSyncCore.h"

#include <plugin/sqlite3/lib/sqlite3mc_amalgamation.h>

static const char *SCHEMA = R"(
PRAGMA journal_mode = WAL;
PRAGMA synchronous = NORMAL;
CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT);
INSERT OR IGNORE INTO meta VALUES ('schema', '1');
CREATE TABLE IF NOT EXISTS files (
	root TEXT, rel TEXT, type TEXT,
	dev INTEGER, ino INTEGER, nlink INTEGER, size INTEGER,
	mtime_ns INTEGER, ctime_ns INTEGER, mode INTEGER, link TEXT, sha256 TEXT,
	seen INTEGER,
	PRIMARY KEY (root, rel));
CREATE TABLE IF NOT EXISTS runs (
	id INTEGER PRIMARY KEY, started INTEGER, finished INTEGER,
	source TEXT, target TEXT, args TEXT, state TEXT);
CREATE TABLE IF NOT EXISTS actions (
	run INTEGER, seq INTEGER, kind TEXT, dir INTEGER, rel TEXT, type TEXT, link TEXT, link_to TEXT,
	size INTEGER, sha256 TEXT, dev INTEGER, ino INTEGER, mtime_ns INTEGER, ctime_ns INTEGER,
	mode INTEGER, note TEXT, state TEXT, bytes INTEGER, error TEXT,
	PRIMARY KEY (run, seq));
CREATE TABLE IF NOT EXISTS chunks (
	run INTEGER, seq INTEGER, idx INTEGER, offset INTEGER, length INTEGER, sha256 TEXT,
	PRIMARY KEY (run, seq, idx));
CREATE TABLE IF NOT EXISTS synced (
	source TEXT, target TEXT, rel TEXT, sha256 TEXT,
	PRIMARY KEY (source, target, rel));
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

String DefaultDbPath()
{
	String data = GetEnv("XDG_DATA_HOME");
	if(data.IsEmpty())
		data = AppendFileName(GetHomeDirectory(), ".local/share");
	return AppendFileName(data, "treesync/treesync.db");
}
