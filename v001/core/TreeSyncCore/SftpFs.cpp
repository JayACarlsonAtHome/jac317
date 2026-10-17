// Remote side of a sync, over SFTP, using libssh2 (from U++'s Core/SSH package)
// directly and in blocking mode. U++'s SshSession/SFtp wrappers are not used: the
// session's ssh-agent login gives up on the first "try again" in non-blocking mode,
// SFtp cannot create symlinks, and neither offers an atomic posix-rename.
//
// SFTP has no inode numbers and only whole-second times: the hash cache matches on
// size and time, and hard links are written out as copies.
#include "TreeSyncCore.h"

#include <Core/SSH/SSH.h>

#include <chrono>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>

static const int TIMEOUT_MS = 120000;

static const char *FxName(unsigned long fx)
{
	switch(fx) {
	case LIBSSH2_FX_NO_SUCH_FILE:          return "no such file";
	case LIBSSH2_FX_PERMISSION_DENIED:     return "permission denied";
	case LIBSSH2_FX_FAILURE:               return "failure";
	case LIBSSH2_FX_OP_UNSUPPORTED:        return "not supported by the server";
	case LIBSSH2_FX_NO_SPACE_ON_FILESYSTEM:return "no space left on the remote disk";
	case LIBSSH2_FX_QUOTA_EXCEEDED:        return "quota exceeded";
	case LIBSSH2_FX_FILE_ALREADY_EXISTS:   return "already exists";
	case LIBSSH2_FX_DIR_NOT_EMPTY:         return "folder not empty";
	case LIBSSH2_FX_NOT_A_DIRECTORY:       return "not a folder";
	case LIBSSH2_FX_NO_CONNECTION:
	case LIBSSH2_FX_CONNECTION_LOST:       return "connection lost";
	}
	return "error";
}

struct SftpFs::Impl {
	TreeEndpoint     e;
	String           known_hosts;
	int              sock = -1;
	LIBSSH2_SESSION *session = nullptr;
	LIBSSH2_SFTP    *sftp = nullptr;
	String           root;
	bool             compress = false;

	LIBSSH2_SESSION *S()    { return session; }

	String P(const String& rel) const
	{
		return rel.IsEmpty() ? root : root == "/" ? "/" + rel : root + "/" + rel;
	}

	String Err(const String& what)
	{
		char *msg = nullptr;
		int len = 0;
		int rc = S() ? libssh2_session_last_error(S(), &msg, &len, 0) : 0;
		if(rc == LIBSSH2_ERROR_SFTP_PROTOCOL && sftp)
			return what + ": " + FxName(libssh2_sftp_last_error(sftp));
		return what + ": " + (msg && len ? String(msg, len) : String("SSH error"));
	}

	unsigned long Fx()      { return sftp ? libssh2_sftp_last_error(sftp) : 0; }

	bool Attrs(const String& path, bool follow, LIBSSH2_SFTP_ATTRIBUTES& a)
	{
		return libssh2_sftp_stat_ex(sftp, path, path.GetCount(),
		                            follow ? LIBSSH2_SFTP_STAT : LIBSSH2_SFTP_LSTAT, &a) == 0;
	}

	String ReadLink(const String& path)
	{
		Buffer<char> buf(4096);
		int n = libssh2_sftp_symlink_ex(sftp, path, path.GetCount(), buf, 4096, LIBSSH2_SFTP_READLINK);
		return n > 0 ? String(buf, n) : String();
	}

	String RealPath(const String& path)
	{
		Buffer<char> buf(4096);
		int n = libssh2_sftp_symlink_ex(sftp, path, path.GetCount(), buf, 4096, LIBSSH2_SFTP_REALPATH);
		return n > 0 ? String(buf, n) : String();
	}

