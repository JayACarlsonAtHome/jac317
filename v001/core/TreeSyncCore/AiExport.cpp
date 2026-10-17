// A run exported as Markdown for analysis: organized (a guide, a contents list, numbered
// sections, tables) and complete (every log line, every action, the whole hash cache of
// the run's trees). An AI reads big files well; nothing is left out to make it smaller.
#include "TreeSyncCore.h"

#include <format>

namespace {

// Markdown table cell: no '|' or line breaks inside.
String Cell(const String& s)
{
	String r;
	for(char c : s)
		if(c == '|')
			r << "\\|";
		else if(c == '\n' || c == '\r')
			r << ' ';
		else
			r.Cat(c);
	return r;
}

// Any query as a Markdown table, every row. binds: "?" values, in order.
String QueryTable(Db& db, const char *sql, const Vector<Value>& binds = {})
{
	Stmt q(db, sql);
	for(int i = 0; i < binds.GetCount(); i++)
		if(IsNumber(binds[i]))
			q.Int(i + 1, (int64)binds[i]);
		else
			q.Text(i + 1, AsString(binds[i]));
	String t;
	int rows = 0;
	while(q.Step()) {
		if(rows++ == 0) {
			t << "|";
			for(int c = 0; c < q.Columns(); c++)
				t << " " << Cell(q.Name(c)) << " |";
			t << "\n|";
			for(int c = 0; c < q.Columns(); c++)
				t << "---|";
			t << "\n";
		}
		t << "|";
		for(int c = 0; c < q.Columns(); c++)
			t << " " << Cell(q.Text(c)) << " |";
		t << "\n";
	}
	if(rows == 0)
		return "(none)\n";
	return t + Format("\n%d row%s.\n", rows, rows == 1 ? "" : "s");
}

int64 Newest(Db& db)
{
	Stmt q(db, "SELECT max(id) FROM runs");
	return q.Step() ? q.Int(0) : 0;
}

String Bytes(int64 n)   { return FormatBytes(n); }

// A log line in the stamped form " HH:MM:SS : HH:MM:SS : YYY:DDD:HH:MM:SS : message".
struct LogLine {
	String start, stop, message;
	int64  seconds = -1;   // duration; -1 when the line has no stamp (older logs)
};

LogLine ParseLine(const String& line)
{
	LogLine l;
	l.message = line;
	if(line.GetCount() >= 43 && line[0] == ' ' && line.Mid(9, 3) == " : " && line.Mid(20, 3) == " : " &&
	   line.Mid(39, 3) == " : ") {
		l.start = line.Mid(1, 8);
		l.stop = line.Mid(12, 8);
		Vector<String> d = Split(line.Mid(23, 16), ':');
		if(d.GetCount() == 5)
			l.seconds = (((ScanInt64(d[0]) * 365 + ScanInt64(d[1])) * 24 + ScanInt64(d[2])) * 60 +
			             ScanInt64(d[3])) * 60 + ScanInt64(d[4]);
		l.message = line.Mid(42);
	}
	return l;
}

// "130.9 MiB" -> bytes.
int64 ParseSize(String s)
{
	s = TrimBoth(s);
	int sp = s.Find(' ');
	if(sp < 0)
		return 0;
	double v = ScanDouble(s.Left(sp));
	String u = s.Mid(sp + 1);
	double m = u == "KiB" ? 1024.0 : u == "MiB" ? 1048576.0 : u == "GiB" ? 1073741824.0
	         : u == "TiB" ? 1099511627776.0 : 1.0;
	return (int64)(v * m);
}

// A finished file's progress line: "[  6/90] name   100% |  1.6 MiB/s |  130.9 MiB |  01m21s".
bool CopyLine(const String& message, String& name, int64& size)
{
	if(!message.StartsWith("[") || message.Find("] Total") >= 0)
		return false;
	Vector<String> f = Split(message, '|', false);
	if(f.GetCount() < 4)
		return false;
	int close = f[0].Find("] ");
	String head = TrimBoth(f[0].Mid(close + 2));
	int pct = head.ReverseFind(' ');
	name = TrimBoth(pct > 0 ? head.Left(pct) : head);
	size = ParseSize(f[2]);
	return true;
}

String Guide()
{
	return
	"## How to read this\n\n"
	"TreeSync syncs two directory trees by SHA-256 (Copy, Mirror or Merge; local or over SFTP). "
	"Every copy is written to `NAME.treesync-part`, read back and checked against the source's "
	"SHA-256, then renamed into place. Big files and all remote copies go in chunks that are recorded "
	"as they finish, so a stopped run resumes part-way through a file. Each start of a run writes its "
	"own log; a run that was stopped and resumed has several.\n\n"
	"**Log lines** start with three time columns, then the message:\n\n"
	"```\n"
	" START    : STOP     : DURATION         : MESSAGE\n"
	" 14:16:45 : 14:16:52 : 000:000:00:00:07 : [1/4] big.bin        100% |   42.7 MiB/s |  300.0 MiB |  00m07s\n"
	"```\n\n"
	"- A copied file's line runs from when its copy began to when it finished (DURATION is "
	"years:days:hours:minutes:seconds, whole seconds).\n"
	"- Lines about the whole run (pre-scan done, post-scan done, Total, finished, took) run from the start of the run.\n"
	"- Other lines (folders created, deletes, moves to `.old`, warnings, `network:` events, `FAILED`, `CONFLICT`) "
	"show the moment they happened.\n"
	"- `[ 35/240+]`: the 35th file copied of 240 that may need copying; `+` while the pre-scan is still finding files. "
	"The columns after the name: percent, rate, size, elapsed.\n"
	"- `network: lost the connection ...` / `... is back after ...`: an outage the run waited out (`--network-wait`).\n"
	"- The table at the end of each log (`Result = ....0 : Good`) is the j5c Results.txt layout. Result is the exit "
	"code: 0 in sync, 1 failures or conflicts, 2 error, 3 stopped.\n"
	"- Older logs (before 2026-10-17 afternoon) have no time columns.\n\n"
	"**Questions worth asking**: what failed and why; whether the network or the disks set the speed "
	"(rates of big files against small ones, gaps between files); outages; which files took longest; "
	"whether the thread count (`--threads`) suits the mix of file sizes.\n\n";
}

String RunTable(Db& db, int64 run)
{
	return QueryTable(db, "SELECT run, started, finished, state, source, target, folders, copied, linked, deleted, "
	                      "failed, conflicts, bytes, args FROM run_summary WHERE run = ?", { run });
}

}

