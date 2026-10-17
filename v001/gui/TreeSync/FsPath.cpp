// Path handling that survives long paths and awkward names (spaces, UTF-8).
//
// - ToFs / FromFs convert between U++ String (UTF-8) and std::filesystem::path
//   through u8string, so non-ASCII names survive on Windows too. On Windows, long
//   absolute paths get the \\?\ prefix, which lifts the 260-character MAX_PATH limit.
// - ListFolder on Linux opens a folder one component at a time with openat, so a
//   path longer than PATH_MAX (4096 bytes) can still be listed.
// - Paths are never passed through a shell, so spaces need no quoting here.
#include "TreeSync.h"

#ifdef PLATFORM_POSIX
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <climits>
#include <cstring>
#endif

namespace fs = std::filesystem;

fs::path ToFs(const String& p)
{
	std::u8string u(reinterpret_cast<const char8_t *>(p.Begin()), p.GetCount());
#ifdef PLATFORM_WIN32
	fs::path fp = fs::path(u).make_preferred();
	std::wstring w = fp.native();
	if(fp.is_absolute() && w.size() >= 248 && !w.starts_with(L"\\\\?\\")) {
		if(w.starts_with(L"\\\\"))
			w = L"\\\\?\\UNC\\" + w.substr(2);   // \\server\share\...
		else
			w = L"\\\\?\\" + w;
	}
	return fs::path(w);
#else
	return fs::path(u);
#endif
}

String FromFs(const fs::path& p)
{
	std::u8string u = p.u8string();
	String s(reinterpret_cast<const char *>(u.data()), (int)u.size());
#ifdef PLATFORM_WIN32
	if(s.StartsWith("\\\\?\\UNC\\"))
		s = "\\\\" + s.Mid(8);
	else if(s.StartsWith("\\\\?\\"))
		s = s.Mid(4);
#endif
	return s;
}

#ifdef PLATFORM_POSIX

// Opens dir for reading. Short paths go straight to open(); longer ones are walked
// one component at a time, O_PATH for the folders on the way (needs only search
// permission, like the kernel's own path walk) and O_RDONLY for the last one.
static int OpenDirLong(const String& dir)
{
	if(dir.GetCount() < PATH_MAX)
		return open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);

	Vector<String> parts = Split(dir, '/');
	int fd = open(dir.StartsWith("/") ? "/" : ".", O_PATH | O_DIRECTORY | O_CLOEXEC);
	for(int i = 0; fd >= 0 && i < parts.GetCount(); i++) {
		int flags = (i == parts.GetCount() - 1 ? O_RDONLY : O_PATH) | O_DIRECTORY | O_CLOEXEC;
		int next = openat(fd, parts[i], flags);
		int err = errno;
		close(fd);
		errno = err;
		fd = next;
	}
	return fd;
}

bool ListFolder(const String& dir, Vector<FolderEntry>& out, String& error)
{
	int fd = OpenDirLong(dir);
	DIR *d = fd >= 0 ? fdopendir(fd) : nullptr;
	if(!d) {
		error = strerror(errno);
		if(fd >= 0)
			close(fd);
		return false;
	}
	while(dirent *e = readdir(d)) {
		if(strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
			continue;
		struct stat st;
		FolderEntry& f = out.Add();
		f.name = e->d_name;
		f.is_link = fstatat(dirfd(d), e->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(st.st_mode);
		f.is_dir = fstatat(dirfd(d), e->d_name, &st, 0) == 0 && S_ISDIR(st.st_mode);   // follows links
	}
	closedir(d);
	return true;
}

#else

bool ListFolder(const String& dir, Vector<FolderEntry>& out, String& error)
{
	std::error_code ec;
	for(const fs::directory_entry& e : fs::directory_iterator(ToFs(dir), ec)) {
		std::error_code ec2;
		FolderEntry& f = out.Add();
		f.name = FromFs(e.path().filename());
		f.is_link = e.is_symlink(ec2);
		f.is_dir = e.is_directory(ec2);
	}
	if(ec)
		error = ec.message();
	return !ec;
}

#endif

// Tidies a path typed or pasted into a path field. Spaces at either end are kept:
// they are legal (if horrible) in file names. Handles, in order:
//   trailing newline from a paste, "quoted" or 'quoted' paths,
//   file:// URIs from file managers (%20 etc.), ~ for the home directory, and
//   shell-escaped spaces (/my\ dir) when the literal path does not exist.
String CleanPath(String p)
{
	while(p.GetCount() && (*p.Last() == '\n' || *p.Last() == '\r'))
		p.Trim(p.GetCount() - 1);
	if(p.GetCount() >= 2 && (p[0] == '"' || p[0] == '\'') && *p.Last() == p[0])
		p = p.Mid(1, p.GetCount() - 2);
	if(p.StartsWith("file://"))
		p = UrlDecode(p.Mid(7));
	if(p == "~" || p.StartsWith("~/"))
		p = GetHomeDirectory() + p.Mid(1);
	if(p.Find("\\ ") >= 0) {
		std::error_code ec;
		String unescaped;
		for(int i = 0; i < p.GetCount(); i++)
			if(p[i] == '\\' && i + 1 < p.GetCount())
				unescaped.Cat(p[++i]);
			else
				unescaped.Cat(p[i]);
		if(!fs::exists(ToFs(p), ec) && fs::exists(ToFs(unescaped), ec))
			p = unescaped;
	}
	return p;
}