	bool HashPath(const String& path, String& hash, String& error, const std::atomic<bool> *stop, int64 *bytes,
	              Throttle *throttle = nullptr)
	{
		LIBSSH2_SFTP_HANDLE *h = libssh2_sftp_open_ex(sftp, path, path.GetCount(), LIBSSH2_FXF_READ, 0,
		                                              LIBSSH2_SFTP_OPENFILE);
		if(!h) {
			error = Err("cannot open " + path);
			return false;
		}
		Sha256Stream sha;
		Buffer<char> buf(1 << 20);
		int64 total = 0;
		for(;;) {
			if(stop && *stop) {
				libssh2_sftp_close_handle(h);
				error = "stopped";
				return false;
			}
			ssize_t n = libssh2_sftp_read(h, buf, 1 << 20);
			if(n < 0) {
				error = Err("cannot read " + path);
				libssh2_sftp_close_handle(h);
				return false;
			}
			if(n == 0)
				break;
			if(throttle)
				throttle->Account(n);
			sha.Put(buf, (int)n);
			total += n;
		}
		libssh2_sftp_close_handle(h);
		hash = sha.FinishString();
		if(bytes)
			*bytes = total;
		return true;
	}

	~Impl()
	{
		if(sftp)
			libssh2_sftp_shutdown(sftp);
		if(session) {
			libssh2_session_disconnect(session, "treesync done");
			libssh2_session_free(session);
		}
		if(sock >= 0)
			close(sock);
	}
};

SftpFs::SftpFs(const TreeEndpoint& e, const String& known_hosts, bool compress)
{
	impl.Create();
	impl->e = e;
	impl->e.user = Nvl(e.user, GetUserName());
	impl->compress = compress;
	impl->known_hosts = Nvl(known_hosts, AppendFileName(GetHomeDirectory(), ".ssh/known_hosts"));
}

SftpFs::~SftpFs() {}

String SftpFs::Id() const
{
	return Format("sftp://%s@%s:%d%s", impl->e.user, impl->e.host, impl->e.port, impl->root);
}

// TCP connect with a timeout, trying each address the name resolves to.
static int ConnectTcp(const String& host, int port, String& error)
{
	addrinfo hints = {}, *res = nullptr;
	hints.ai_socktype = SOCK_STREAM;
	int rc = getaddrinfo(host, AsString(port), &hints, &res);
	if(rc != 0) {
		error = "cannot resolve " + host + ": " + gai_strerror(rc);
		return -1;
	}
	int fd = -1;
	for(addrinfo *ai = res; ai && fd < 0; ai = ai->ai_next) {
		fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK, ai->ai_protocol);
		if(fd < 0)
			continue;
		if(connect(fd, ai->ai_addr, ai->ai_addrlen) != 0 && errno == EINPROGRESS) {
			pollfd p = { fd, POLLOUT, 0 };
			int soerr = 0;
			socklen_t len = sizeof(soerr);
			if(poll(&p, 1, 15000) != 1 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len) != 0 || soerr) {
				error = Format("cannot connect to %s port %d: %s", host, port,
				               soerr ? strerror(soerr) : "timed out");
				close(fd);
				fd = -1;
				continue;
			}
		}
		fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
		// SFTP is many small request/reply pairs: without this, Nagle's algorithm and
		// delayed ACKs stall each one by tens of milliseconds.
		int one = 1;
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
		// Keep-alive: on a long run some connections sit idle for minutes (pre-scan,
		// post-scan); probes keep firewalls and NAT from dropping them and notice a
		// dead server: first probe after 60 s idle, then every 15 s, give up after 4.
		int idle = 60, interval = 15, count = 4;
		setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
		setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
		setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
		setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
	}
	freeaddrinfo(res);
	if(fd < 0 && error.IsEmpty())
		error = Format("cannot connect to %s port %d", host, port);
	return fd;
}

// Checks the server's host key against known_hosts, like ssh does: an unknown or
// changed key refuses the connection.
static bool CheckHostKey(LIBSSH2_SESSION *s, const String& host, int port, const String& file, String& error)
{
	size_t len = 0;
	int type = 0;
	const char *key = libssh2_session_hostkey(s, &len, &type);
	const char *sha = libssh2_hostkey_hash(s, LIBSSH2_HOSTKEY_HASH_SHA256);
	String fp = "SHA256:" + Base64Encode(String(sha ? sha : "", sha ? 32 : 0));
	fp.TrimEnd("=");
	LIBSSH2_KNOWNHOSTS *kh = libssh2_knownhost_init(s);
	if(!key || !kh || libssh2_knownhost_readfile(kh, file, LIBSSH2_KNOWNHOST_FILE_OPENSSH) < 0) {
		if(kh)
			libssh2_knownhost_free(kh);
		error = "cannot read known_hosts file " + file;
		return false;
	}
	libssh2_knownhost *found = nullptr;
	int r = libssh2_knownhost_checkp(kh, host, port, key, len,
	                                 LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW, &found);
	libssh2_knownhost_free(kh);
	if(r == LIBSSH2_KNOWNHOST_CHECK_MATCH)
		return true;
	if(r == LIBSSH2_KNOWNHOST_CHECK_MISMATCH)
		error = "HOST KEY MISMATCH for " + host + ": the server's key (" + fp + ") is not the one in " +
		        file + ". Someone may be intercepting the connection, or the server was "
		        "reinstalled. Not connecting.";
	else
		error = "unknown host key for " + host + " (" + fp + "). Connect once with ssh to check "
		        "and accept it, then try again.";
	return false;
}

