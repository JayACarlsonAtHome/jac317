// Command-line options, Linux style: --long-name, --long-name=value or
// --long-name value, and --no-long-name for switches that default on.
// One table drives parsing, the command line shown in the GUI, and --help.
// Each switch notes its robocopy equivalent for reference.
#include "TreeSyncCore.h"

#include <charconv>

const char *DISCLAIMER =
	"TreeSync is experimental testing software. It copies, overwrites and deletes files,\n"
	"and can suggest commands that change user accounts. Use it at your own risk: try it\n"
	"on copies first and keep backups. It is provided \"AS IS\", without warranty of any\n"
	"kind, and the authors and contributors are not liable for any damage or loss of\n"
	"data arising from its use. See sections 7 and 8 of the Apache License 2.0 (LICENSE).\n";

const BoolOption BOOL_OPTIONS[] = {
	{ "dry-run", &SyncFlags::dry_run, "/L", "Dry run: report only, change nothing",
	  "Shows what a sync would do without changing anything. In the GUI, use Sync > Dry run instead." },
	{ "fat-file-times", &SyncFlags::fat_file_times, "/FFT", "FAT file times: 3-second tolerance",
	  "Treats file times up to 3 seconds apart as equal. Turn it on when either side is a FAT or "
	  "exFAT disk (USB sticks, SD cards) or some network shares: they keep times only in 2-second "
	  "steps, and the two sides may round differently, so without this every file there looks "
	  "changed. (robocopy's /FFT allows 2 seconds; TreeSync allows one more for safety.)" },
	{ "dst-tolerance", &SyncFlags::dst_tolerance, "/DST", "Allow 1-hour (DST) time shifts",
	  "Treats file times one hour apart, give or take 3 seconds, as equal. For FAT disks and Windows "
	  "shares whose times jump by an hour when daylight saving time starts or ends." },
	{ "follow-links", &SyncFlags::follow_links, "", "Copy what symlinks point to, not the links",
	  "A symlink (symbolic or soft link) is a tiny file that holds only a path to another file or "
	  "folder, like a shortcut.\n"
	  "Off (normal): the symlink itself is copied, holding the same path. A relative path such as "
	  "'v3' then finds the copy of v3 on the target. An absolute path such as '/home/jay/docs/v3' "
	  "points to that path on the target machine, which may not exist there.\n"
	  "On: each symlink is replaced by a real copy of what it points to.\n"
	  "Example: docs/latest is a symlink to v3. Off: the target gets docs/latest as a symlink to v3. "
	  "On: the target gets docs/latest as a real folder with all of v3's files in it, so those "
	  "files take up space twice, once in v3 and once in latest." },
	{ "hard-links", &SyncFlags::hard_links, "", "Relink hard-linked files, not copies",
	  "A hard link is one file with two or more names: change it under one name and it changes under "
	  "all of them. On (normal): such a file is copied once and its other names are linked to it on "
	  "the target, so it stays one file and uses the space once. Off: every name becomes its own "
	  "separate copy. Over SFTP hard links cannot be made, so they are always written out." },
	{ "compress", &SyncFlags::compress, "", "Compress SSH traffic (helps text, not media)",
	  "Compresses what is sent over SSH; the other side unpacks it automatically. Helps for text, logs "
	  "and documents on a slow connection. Photos, video and zip files are already compressed, so it "
	  "only costs CPU time for them. No effect on local copies." },
	{ "preserve-owner", &SyncFlags::preserve_owner, "/COPY:O", "Copy owner and group (by name; needs root)",
	  "Gives each copy the same owner and group as the original, matched by name: user 'jay' stays "
	  "'jay' even when that user has a different number on the other machine. Needs root on the side "
	  "being written; otherwise copies belong to whoever runs TreeSync." },
	{ "numeric-ids", &SyncFlags::numeric_ids, "", "Copy owners by number, not by name",
	  "With 'Copy owner and group', uses the raw user and group numbers instead of the names. Only "
	  "for machines that number their users the same way; otherwise files can end up owned by the "
	  "wrong user." },
	{ "safe-delete", &SyncFlags::safe_delete, "", "Move deleted/replaced files to <target>.old",
	  "Nothing a sync removes or overwrites is lost: those files are moved to a folder next to the "
	  "target, <target>.old, in the same folder layout. A name already there gets .1, .2, ... added. "
	  "Off: removed and replaced files are deleted for good." },
	{ "low-priority", &SyncFlags::low_priority, "", "Low CPU and disk priority (nice, ionice)",
	  "Runs the sync at the lowest CPU and disk priority, so other programs and copies go first. The "
	  "sync takes longer while the machine is busy. Does not change the remote machine's priority." },
	{ "include-same", &SyncFlags::include_same, "/IS", "Include same files (rehash / recopy)",
	  "Copies files even when they are already identical on both sides. Normally they are skipped. "
	  "Useful to rewrite a target you suspect is damaged." },
	{ "skip-older", &SyncFlags::skip_older, "/XO", "Skip if source is older than target",
	  "Does not copy a file when the source version is older than the one on the target. Protects "
	  "newer changes made on the target." },
	{ "skip-newer", &SyncFlags::skip_newer, "/XN", "Skip if source is newer than target",
	  "Does not copy a file when the source version is newer than the one on the target." },
	{ "skip-changed", &SyncFlags::skip_changed, "/XC", "Skip changed files (same time, new hash)",
	  "Does not copy a file whose content differs but whose time is the same on both sides." },
	{ "keep-extra", &SyncFlags::keep_extra, "/XX", "Keep extra target files (mirror only)",
	  "Mirror normally removes files from the target that the source does not have. With this on, "
	  "they are kept. Copy mode never removes anything anyway." },
	{ "skip-source-only", &SyncFlags::skip_source_only, "/XL", "Skip files that exist only in source",
	  "Only updates files the target already has; files that are new in the source are not copied." },
};
static_assert(std::size(BOOL_OPTIONS) == BOOL_OPTION_COUNT);

