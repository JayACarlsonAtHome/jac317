// Benchmarks of TreeSync's own hot paths, to compare what different compilers make
// of the same code (treesync-cli --bench, the Build window's compare step). Small
// and in memory: no disk or network, so only the generated code is measured.
#include "TreeSyncCore.h"

#include <chrono>
#include <format>

namespace {

using Clock = std::chrono::steady_clock;

// Best of 3 rounds, in seconds.
template <class F>
double Best(F f)
{
	double best = 1e9;
	for(int round = 0; round < 3; round++) {
		auto t0 = Clock::now();
		f();
		best = min(best, std::chrono::duration<double>(Clock::now() - t0).count());
	}
	return best;
}

}

String BenchFacts()
{
	String r;
	// SHA-256, the work of every scan and every verified copy: 64 MiB in 1 MiB slices.
	const int SLICE = 1 << 20, SLICES = 64;
	Buffer<byte> buf(SLICE);
	for(int i = 0; i < SLICE; i++)
		buf[i] = (byte)(i * 2654435761u >> 24);
	String digest;
	double t = Best([&] {
		Sha256Stream sha;
		for(int i = 0; i < SLICES; i++)
			sha.Put(buf, SLICE);
		digest = sha.FinishString();
	});
	r << "bench:sha256 MiB/s\t" << std::format("{:.0f}", SLICES / t) << "\n";

	// Progress lines: one per copied file, many thousands in a run.
	const int LINES = 25000;
	CopyProgress p;
	p.index = 1234;
	p.count = 47908;
	p.name = "llvm-full_release_22_0_0/lib/clang/22/include/avx512vlbf16intrin.h";
	p.size = 7654321;
	p.done = 3456789;
	p.seconds = 2.5;
	int64 chars = 0;
	t = Best([&] {
		for(int i = 0; i < LINES; i++)
			chars += ProgressLine(p).GetCount();
	});
	r << "bench:progress lines/s\t" << std::format("{:.0f}", LINES / t) << "\n";

	// Sorting paths the way the scanner orders a tree.
	const int PATHS = 50000;
	Vector<String> paths;
	// int64: i * 104729 overflows int past i = 20505, and gcc -O3 turns that undefined
	// behaviour into an endless loop (docs/findings/bench_int_overflow_endless_loop.md).
	for(int i = 0; i < PATHS; i++)
		paths.Add(Format("dir%d/sub%d/file %d.txt", (int)(i * 7919LL % 97), (int)(i * 104729LL % 89), i));
	t = Best([&] {
		Vector<String> v = clone(paths);
		Sort(v, [](const String& a, const String& b) { return SortKey(a) < SortKey(b); });
	});
	r << "bench:sort paths/s\t" << std::format("{:.0f}", PATHS / t) << "\n";
	return r;
}