// Tries every key the ssh-agent holds.
static bool AgentLogin(LIBSSH2_SESSION *s, const String& user)
{
	LIBSSH2_AGENT *agent = libssh2_agent_init(s);
	if(!agent)
		return false;
	bool ok = false;
	if(libssh2_agent_connect(agent) == 0 && libssh2_agent_list_identities(agent) == 0) {
		libssh2_agent_publickey *id = nullptr, *prev = nullptr;
		while(!ok && libssh2_agent_get_identity(agent, &id, prev) == 0) {
			ok = libssh2_agent_userauth(agent, user, id) == 0;
			prev = id;
		}
		libssh2_agent_disconnect(agent);
	}
	libssh2_agent_free(agent);
	return ok;
}

bool SftpFs::Connect(String& error)
{
	Impl& m = *impl;
	const TreeEndpoint& e = m.e;
	String user = Nvl(e.user, GetUserName());

	m.sock = ConnectTcp(e.host, e.port, error);
	if(m.sock < 0)
		return false;
	m.session = libssh2_session_init();
	if(!m.session) {
		error = "cannot start an SSH session";
		return false;
	}
	libssh2_session_set_blocking(m.session, 1);
	libssh2_session_set_timeout(m.session, TIMEOUT_MS);
	// --compress: zlib on the SSH connection; the other end decompresses on its own.
	if(m.compress)
		libssh2_session_flag(m.session, LIBSSH2_FLAG_COMPRESS, 1);
	if(libssh2_session_handshake(m.session, m.sock) != 0) {
		error = m.Err("SSH handshake with " + e.host + " failed");
		return false;
	}
	if(!CheckHostKey(m.session, e.host, e.port, m.known_hosts, error))
		return false;

	// Login with keys only, in the order ssh uses: the given key, ssh-agent, then the
	// usual key files. No passwords: passwordless SSH must be set up beforehand.
	Vector<String> tried;
	bool ok = false;
	auto key_login = [&](const String& key) {
		String pub = key + ".pub";
		tried.Add("key " + key);
		return libssh2_userauth_publickey_fromfile(m.session, user, FileExists(pub) ? ~pub : nullptr,
		                                           key, "") == 0;
	};
	if(e.key_file.GetCount())
		ok = key_login(e.key_file);
	if(!ok && GetEnv("SSH_AUTH_SOCK").GetCount()) {
		tried.Add("ssh-agent");
		ok = AgentLogin(m.session, user);
	}
	if(!ok && e.key_file.IsEmpty())
		for(const char *k : { ".ssh/id_ed25519", ".ssh/id_ecdsa", ".ssh/id_rsa" }) {
			String key = AppendFileName(GetHomeDirectory(), k);
			if(FileExists(key) && (ok = key_login(key)))
				break;
		}
	if(!ok) {
		String who = user + "@" + e.host;
		error = Format("cannot log in to %s:%d (tried: %s).\n"
		               "TreeSync needs passwordless SSH. Set up a key first:\n"
		               "  ssh-keygen -t ed25519    (if you have no key yet)\n"
		               "  ssh-copy-id %s\n"
		               "and check that \"ssh %s\" logs in without asking for a password.",
		               who, e.port, tried.GetCount() ? Join(tried, ", ") : String("no key or ssh-agent found"),
		               e.port == 22 ? who : Format("-p %d %s", e.port, who),
		               e.port == 22 ? who : Format("-p %d %s", e.port, who));
		return false;
	}

	m.sftp = libssh2_sftp_init(m.session);
	if(!m.sftp) {
		error = m.Err("cannot start SFTP on " + e.host);
		return false;
	}
	// Absolute, symlink-free root. A target that does not exist yet keeps its path;
	// a relative one is taken from the remote home folder.
	m.root = e.path.IsEmpty() ? String(".") : e.path;
	String real = m.RealPath(m.root);
	if(real.GetCount())
		m.root = real;
	else if(!m.root.StartsWith("/"))
		m.root = AppendFileName(m.RealPath("."), m.root);
	return true;
}