const IntOption INT_OPTIONS[] = {
	{ "retries", &SyncFlags::retries, "/R:n", "Retries on a failed copy", 0, 1000000,
	  "How many more times a failed copy is tried. Errors that waiting cannot fix (permission denied, "
	  "disk full) are not retried." },
	{ "retry-wait", &SyncFlags::retry_wait, "/W:n", "Seconds between retries", 0, 3600,
	  "How long to wait before each retry, in seconds." },
	{ "threads", &SyncFlags::threads, "/MT:n", "Copy threads", 1, 128,
	  "How many files are copied at the same time. More helps with many small files. A remote side "
	  "gets one SSH connection per thread. 1 copies one file at a time." },
	{ "big-file-mib", &SyncFlags::big_file_mib, "", "Chunked copy from (MiB)", 1, 1048576,
	  "Files at least this big, in MiB, are copied in pieces that are recorded as they finish, so a "
	  "stopped copy continues where it left off. Smaller files are started over if stopped. Remote "
	  "copies always use pieces." },
	{ "chunk-mib", &SyncFlags::chunk_mib, "", "Chunk size (MiB)", 1, 1024,
	  "The size of each recorded piece of a big file, in MiB. Smaller pieces lose less when stopped "
	  "but are recorded more often." },
	{ "bwlimit", &SyncFlags::bwlimit_kib, "/IPG:n", "Bandwidth limit (KiB/s)", 0, 10485760,
	  "Caps the speed in KiB per second (1024 is 1 MiB/s), leaving room for other work. 0 means no "
	  "limit. Counts network traffic; for a local copy it limits the writes." },
};
static_assert(std::size(INT_OPTIONS) == INT_OPTION_COUNT);

