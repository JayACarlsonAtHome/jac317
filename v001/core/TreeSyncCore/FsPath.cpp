// Path handling that survives long paths and awkward names (spaces, UTF-8).
//
// - ToFs / FromFs convert between U++ String (UTF-8) and std::filesystem::path.
// - ListFolder on Linux opens a folder one component at a time with openat, so a
//   path longer than PATH_MAX (4096 bytes) can still be listed.
// - Paths are never passed through a shell, so spaces need no quoting here.
#include "TreeSyncCore.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <climits>
#include <cstring>

namespace fs = std::filesystem;

fs::path ToFs(const String& p)
{
	return fs::path(p.ToStd());
}

String FromFs(const fs::path& p)
{
	return String(p.native());
}

// Opens dir for reading. Short paths go straight to open(); longer ones are walked
// one component at a time, O_PATH for the folders on the way (needs only search
// permission, like the kernel's own path walk) and O_RDONLY for the last one.
int OpenDir(const String& dir)
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
	int fd = OpenDir(dir);
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
		if(fstatat(dirfd(d), e->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0) {
			f.type = S_ISDIR(st.st_mode) ? 'd' : S_ISLNK(st.st_mode) ? 'l' : S_ISREG(st.st_mode) ? 'f' : 's';
			f.size = st.st_size;
			f.mtime_ns = (int64)st.st_mtim.tv_sec * 1000000000 + st.st_mtim.tv_nsec;
			f.ctime_ns = (int64)st.st_ctim.tv_sec * 1000000000 + st.st_ctim.tv_nsec;
			f.dev = st.st_dev;
			f.ino = st.st_ino;
			f.nlink = st.st_nlink;
			f.mode = st.st_mode & 07777;
			f.uid = st.st_uid;
			f.gid = st.st_gid;
			f.is_link = S_ISLNK(st.st_mode);
			if(f.is_link) {
				char buf[PATH_MAX + 1];
				ssize_t n = readlinkat(dirfd(d), e->d_name, buf, PATH_MAX);
				f.link = String(buf, (int)max<ssize_t>(n, 0));
			}
		}
		f.is_dir = fstatat(dirfd(d), e->d_name, &st, 0) == 0 && S_ISDIR(st.st_mode);   // follows links
	}
	closedir(d);
	return true;
}

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