bool SftpFs::RootExists()
{
	LIBSSH2_SFTP_ATTRIBUTES a;
	return impl->Attrs(impl->root, true, a) && LIBSSH2_SFTP_S_ISDIR(a.permissions);
}

bool SftpFs::MakeRoot(String& error)
{
	// Create each missing folder on the way down.
	Vector<String> parts = Split(impl->root, '/');
	String path;
	for(const String& p : parts) {
		path << "/" << p;
		LIBSSH2_SFTP_ATTRIBUTES a;
		if(impl->Attrs(path, true, a) && LIBSSH2_SFTP_S_ISDIR(a.permissions))
			continue;
		if(libssh2_sftp_mkdir(impl->sftp, path, 0755) != 0) {
			error = impl->Err("cannot create " + path);
			return false;
		}
	}
	return true;
}

// Remote scanner -------------------------------------------------------------

namespace {

struct RemoteScanner {
	SftpFs::Impl&     m;
	String            id;
	const SyncFlags&  flags;
	SyncReport&       report;
	Vector<Entry>&    out;
	ScanStats&        stats;
	HashCache&        cache;
	Index<String>     open_dirs;   // real paths of folders being walked: loop guard
	std::chrono::steady_clock::time_point last_status = std::chrono::steady_clock::now();

	void Warn(const String& msg)
	{
		stats.warnings++;
		report.WhenLine("warning: " + msg);
	}

	void Status(const String& rel)
	{
		auto now = std::chrono::steady_clock::now();
		if(now - last_status < std::chrono::milliseconds(250))
			return;
		last_status = now;
		report.WhenStatus(Format("Scanning %s: %d entries, %s hashed  %s", id, stats.entries,
		                         FormatBytes(stats.hashed_bytes), rel));
	}

	String Dir(const String& path, const String& rel)
	{
		VectorMap<String, LIBSSH2_SFTP_ATTRIBUTES> items;
		LIBSSH2_SFTP_HANDLE *h = libssh2_sftp_open_ex(m.sftp, path, path.GetCount(), 0, 0,
		                                              LIBSSH2_SFTP_OPENDIR);
		if(!h)
			Warn(m.Err("cannot read folder " + path));
		else {
			char name[1024];
			LIBSSH2_SFTP_ATTRIBUTES a;
			int n;
			while((n = libssh2_sftp_readdir_ex(h, name, sizeof(name), nullptr, 0, &a)) > 0) {
				String s(name, n);
				if(!SkipName(s))
					items.Add(s, a);
			}
			if(n < 0)
				Warn(m.Err("error while reading folder " + path));
			libssh2_sftp_close_handle(h);
		}
		SortByKey(items);

		String listing;
		for(int i = 0; i < items.GetCount(); i++) {
			if(report.Stopping())
				return Null;
			const String& name = items.GetKey(i);
			LIBSSH2_SFTP_ATTRIBUTES a = items[i];
			String p = path == "/" ? "/" + name : path + "/" + name;
			Entry e;
			e.rel = rel.IsEmpty() ? name : rel + "/" + name;
			bool follow = false;
			if(LIBSSH2_SFTP_S_ISLNK(a.permissions) && flags.follow_links) {
				LIBSSH2_SFTP_ATTRIBUTES t;
				if(m.Attrs(p, true, t) && (LIBSSH2_SFTP_S_ISDIR(t.permissions) || LIBSSH2_SFTP_S_ISREG(t.permissions))) {
					a = t;
					follow = true;
				}
			}
			e.size = (a.flags & LIBSSH2_SFTP_ATTR_SIZE) ? (int64)a.filesize : 0;
			e.mtime_ns = (a.flags & LIBSSH2_SFTP_ATTR_ACMODTIME) ? (int64)a.mtime * 1000000000 : 0;
			e.mode = a.permissions & 07777;
			stats.entries++;
			Status(e.rel);

			if(LIBSSH2_SFTP_S_ISDIR(a.permissions)) {
				if(Excluded(flags.exclude_dirs, name))
					continue;
				String real = follow ? m.RealPath(p) : p;
				if(open_dirs.Find(real) >= 0) {
					Warn("symlink loop skipped: " + p);
					continue;
				}
				open_dirs.Add(real);
				e.type = 'd';
				e.size = 0;
				int at = out.GetCount();
				out.Add(e);
				String hsh = Dir(p, e.rel);
				open_dirs.Drop();
				if(IsNull(hsh))
					return Null;
				out[at].hash = hsh;
				cache.Store(out[at]);
				ListingLine(listing, 'd', hsh, name);
			}
			else if(LIBSSH2_SFTP_S_ISLNK(a.permissions)) {
				e.type = 'l';
				e.link = m.ReadLink(p);
				e.hash = SHA256String("symlink\n" + e.link);
				out.Add(e);
				cache.Store(e);
				ListingLine(listing, 'l', e.hash, name);
			}
			else if(LIBSSH2_SFTP_S_ISREG(a.permissions)) {
				if(Excluded(flags.exclude_files, name))
					continue;
				e.type = 'f';
				if(cache.Lookup(e, e.hash))
					stats.reused++;
				else {
					String error;
					int64 bytes = 0;
					if(!m.HashPath(p, e.hash, error, report.stop, &bytes, report.throttle)) {
						if(report.Stopping())
							return Null;
						Warn(error);
						continue;
					}
					stats.hashed++;
					stats.hashed_bytes += bytes;
				}
				out.Add(e);
				cache.Store(e);
				ListingLine(listing, 'f', e.hash, name);
			}
			else
				Warn("skipped special file (device, pipe or socket): " + p);
		}
		return SHA256String(listing);
	}
};

}