const char *HELP_MODE[3] = {
	"Copy: new and changed files go from source to target. Nothing on the target is ever removed.",
	"Mirror: the target is made to match the source, so files the target has but the source does "
	"not are removed (with safe delete they are moved to <target>.old).",
	"Merge: changes go both ways. A file changed on only one side is copied to the other; a file "
	"changed on both sides since the last sync is a conflict and left alone. Nothing is removed.",
};
const char *HELP_OWNERS =
	"Who the copies belong to. Setting an owner needs root on the side being written; without "
	"it, copies belong to whoever runs TreeSync.\n"
	"Don't copy: copies belong to whoever runs TreeSync.\n"
	"By name (normal): each copy gets the same owner and group names as the original. User 'jay' "
	"stays 'jay' even when that user has a different number on the other machine.\n"
	"By number: the raw user and group numbers are copied. Only for machines that number their "
	"users the same way; otherwise files can end up owned by the wrong user. (--numeric-ids)";
const char *HELP_EXCLUDE_FILES =
	"File names to leave out, with * and ? as wildcards, separated by ';'. Example: *.tmp;Thumbs.db";
const char *HELP_EXCLUDE_DIRS =
	"Folder names to leave out, with everything inside them; * and ? are wildcards, ';' separates. "
	"Example: .git;node_modules;cache*";

static const struct { const char *name; SyncMode mode; const char *robocopy; const char *text; } MODES[] = {
	{ "copy",   SyncMode::Copy,   "/E",   "Copy new and changed files, never delete" },
	{ "mirror", SyncMode::Mirror, "/MIR", "Make target match source, deleting extras" },
	{ "merge",  SyncMode::Merge,  "",     "Copy both ways; changed on both sides = conflict" },
};

// Quotes for a POSIX shell only when needed: --name='value with spaces'.
static String ShellQuote(const String& arg)
{
	auto safe = [](int c) { return IsAlNum(c) || strchr("-_./:=,+@%", c); };
	bool ok = arg.GetCount() > 0;
	for(char c : arg)
		ok = ok && safe((byte)c);
	if(ok)
		return arg;
	int eq = arg.StartsWith("--") ? arg.Find('=') : -1;
	String head = eq >= 0 ? arg.Left(eq + 1) : String();
	String value = eq >= 0 ? arg.Mid(eq + 1) : arg;
	String q = head + "'";
	for(char c : value)
		if(c == '\'')
			q << "'\\''";
		else
			q.Cat(c);
	return q + "'";
}

static void EndpointArgs(const char *side, const TreeEndpoint& e, Vector<String>& args)
{
	String s = side;
	if(e.path.GetCount())
		args.Add("--" + s + "=" + e.path);
	if(!e.remote)
		return;
	args.Add("--" + s + "-host=" + e.host);
	if(e.user.GetCount())
		args.Add("--" + s + "-user=" + e.user);
	if(e.port != 22)
		args.Add("--" + s + "-port=" + AsString(e.port));
	if(e.key_file.GetCount())
		args.Add("--" + s + "-key=" + e.key_file);
}

// Shows every switch that is on, a --no- form for a default-on switch that is
// off, and numbers that differ from their defaults.
Vector<String> FlagArgs(const SyncFlags& f)
{
	Vector<String> args;
	const SyncFlags def;
	for(const auto& m : MODES)
		if(m.mode == f.mode)
			args.Add(String("--") + m.name);
	for(const BoolOption& o : BOOL_OPTIONS)
		if(f.*o.field)
			args.Add(String("--") + o.name);
		else if(def.*o.field)
			args.Add(String("--no-") + o.name);
	for(const IntOption& o : INT_OPTIONS)
		if(f.*o.field != def.*o.field)
			args.Add(String("--") + o.name + "=" + AsString(f.*o.field));
	for(const String& p : Split(f.exclude_files, ';'))
		args.Add("--exclude-file=" + p);
	for(const String& p : Split(f.exclude_dirs, ';'))
		args.Add("--exclude-dir=" + p);
	return args;
}

