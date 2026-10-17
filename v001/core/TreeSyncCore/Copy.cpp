// File hashing and small-file copy.
//
// CopySmall runs /usr/bin/cp (full path, no shell, no PATH lookup) once per file:
//   - the source folder is handed to cp as fd 3, and the source is named
//     /proc/self/fd/3/NAME, so the source path can be any length;
//   - cp starts inside the target folder (posix_spawn fchdir), so the target is
//     a short relative name too;
//   - names go in an argument list, so spaces and quotes need no escaping.
#include "TreeSyncCore.h"

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

extern char **environ;

bool HashAt(int dirfd, const String& name, String& hash, String& error,
            const std::atomic<bool> *stop, int64 *bytes, bool follow)
{
	int fd = openat(dirfd, name, O_RDONLY | O_CLOEXEC | (follow ? 0 : O_NOFOLLOW));
	if(fd < 0) {
		error = "cannot open " + name + ": " + strerror(errno);
		return false;
	}
	posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
	Sha256Stream sha;
	Buffer<byte> buf(1 << 20);
	int64 total = 0;
	for(;;) {
		if(stop && *stop) {
			close(fd);
			error = "stopped";
			return false;
		}
		ssize_t n = read(fd, buf, 1 << 20);
		if(n < 0 && errno == EINTR)
			continue;
		if(n < 0) {
			error = "cannot read " + name + ": " + strerror(errno);
			close(fd);
			return false;
		}
		if(n == 0)
			break;
		sha.Put(buf, (int)n);
		total += n;
	}
	close(fd);
	hash = sha.FinishString();
	if(bytes)
		*bytes = total;
	return true;
}

bool CopySmall(int src_dir, const String& name, int dst_dir, const String& part, String& error)
{
	int err_pipe[2];
	if(pipe2(err_pipe, O_CLOEXEC) != 0) {
		error = String("pipe: ") + strerror(errno);
		return false;
	}
	// dup2 onto the same number would leave close-on-exec set, so never hand over fd 3 as is.
	int src = src_dir == 3 ? fcntl(src_dir, F_DUPFD_CLOEXEC, 10) : src_dir;

	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, src, 3);
	posix_spawn_file_actions_adddup2(&fa, err_pipe[1], 2);
	posix_spawn_file_actions_addfchdir_np(&fa, dst_dir);

	String from = "/proc/self/fd/3/" + name;
	const char *argv[] = { "cp", "--preserve=all", "--reflink=auto", "--sparse=auto", "--",
	                       from, part, nullptr };
	pid_t pid;
	int rc = posix_spawn(&pid, "/usr/bin/cp", &fa, nullptr, (char **)argv, environ);
	posix_spawn_file_actions_destroy(&fa);
	close(err_pipe[1]);
	if(src != src_dir)
		close(src);
	if(rc != 0) {
		close(err_pipe[0]);
		error = String("cannot run /usr/bin/cp: ") + strerror(rc);
		return false;
	}

	String message;
	char buf[1024];
	for(ssize_t n; (n = read(err_pipe[0], buf, sizeof(buf))) != 0;)
		if(n > 0)
			message.Cat(buf, (int)n);
		else if(errno != EINTR)
			break;
	close(err_pipe[0]);

	int status = 0;
	while(waitpid(pid, &status, 0) < 0 && errno == EINTR)
		;
	if(WIFEXITED(status) && WEXITSTATUS(status) == 0)
		return true;
	message = TrimBoth(message);
	error = message.GetCount() ? message : String("cp failed");
	return false;
}