Vector<String> RunLogs(const String& db_path, int64 run)
{
	Index<String> logs;
	String folder = LogFolder(db_path);
	Db db;
	String error;
	if(db.Open(Nvl(db_path, DefaultDbPath()), error)) {
		Stmt q(db, "SELECT log FROM run_logs WHERE run = ? UNION SELECT log FROM runs WHERE id = ? AND log <> ''");
		q.Int(1, run).Int(2, run);
		while(q.Step())
			if(FileExists(q.Text(0)))
				logs.FindAdd(q.Text(0));
	}
	// Logs from before run_logs existed: each says "run N" or "resuming run N" near the top.
	String a = Format("run %d", run), b = Format("resuming run %d", run);
	for(FindFile ff(AppendFileName(folder, "*.log")); ff; ff.Next()) {
		if(logs.Find(ff.GetPath()) >= 0)
			continue;
		FileIn in(ff.GetPath());
		for(int i = 0; i < 40 && !in.IsEof(); i++) {
			String m = ParseLine(in.GetLine()).message;
			if(m == a || m == b) {
				logs.Add(ff.GetPath());
				break;
			}
		}
	}
	Vector<String> v = logs.PickKeys();
	Sort(v);   // the names are their start times
	return v;
}

bool ExportAiLogs(const String& db_path, int64 run, String& out_path, String& error)
{
	Db db;
	if(!db.Open(Nvl(db_path, DefaultDbPath()), error))
		return false;
	if(run <= 0)
		run = Newest(db);
	if(run <= 0) {
		error = "no runs in the database yet";
		return false;
	}
	Vector<String> logs = RunLogs(db_path, run);
	String md;
	md << "# TreeSync run " << run << ": logs, for analysis\n\n"
	   << "Exported " << DateTimeText((int64)time(nullptr)) << " from `" << Nvl(db_path, DefaultDbPath()) << "`; "
	   << logs.GetCount() << " log file" << (logs.GetCount() == 1 ? "" : "s") << ", all of them in full.\n\n"
	   << "## Contents\n\n"
	   << "1. How to read this\n2. The run\n3. Each start of the run\n4. Problems and events\n"
	   << "5. The slowest copies\n6. Copies per minute (throughput)\n7. The logs in full\n\n"
	   << Guide()
	   << "## 2. The run\n\n" << RunTable(db, run) << "\n";

	// Read every log once.
	struct Log { String path; Vector<String> lines; };
	Array<Log> all;
	for(const String& p : logs) {
		Log& l = all.Add();
		l.path = p;
		l.lines = Split(LoadFile(p), '\n', false);
	}

	md << "## 3. Each start of the run\n\n| # | Log | Lines | First | Last | Result |\n|---|---|---|---|---|---|\n";
	for(int i = 0; i < all.GetCount(); i++) {
		String first, last, result;
		for(const String& line : all[i].lines) {
			LogLine l = ParseLine(line);
			if(l.seconds >= 0) {
				if(first.IsEmpty())
					first = l.start;
				last = l.stop;
			}
			if(line.Find(" : Result = ") >= 0)
				result = TrimBoth(line.Mid(line.Find(" : Result = ") + 3));
		}
		md << "| " << i + 1 << " | `" << Cell(GetFileName(all[i].path)) << "` | " << all[i].lines.GetCount() << " | "
		   << first << " | " << last << " | " << Cell(result) << " |\n";
	}

	md << "\n## 4. Problems and events\n\nEvery line about a failure, conflict, warning, the network, a stop or a "
	      "resume, with the log it is in.\n\n| Log | Time | Line |\n|---|---|---|\n";
	int events = 0;
	for(int i = 0; i < all.GetCount(); i++)
		for(const String& line : all[i].lines) {
			LogLine l = ParseLine(line);
			const String& m = l.message;
			if(m.StartsWith("FAILED") || m.StartsWith("CONFLICT") || m.StartsWith("warning") ||
			   m.StartsWith("network:") || m.StartsWith("stopped") || m.StartsWith("resuming") ||
			   m.StartsWith("treesync-cli:") || m.Find("retry ") == 0) {
				md << "| " << i + 1 << " | " << l.stop << " | " << Cell(m) << " |\n";
				events++;
			}
		}
	if(!events)
		md << "| | | (none) |\n";

	// Copies: the slowest, and how many and how much finished in each minute.
	struct Copy : Moveable<Copy> { int log; String name, start, stop; int64 seconds, size; };
	Vector<Copy> copies;
	VectorMap<String, Tuple<int, int64>> per_minute;   // "log HH:MM" -> files, bytes
	for(int i = 0; i < all.GetCount(); i++)
		for(const String& line : all[i].lines) {
			LogLine l = ParseLine(line);
			Copy c;
			if(l.seconds < 0 || !CopyLine(l.message, c.name, c.size))
				continue;
			c.log = i + 1;
			c.start = l.start;
			c.stop = l.stop;
			c.seconds = l.seconds;
			copies.Add(c);
			Tuple<int, int64>& m = per_minute.GetAdd(Format("%d %s", i + 1, l.stop.Left(5)), MakeTuple(0, (int64)0));
			m.a++;
			m.b += c.size;
		}
	Vector<Copy> slow = clone(copies);
	Sort(slow, [](const Copy& a, const Copy& b) { return a.seconds > b.seconds; });
	md << "\n## 5. The slowest copies\n\n" << copies.GetCount() << " copies have time columns; the 200 slowest:\n\n"
	   << "| Log | Start | Stop | Seconds | Size | Rate | File |\n|---|---|---|---|---|---|---|\n";
	for(int i = 0; i < min(slow.GetCount(), 200); i++) {
		const Copy& c = slow[i];
		md << "| " << c.log << " | " << c.start << " | " << c.stop << " | " << c.seconds << " | " << Bytes(c.size)
		   << " | " << (c.seconds > 0 ? Bytes(c.size / c.seconds) + "/s" : String("-")) << " | " << Cell(c.name) << " |\n";
	}
	md << "\n## 6. Copies per minute (throughput)\n\nFiles and bytes whose copy finished in each minute.\n\n"
	   << "| Log | Minute | Files | Bytes | Average rate |\n|---|---|---|---|---|\n";
	for(int i = 0; i < per_minute.GetCount(); i++) {
		Vector<String> k = Split(per_minute.GetKey(i), ' ');
		md << "| " << k[0] << " | " << k[1] << " | " << per_minute[i].a << " | " << Bytes(per_minute[i].b) << " | "
		   << Bytes(per_minute[i].b / 60) << "/s |\n";
	}

	md << "\n## 7. The logs in full\n\n";
	for(int i = 0; i < all.GetCount(); i++) {
		md << "### Log " << i + 1 << ": `" << all[i].path << "`\n\n```text\n";
		for(const String& line : all[i].lines)
			md << line << "\n";
		md << "```\n\n";
	}

	String folder = AppendFileName(LogFolder(db_path), "ai");
	RealizeDirectory(folder);
	out_path = AppendFileName(folder, Format("run-%d-logs.AI.md", run));
	if(!SaveFile(out_path, md)) {
		error = "cannot write " + out_path;
		return false;
	}
	return true;
}