Vector<String> ToArgs(const SyncSetup& s)
{
	Vector<String> args;
	EndpointArgs("source", s.source, args);
	EndpointArgs("target", s.target, args);
	args.Append(FlagArgs(s.flags));
	if(s.db.GetCount())
		args.Add("--db=" + s.db);
	if(s.known_hosts.GetCount())
		args.Add("--known-hosts=" + s.known_hosts);
	if(s.safe_delete_dir.GetCount())
		args.Add("--safe-delete-dir=" + s.safe_delete_dir);
	if(s.fresh)
		args.Add("--fresh");
	return args;
}

String CommandLineText(const Vector<String>& args)
{
	String s = ToLower(GetExeTitle());
	for(const String& a : args)
		s << ' ' << ShellQuote(a);
	return s;
}

static bool ParseInt(const String& v, int lo, int hi, int& out)
{
	const char *b = v.Begin(), *e = v.End();
	auto r = std::from_chars(b, e, out);
	return v.GetCount() && r.ec == std::errc() && r.ptr == e && out >= lo && out <= hi;
}

int ParseCommandLine(const Vector<String>& args, SyncSetup& s, String& error)
{
	for(int i = 0; i < args.GetCount(); i++) {
		String arg = args[i];
		if(arg == "-h" || arg == "--help")
			return PARSE_HELP;
		if(!arg.StartsWith("--")) {
			error = "unexpected argument '" + arg + "'";
			return PARSE_ERROR;
		}
		String name = arg.Mid(2);
		String value;
		bool has_value = false;
		if(int eq = name.Find('='); eq >= 0) {
			value = name.Mid(eq + 1);
			name = name.Left(eq);
			has_value = true;
		}
		// Takes the value from --name=value or from the next argument.
		auto need = [&]() -> bool {
			if(has_value)
				return true;
			if(i + 1 >= args.GetCount()) {
				error = "--" + name + " needs a value";
				return false;
			}
			value = args[++i];
			return has_value = true;
		};
		auto flag_only = [&]() -> bool {
			if(has_value)
				error = "--" + name + " does not take a value";
			return !has_value;
		};

		bool done = false;
		for(const auto& m : MODES)
			if(name == m.name) {
				if(!flag_only())
					return PARSE_ERROR;
				s.flags.mode = m.mode;
				done = true;
			}
		for(const BoolOption& o : BOOL_OPTIONS) {
			bool on = name == o.name;
			bool off = name == String("no-") + o.name;
			if(on || off) {
				if(!flag_only())
					return PARSE_ERROR;
				s.flags.*o.field = on;
				// Owners are one choice: off, by name, or by number. --numeric-ids
				// means "by number" and so switches owner copying on; switching owner
				// copying off leaves no numbers to copy.
				if(o.field == &SyncFlags::numeric_ids && on)
					s.flags.preserve_owner = true;
				if(o.field == &SyncFlags::preserve_owner && !on)
					s.flags.numeric_ids = false;
				done = true;
			}
		}
		for(const IntOption& o : INT_OPTIONS)
			if(name == o.name) {
				if(!need())
					return PARSE_ERROR;
				if(!ParseInt(value, o.min, o.max, s.flags.*o.field)) {
					error = Format("--%s must be a whole number from %d to %d", name, o.min, o.max);
					return PARSE_ERROR;
				}
				done = true;
			}
		if(name == "fresh") {
			if(!flag_only())
				return PARSE_ERROR;
			s.fresh = true;
			continue;
		}
		if(name == "db" || name == "known-hosts" || name == "safe-delete-dir") {
			if(!need())
				return PARSE_ERROR;
			(name == "db" ? s.db : name == "known-hosts" ? s.known_hosts : s.safe_delete_dir) = CleanPath(value);
			continue;
		}
		if(name == "exclude-file" || name == "exclude-dir") {
			if(!need())
				return PARSE_ERROR;
			String& list = name == "exclude-file" ? s.flags.exclude_files : s.flags.exclude_dirs;
			list << (list.GetCount() ? ";" : "") << value;
			done = true;
		}
		for(const char *side : { "source", "target" }) {
			TreeEndpoint& e = String(side) == "source" ? s.source : s.target;
			String key = name.StartsWith(side) ? name.Mid(strlen(side)) : String("?");
			if(key == "-password") {
				error = "TreeSync has no password login: set up passwordless SSH (a key) for this "
				        "host first, then use ssh-agent, ~/.ssh/id_*, or --" + String(side) + "-key";
				return PARSE_ERROR;
			}
			if(key != "" && key != "-host" && key != "-user" && key != "-port" && key != "-key")
				continue;
			if(!need())
				return PARSE_ERROR;
			if(key == "")
				e.path = CleanPath(value);
			else if(key == "-host") {
				e.host = value;
				e.remote = true;
			}
			else if(key == "-user")
				e.user = value;
			else if(key == "-key") {
				e.key_file = CleanPath(value);
			}
			else if(!ParseInt(value, 1, 65535, e.port)) {
				error = "--" + name + " must be a port number from 1 to 65535";
				return PARSE_ERROR;
			}
			done = true;
		}
		if(!done) {
			error = "unknown option '--" + name + "'";
			return PARSE_ERROR;
		}
	}
	return PARSE_OK;
}

