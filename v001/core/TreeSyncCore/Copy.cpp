// File hashing and the --bwlimit throttle.
#include "TreeSyncCore.h"

#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <thread>

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

Throttle::Throttle(int64 bytes_per_second, const std::atomic<bool> *stop)
:	rate(bytes_per_second), tokens((double)bytes_per_second), last(std::chrono::steady_clock::now()), stop(stop)
{}

void Throttle::Account(int64 bytes)
{
	if(rate <= 0)
		return;
	auto refill = [&] {
		auto now = std::chrono::steady_clock::now();
		tokens = min((double)rate, tokens + std::chrono::duration<double>(now - last).count() * rate);
		last = now;
	};
	{
		std::lock_guard<std::mutex> lock(m);
		refill();
		tokens -= (double)bytes;
	}
	// In debt: wait (without the lock) until the shared bucket is back to zero,
	// in steps of 100 ms.
	while(!(stop && *stop)) {
		double wait;
		{
			std::lock_guard<std::mutex> lock(m);
			refill();
			if(tokens >= 0)
				return;
			wait = min(-tokens / rate, 0.1);
		}
		std::this_thread::sleep_for(std::chrono::duration<double>(wait));
	}
}