bool SftpFs::Scan(Db& db, const SyncFlags& flags, SyncReport& report, Vector<Entry>& out,
                  String& root_hash, ScanStats& stats, String& error)
{
	out.Clear();
	stats = ScanStats();
	HashCache cache(db, Id(), flags, true);
	RemoteScanner s{ *impl, Id(), flags, report, out, stats, cache };
	root_hash = s.Dir(impl->root, Null);
	if(IsNull(root_hash)) {
		db.Exec("COMMIT");
		error = "stopped";
		return false;
	}
	cache.Finish();
	Sort(out, [](const Entry& a, const Entry& b) { return SortKey(a.rel) < SortKey(b.rel); });
	return true;
}

bool SftpFs::Stat(const String& rel, bool follow, Entry& e, String& error)
{
	LIBSSH2_SFTP_ATTRIBUTES a;
	if(!impl->Attrs(impl->P(rel), follow, a)) {
		error = impl->Err("cannot stat " + impl->P(rel));
		return false;
	}
	e.rel = rel;
	e.type = LIBSSH2_SFTP_S_ISDIR(a.permissions) ? 'd' : LIBSSH2_SFTP_S_ISLNK(a.permissions) ? 'l' : 'f';
	e.size = (a.flags & LIBSSH2_SFTP_ATTR_SIZE) ? (int64)a.filesize : 0;
	e.mtime_ns = (a.flags & LIBSSH2_SFTP_ATTR_ACMODTIME) ? (int64)a.mtime * 1000000000 : 0;
	e.mode = a.permissions & 07777;
	e.dev = e.ino = 0;
	if(a.flags & LIBSSH2_SFTP_ATTR_UIDGID) {
		e.uid = a.uid;
		e.gid = a.gid;
	}
	if(e.type == 'l')
		e.link = impl->ReadLink(impl->P(rel));
	return true;
}

bool SftpFs::Hash(const String& rel, bool, String& hash, String& error, const std::atomic<bool> *stop)
{
	return impl->HashPath(impl->P(rel), hash, error, stop, nullptr);
}

namespace {

struct RemoteFile : TreeFile {
	SftpFs::Impl        *m = nullptr;
	LIBSSH2_SFTP_HANDLE *h = nullptr;
	String               path;

	bool ReadAt(int64 off, void *buf, int64 len, String& error) override
	{
		libssh2_sftp_seek64(h, off);
		for(int64 got = 0; got < len;) {
			ssize_t n = libssh2_sftp_read(h, (char *)buf + got, (size_t)min<int64>(len - got, 1 << 20));
			if(n <= 0) {
				error = n < 0 ? m->Err("cannot read " + path) : String("unexpected end of file " + path);
				return false;
			}
			got += n;
		}
		return true;
	}