bool ExportAiSql(const String& db_path, int64 run, String& out_path, String& error)
{
	Db db;
	String path = Nvl(db_path, DefaultDbPath());
	if(!db.Open(path, error))
		return false;
	if(run <= 0)
		run = Newest(db);
	if(run <= 0) {
		error = "no runs in the database yet";
		return false;
	}
	String source, target;
	{
		Stmt q(db, "SELECT source, target FROM runs WHERE id = ?");
		q.Int(1, run);
		if(q.Step()) {
			source = q.Text(0);
			target = q.Text(1);
		}
	}
	String md;
	md << "# TreeSync run " << run << ": the database, for analysis\n\n"
	   << "Exported " << DateTimeText((int64)time(nullptr)) << " from `" << path << "` (SQLite, WAL). "
	   << "Every table below is complete for this run and its two trees.\n\n"
	   << "## Contents\n\n"
	   << "1. How to read this\n2. Schema\n3. All runs\n4. This run: actions by kind and state\n"
	   << "5. This run: failures\n6. This run: the biggest copies\n7. This run: chunks of big files\n"
	   << "8. This run: every action\n9. The logs of every run\n10. Hash cache: the source tree\n"
	   << "11. Hash cache: the target tree\n12. Synced: what the two trees last agreed on\n13. Queries to start from\n\n"
	   << "## 1. How to read this\n\n"
	   << "TreeSync records everything in SQLite before doing it, so a stopped run resumes where it was.\n\n"
	   << "- `runs`: one row per run (`state`: running, stopped, done, failed, abandoned; `args`: the switches; "
	      "`started`/`finished`: Unix seconds).\n"
	   << "- `actions`: what a run decided and did, in order (`seq`). `kind`: mkdir, copy, link, delete, conflict. "
	      "`dir`: 0 source to target, 1 back (Merge). `state`: planned, in_progress, done, failed. `replaces`: 1 when "
	      "the target had a different version (moved to `.old` with safe delete).\n"
	   << "- `chunks`: big and remote files are copied in chunks; each verified chunk is a row, so a restart continues "
	      "at the next one.\n"
	   << "- `files`: the hash cache, per tree (`root`): a file whose identity (device, inode, size, times) is "
	      "unchanged keeps its SHA-256 and is not read again. Remote trees (`sftp://...`) have no inodes and "
	      "whole-second times.\n"
	   << "- `synced`: the hash both trees had for each file at the last sync (Merge uses it to tell which side changed).\n"
	   << "- `run_logs`: every log file of every run (one per start).\n"
	   << "- Views: `run_summary`, `run_actions`, `failures`.\n"
	   << "- Times: `*_ns` columns are nanoseconds since 1970 (UTC).\n\n"
	   << "This run: source `" << source << "`, target `" << target << "`.\n\n";

	md << "## 2. Schema\n\n```sql\n";
	{
		Stmt q(db, "SELECT sql FROM sqlite_master WHERE sql IS NOT NULL ORDER BY type DESC, name");
		while(q.Step())
			md << q.Text(0) << ";\n\n";
	}
	md << "```\n\n";

	md << "## 3. All runs\n\n" << QueryTable(db, "SELECT * FROM run_summary ORDER BY run") << "\n";
	md << "## 4. This run: actions by kind and state\n\n"
	   << QueryTable(db, "SELECT kind, state, count(*) AS actions, sum(size) AS bytes, sum(replaces) AS replaced "
	                     "FROM actions WHERE run = ? GROUP BY kind, state ORDER BY kind, state", { run }) << "\n";
	md << "## 5. This run: failures\n\n"
	   << QueryTable(db, "SELECT seq, kind, rel, error FROM actions WHERE run = ? AND state = 'failed' ORDER BY seq",
	                 { run }) << "\n";
	md << "## 6. This run: the biggest copies\n\n"
	   << QueryTable(db, "SELECT seq, rel, size, state, replaces FROM actions WHERE run = ? AND kind = 'copy' "
	                     "ORDER BY size DESC LIMIT 200", { run }) << "\n";
	md << "## 7. This run: chunks of big files\n\n"
	   << QueryTable(db, "SELECT c.seq, a.rel, c.idx, c.offset, c.length, c.sha256 FROM chunks c "
	                     "LEFT JOIN actions a ON a.run = c.run AND a.seq = c.seq WHERE c.run = ? ORDER BY c.seq, c.idx",
	                 { run }) << "\n";
	md << "## 8. This run: every action\n\n"
	   << QueryTable(db, "SELECT seq, kind, dir, rel, type, size, state, bytes, replaces, note, error, link, link_to, "
	                     "mode, datetime(mtime_ns / 1000000000, 'unixepoch') AS mtime_utc, sha256 "
	                     "FROM actions WHERE run = ? ORDER BY seq", { run }) << "\n";
	md << "## 9. The logs of every run\n\n"
	   << QueryTable(db, "SELECT run, log FROM run_logs ORDER BY run, log") << "\n";
	for(int side = 0; side < 2; side++) {
		const String& root = side ? target : source;
		md << "## " << (side ? 11 : 10) << ". Hash cache: the " << (side ? "target" : "source") << " tree\n\n"
		   << "`" << root << "`\n\n"
		   << QueryTable(db, "SELECT type, count(*) AS entries, sum(size) AS bytes FROM files WHERE root = ? "
		                     "GROUP BY type", { root }) << "\n"
		   << QueryTable(db, "SELECT rel, type, size, nlink, mode, datetime(mtime_ns / 1000000000, 'unixepoch') "
		                     "AS mtime_utc, dev, ino, link, sha256 FROM files WHERE root = ? ORDER BY rel", { root })
		   << "\n";
	}
	md << "## 12. Synced: what the two trees last agreed on\n\n"
	   << QueryTable(db, "SELECT rel, sha256 FROM synced WHERE source = ? AND target = ? ORDER BY rel",
	                 { source, target }) << "\n";
	md << "## 13. Queries to start from\n\n```sql\n"
	   << "-- The runs, newest first\nSELECT * FROM run_summary ORDER BY run DESC;\n\n"
	   << "-- Why did things fail?\nSELECT run, rel, error FROM failures ORDER BY run DESC;\n\n"
	   << "-- Files copied again and again across runs to the same target\n"
	   << "SELECT a.rel, count(*) AS times FROM actions a JOIN runs r ON r.id = a.run\n"
	   << " WHERE a.kind = 'copy' AND a.state = 'done' AND r.target = '" << target << "'\n"
	   << " GROUP BY a.rel HAVING times > 1 ORDER BY times DESC;\n\n"
	   << "-- Big files still part-way (chunks recorded, not done)\n"
	   << "SELECT a.run, a.rel, count(c.idx) AS chunks_done, a.size FROM actions a JOIN chunks c\n"
	   << "  ON c.run = a.run AND c.seq = a.seq WHERE a.state <> 'done' GROUP BY a.run, a.seq;\n"
	   << "```\n";

	String folder = AppendFileName(LogFolder(db_path), "ai");
	RealizeDirectory(folder);
	out_path = AppendFileName(folder, Format("run-%d-sql.AI.md", run));
	if(!SaveFile(out_path, md)) {
		error = "cannot write " + out_path;
		return false;
	}
	return true;
}
