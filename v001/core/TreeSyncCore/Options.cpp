// Command-line options, Linux style: --long-name, --long-name=value or
// --long-name value, and --no-long-name for switches that default on.
// One table drives parsing, the command line shown in the GUI, and --help.
// Each switch notes its robocopy equivalent for reference.
#include "TreeSyncCore.h"

#include <charconv>

const BoolOption BOOL_OPTIONS[] = {
	{ "dry-run",          &SyncFlags::dry_run,          "/L",   "Dry run: report only, change nothing" },
	{ "fat-file-times",   &SyncFlags::fat_file_times,   "/FFT", "FAT file times: 2-second tolerance" },
	{ "dst-tolerance",    &SyncFlags::dst_tolerance,    "/DST", "Allow exact 1-hour (DST) time shifts" },
	{ "follow-links",     &SyncFlags::follow_links,     "",     "Write out symlink targets, not the links" },
	{ "hard-links",       &SyncFlags::hard_links,       "",     "Relink hard-linked files, not copies" },
	{ "compress",         &SyncFlags::compress,         "",     "Compress SSH traffic (helps text, not media)" },
	{ "include-same",     &SyncFlags::include_same,     "/IS",  "Include same files (rehash / recopy)" },
	{ "skip-older",       &SyncFlags::skip_older,       "/XO",  "Skip if source is older than target" },
	{ "skip-newer",       &SyncFlags::skip_newer,       "/XN",  "Skip if source is newer than target" },
	{ "skip-changed",     &SyncFlags::skip_changed,     "/XC",  "Skip changed files (same time, new hash)" },
	{ "keep-extra",       &SyncFlags::keep_extra,       "/XX",  "Keep extra target files (mirror only)" },
	{ "skip-source-only", &SyncFlags::skip_source_only, "/XL",  "Skip files that exist only in source" },
};
static_assert(std::size(BOOL_OPTIONS) == BOOL_OPTION_COUNT);

const IntOption INT_OPTIONS[] = {
	{ "retries",    &SyncFlags::retries,    "/R:n",  "Retries on a failed copy",  0, 1000000 },
	{ "retry-wait", &SyncFlags::retry_wait, "/W:n",  "Seconds between retries",   0, 3600 },
	{ "threads",    &SyncFlags::threads,    "/MT:n", "Copy threads",              1, 128 },
	{ "big-file-mib", &SyncFlags::big_file_mib, "",  "Chunked copy from (MiB)",   1, 1048576 },
	{ "chunk-mib",  &SyncFlags::chunk_mib,  "",      "Chunk size (MiB)",          1, 1024 },
};
static_assert(std::size(INT_OPTIONS) == INT_OPTION_COUNT);

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
	if(e.use_key && e.key_file.GetCount())
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
		if(name == "db" || name == "known-hosts") {
			if(!need())
				return PARSE_ERROR;
			(name == "db" ? s.db : s.known_hosts) = CleanPath(value);
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
				error = "passwords are not accepted on the command line (other users can see them in 'ps'); "
				        "use --" + String(side) + "-key or the setup screen";
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
				e.use_key = true;
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
	  << "Sync two directory trees by SHA-256. Opens the GUI with these settings.\n\n"
	  << "Trees (SIDE is source or target):\n"
	  << "  --SIDE=PATH            tree root\n"
	  << "  --SIDE-host=HOST       remote machine (makes the side remote)\n"
	  << "  --SIDE-user=USER       remote login\n"
	  << "  --SIDE-port=PORT       SSH port (default 22)\n"
	  << "  --SIDE-key=FILE        SSH key file\n"
	  << "  Login order: --SIDE-key, a password (setup screen, or treesync-cli\n"
	  << "  --password-stdin), ssh-agent, then ~/.ssh/id_ed25519, id_ecdsa, id_rsa.\n"
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
	  << "  --known-hosts=FILE     SSH known_hosts file (default ~/.ssh/known_hosts)\n\n"
	  << "  -h, --help             Show this help\n";
	return u;
}