	bool WriteAt(int64 off, const void *buf, int64 len, String& error) override
	{
		libssh2_sftp_seek64(h, off);
		for(int64 put = 0; put < len;) {
			ssize_t n = libssh2_sftp_write(h, (const char *)buf + put, (size_t)min<int64>(len - put, 1 << 20));
			if(n <= 0) {
				error = m->Err("cannot write " + path);
				return false;
			}
			put += n;
		}
		return true;
	}

	bool Truncate(int64 size, String& error) override
	{
		LIBSSH2_SFTP_ATTRIBUTES a = {};
		a.flags = LIBSSH2_SFTP_ATTR_SIZE;
		a.filesize = size;
		if(libssh2_sftp_fsetstat(h, &a) == 0)
			return true;
		error = m->Err("cannot set the size of " + path);
		return false;
	}

	void Flush() override   { libssh2_sftp_fsync(h); }   // servers without fsync@openssh.com just refuse

	bool SetAttrs(const FileAttrs& fa, String& error) override
	{
		LIBSSH2_SFTP_ATTRIBUTES a = {};
		a.flags = LIBSSH2_SFTP_ATTR_PERMISSIONS | LIBSSH2_SFTP_ATTR_ACMODTIME;
		a.permissions = fa.mode & 07777;
		a.atime = a.mtime = (unsigned long)(fa.mtime_ns / 1000000000);
		if(libssh2_sftp_fsetstat(h, &a) != 0) {
			error = m->Err("cannot set times or permissions of " + path);
			return false;
		}
		if(fa.uid >= 0) {
			LIBSSH2_SFTP_ATTRIBUTES o = {};
			o.flags = LIBSSH2_SFTP_ATTR_UIDGID;
			o.uid = (unsigned long)fa.uid;
			o.gid = (unsigned long)fa.gid;
			if(libssh2_sftp_fsetstat(h, &o) != 0) {
				error = m->Err("cannot set owner of " + path);
				return false;
			}
		}
		return true;
	}

	~RemoteFile()           { if(h) libssh2_sftp_close_handle(h); }
};

}

One<TreeFile> SftpFs::Open(const String& rel, bool write, String& error)
{
	One<TreeFile> f;
	String path = impl->P(rel);
	LIBSSH2_SFTP_HANDLE *h = libssh2_sftp_open_ex(impl->sftp, path, path.GetCount(),
		write ? LIBSSH2_FXF_READ | LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT : LIBSSH2_FXF_READ,
		0600, LIBSSH2_SFTP_OPENFILE);
	if(!h) {
		error = impl->Err("cannot open " + path);
		return f;
	}
	RemoteFile& rf = f.Create<RemoteFile>();
	rf.m = ~impl;
	rf.h = h;
	rf.path = path;
	return f;
}

bool SftpFs::SetAttrs(const String& rel, const FileAttrs& fa, String& error)
{
	String path = impl->P(rel);
	LIBSSH2_SFTP_ATTRIBUTES a = {};
	a.flags = LIBSSH2_SFTP_ATTR_PERMISSIONS | LIBSSH2_SFTP_ATTR_ACMODTIME;
	a.permissions = fa.mode & 07777;
	a.atime = a.mtime = (unsigned long)(fa.mtime_ns / 1000000000);
	if(libssh2_sftp_setstat(impl->sftp, path, &a) != 0) {
		error = impl->Err("cannot set times or permissions of " + path);
		return false;
	}
	if(fa.uid >= 0) {   // only works when the remote login is root; not an error otherwise
		LIBSSH2_SFTP_ATTRIBUTES o = {};
		o.flags = LIBSSH2_SFTP_ATTR_UIDGID;
		o.uid = (unsigned long)fa.uid;
		o.gid = (unsigned long)fa.gid;
		libssh2_sftp_setstat(impl->sftp, path, &o);
	}
	return true;
}

bool SftpFs::Mkdir(const String& rel, int mode, String& error)
{
	String path = impl->P(rel);
	if(libssh2_sftp_mkdir(impl->sftp, path, (mode & 07777) | 0700) == 0)
		return true;
	LIBSSH2_SFTP_ATTRIBUTES a;
	if(impl->Attrs(path, false, a) && LIBSSH2_SFTP_S_ISDIR(a.permissions))
		return true;
	error = impl->Err("cannot create folder " + path);
	return false;
}