String Usage()
{
	const SyncFlags def;
	String u;
	u << "Usage: " << ToLower(GetExeTitle()) << " [OPTION]...\n"
	  << "Sync two directory trees by SHA-256. treesync opens the GUI with these\n"
	  << "settings; treesync-cli runs the sync in the terminal.\n\n"
	  << DISCLAIMER << "\n"
	  << "Trees (SIDE is source or target):\n"
	  << "  --SIDE=PATH            tree root\n"
	  << "  --SIDE-host=HOST       remote machine (makes the side remote)\n"
	  << "  --SIDE-user=USER       remote login\n"
	  << "  --SIDE-port=PORT       SSH port (default 22)\n"
	  << "  --SIDE-key=FILE        SSH key file\n"
	  << "  Keys only, no passwords: set up passwordless SSH first. Tried in order:\n"
	  << "  --SIDE-key, ssh-agent, then ~/.ssh/id_ed25519, id_ecdsa, id_rsa.\n"
	  << "  The host key must already be in known_hosts (connect once with ssh).\n\n"
	  << "Mode (default --mirror):\n";
	for(const auto& m : MODES)
		u << Format("  --%-21s%s", m.name, m.text)
		  << (*m.robocopy ? String(" (robocopy ") + m.robocopy + ")" : String()) << "\n";
	u << "\nSwitches (--no-NAME turns one off):\n";
	for(const BoolOption& o : BOOL_OPTIONS)
		u << Format("  --%-21s%s (robocopy %s)%s\n", o.name, o.text, o.robocopy,
		            def.*o.field ? ", default on" : "");
	u << "\nValues:\n";
	for(const IntOption& o : INT_OPTIONS)
		u << Format("  --%-21s%s, %d-%d, default %d", String(o.name) + "=N", o.text, o.min, o.max, def.*o.field)
		  << (*o.robocopy ? String(" (robocopy ") + o.robocopy + ")" : String()) << "\n";
	u << "  --exclude-file=GLOB    Skip matching files, repeatable (robocopy /XF)\n"
	  << "  --exclude-dir=GLOB     Skip matching folders, repeatable (robocopy /XD)\n\n"
	  << "Run:\n"
	  << "  --fresh                Start over instead of resuming an unfinished run\n"
	  << "  --db=PATH              Database file (default " << DefaultDbPath() << ")\n"
	  << "  --known-hosts=FILE     SSH known_hosts file (default ~/.ssh/known_hosts)\n"
	  << "  --safe-delete-dir=PATH Where --safe-delete moves files on the target side\n"
	  << "                         (default <target>.old; must be on the same disk)\n"
	  << "  --compare-ids          (treesync-cli) Compare users and groups on the two machines,\n"
	  << "                         suggest one numbering for both, and print the commands\n\n"
	  << "  -h, --help             Show this help\n";
	return u;
}