int SftpFs::Remove(const String& rel, bool dir, String& error)
{
	String path = impl->P(rel);
	int rc = dir ? libssh2_sftp_rmdir(impl->sftp, path) : libssh2_sftp_unlink(impl->sftp, path);
	if(rc == 0)
		return 0;
	unsigned long fx = impl->Fx();
	if(fx == LIBSSH2_FX_NO_SUCH_FILE)
		return 0;
	// OpenSSH reports a non-empty folder as a plain failure: look for anything inside.
	if(dir && (fx == LIBSSH2_FX_DIR_NOT_EMPTY || fx == LIBSSH2_FX_FAILURE)) {
		LIBSSH2_SFTP_HANDLE *h = libssh2_sftp_open_ex(impl->sftp, path, path.GetCount(), 0, 0,
		                                              LIBSSH2_SFTP_OPENDIR);
		if(h) {
			char name[1024];
			LIBSSH2_SFTP_ATTRIBUTES a;
			bool empty = true;
			for(int n; (n = libssh2_sftp_readdir_ex(h, name, sizeof(name), nullptr, 0, &a)) > 0;)
				if(String(name, n) != "." && String(name, n) != "..")
					empty = false;
			libssh2_sftp_close_handle(h);
			if(!empty)
				return 1;
		}
	}
	error = impl->Err("cannot delete " + path);
	return -1;
}

bool SftpFs::Symlink(const String& rel, const String& target, String& error)
{
	String path = impl->P(rel);
	libssh2_sftp_unlink(impl->sftp, path);
	// OpenSSH's server takes the two SYMLINK arguments in the opposite order to the
	// SFTP draft, and libssh2 passes them through as given; so the link path goes
	// second here. (Checked against OpenSSH by TreeSyncTest.)
	if(libssh2_sftp_symlink_ex(impl->sftp, target, target.GetCount(), (char *)~path, path.GetCount(),
	                           LIBSSH2_SFTP_SYMLINK) == 0)
		return true;
	error = impl->Err("cannot create symlink " + path);
	return false;
}

bool SftpFs::Rename(const String& from, const String& to, String& error)
{
	String a = impl->P(from), b = impl->P(to);
	if(libssh2_sftp_posix_rename(impl->sftp, a, b) == 0)
		return true;
	// No posix-rename@openssh.com: plain rename cannot replace, so remove first.
	libssh2_sftp_unlink(impl->sftp, b);
	if(libssh2_sftp_rename_ex(impl->sftp, a, a.GetCount(), b, b.GetCount(),
	                          LIBSSH2_SFTP_RENAME_OVERWRITE | LIBSSH2_SFTP_RENAME_ATOMIC) == 0)
		return true;
	error = impl->Err("cannot rename " + a + " to " + b);
	return false;
}

// The server's own account files: covers local accounts, not LDAP or SSSD ones.
bool SftpFs::LoadIds(IdTable& t, String& error)
{
	for(int group = 0; group < 2; group++) {
		String path = group ? "/etc/group" : "/etc/passwd";
		LIBSSH2_SFTP_HANDLE *h = libssh2_sftp_open_ex(impl->sftp, path, path.GetCount(), LIBSSH2_FXF_READ, 0,
		                                              LIBSSH2_SFTP_OPENFILE);
		if(!h) {
			error = impl->Err("cannot read " + path + " on " + impl->e.host);
			return false;
		}
		String text;
		char buf[65536];
		for(ssize_t n; (n = libssh2_sftp_read(h, buf, sizeof(buf))) > 0;)
			text.Cat(buf, (int)n);
		libssh2_sftp_close_handle(h);
		t.Parse(group, text);
	}
	return true;
}

bool SftpFs::List(const String& rel, Vector<FolderEntry>& out, String& error)
{
	Impl& m = *impl;
	String path = m.P(rel);
	LIBSSH2_SFTP_HANDLE *h = libssh2_sftp_open_ex(m.sftp, path, path.GetCount(), 0, 0, LIBSSH2_SFTP_OPENDIR);
	if(!h) {
		error = m.Err("cannot read folder " + path);
		return false;
	}
	char name[1024];
	LIBSSH2_SFTP_ATTRIBUTES a;
	int n;
	while((n = libssh2_sftp_readdir_ex(h, name, sizeof(name), nullptr, 0, &a)) > 0) {
		String s(name, n);
		if(s == "." || s == "..")
			continue;
		FolderEntry& f = out.Add();
		f.name = s;
		f.is_link = LIBSSH2_SFTP_S_ISLNK(a.permissions);
		f.is_dir = LIBSSH2_SFTP_S_ISDIR(a.permissions);
		f.type = f.is_dir ? 'd' : f.is_link ? 'l' : LIBSSH2_SFTP_S_ISREG(a.permissions) ? 'f' : 's';
		f.size = (a.flags & LIBSSH2_SFTP_ATTR_SIZE) ? (int64)a.filesize : 0;
		f.mtime_ns = (a.flags & LIBSSH2_SFTP_ATTR_ACMODTIME) ? (int64)a.mtime * 1000000000 : 0;
		f.mode = a.permissions & 07777;
		if(a.flags & LIBSSH2_SFTP_ATTR_UIDGID) {
			f.uid = a.uid;
			f.gid = a.gid;
		}
		if(f.is_link) {   // like local listings: is_dir says what the link points to
			String full = path == "/" ? "/" + s : path + "/" + s;
			LIBSSH2_SFTP_ATTRIBUTES t;
			f.is_dir = m.Attrs(full, true, t) && LIBSSH2_SFTP_S_ISDIR(t.permissions);
			f.link = m.ReadLink(full);
		}
	}
	if(n < 0)
		error = m.Err("error while reading folder " + path);
	libssh2_sftp_close_handle(h);
	return n >= 0;
}

String SftpFs::RootPath() const
{
	return impl->root;
}

bool SftpFs::MoveOut(const String& rel, const String& dest, String& moved_to, String& error)
{
	Impl& m = *impl;
	LIBSSH2_SFTP_ATTRIBUTES a;
	String from = m.P(rel);
	if(!m.Attrs(from, false, a)) {
		if(m.Fx() == LIBSSH2_FX_NO_SUCH_FILE) {   // already moved (a resumed run)
			moved_to.Clear();
			return true;
		}
		error = m.Err("cannot stat " + from);
		return false;
	}
	String path;
	for(const String& p : Split(GetFileFolder(dest), '/')) {
		path << "/" << p;
		// Another copy thread may create the same folder at the same moment: check again.
		if(!(m.Attrs(path, true, a) && LIBSSH2_SFTP_S_ISDIR(a.permissions)) &&
		   libssh2_sftp_mkdir(m.sftp, path, 0755) != 0 &&
		   !(m.Attrs(path, true, a) && LIBSSH2_SFTP_S_ISDIR(a.permissions))) {
			error = m.Err("cannot create folder " + path);
			return false;
		}
	}
	String cand = dest;
	for(int n = 1; m.Attrs(cand, false, a); n++)
		cand = dest + "." + AsString(n);
	if(libssh2_sftp_posix_rename(m.sftp, from, cand) != 0) {
		error = m.Err("cannot move " + from + " to " + cand) +
		        " (if " + GetFileFolder(dest) + " is on a different disk, use --safe-delete-dir on the same disk, or --no-safe-delete)";
		return false;
	}
	moved_to = cand;
	return true;
}

bool SftpFs::CanChown() const
{
	return impl->e.user == "root";
}

String SftpFs::Name() const
{
	return impl->e.user + "@" + impl->e.host;
}

// SFTP setstat follows symlinks, so a symlink's own owner cannot be set: callers
// only pass files and folders.
bool SftpFs::Chown(const String& rel, int64 uid, int64 gid, String& error)
{
	LIBSSH2_SFTP_ATTRIBUTES a = {};
	a.flags = LIBSSH2_SFTP_ATTR_UIDGID;
	a.uid = (unsigned long)uid;
	a.gid = (unsigned long)gid;
	if(libssh2_sftp_setstat(impl->sftp, impl->P(rel), &a) == 0)
		return true;
	error = impl->Err("cannot set owner of " + impl->P(rel));
	return false;
}

bool SftpFs::HardLink(const String&, const String& rel, String& error)
{
	error = "SFTP cannot create hard links: " + rel;
	return false;
}
