# Our bench's `int` overflow: gcc -O3 makes an endless loop that eats all memory

**Found:** 2026-10-17, the Build window's "both, release, tests, compare, TSan" run:
gcc and clang Release builds both passed 217/217, then the compare step for gcc
locked up the whole computer (61 GB RAM + 31 GB swap used up; reboot needed).
**Status:** our bug (undefined behaviour in `Bench.cpp`), not gcc's. Fixed.

## Where

`core/TreeSyncCore/Bench.cpp`, the sort benchmark's test data:

```cpp
const int PATHS = 50000;
for(int i = 0; i < PATHS; i++)
	paths.Add(Format("dir%d/sub%d/file %d.txt", i * 7919 % 97, i * 104729 % 89, i));
```

`i * 104729` overflows `int` once `i` passes 20505 (2^31 / 104729). Signed overflow
is undefined behaviour, so gcc -O3 may assume it never happens, and it rewrites the
loop in a way that never reaches `i < PATHS` again. The loop keeps adding paths
until memory runs out. clang -O3 and gcc -O0 happen to keep the original exit test,
so only the gcc Release `treesync-cli --bench` (run by `compare.sh`) was affected.

It is not `import std`: the compare step never got to its probe (`out/gcc/probe`
kept its earlier time), and TreeSync itself does not use modules.

## How it was found

With a 4 GB cap, so the machine survives:

```
(ulimit -v 4000000; out/gcc/treesync-cli --bench)
Out of memory!
U++ allocated memory (including failed request): 4827892 KB
System allocation request: 1635954688 (0x6182b000)
```

A gcc -O3 build with symbols (`umk ... -brsH +DEBUG_FULL`) under gdb:

```
#8  Upp::Vector<Upp::String>::ReAlloc ...        a 1.6 GB Vector: ~100 million paths, not 50000
#11 Upp::Vector<Upp::String>::Add ...
#12 BenchFacts () at core/TreeSyncCore/Bench.cpp:65
```

## Fix

Multiply in 64 bits: `(int)(i * 104729LL % 89)` (and the same for 7919). After the
fix: gcc and clang `compare.sh` both finish (bench, import std probe), and the clang
Release TSan run passes 215/215.

## Lesson

Test data generators like `i * BIG_PRIME % n` overflow quietly; use `int64` or
`unsigned`. When a run can eat memory, cap it while looking into it:
`ulimit -v` for normal programs, `systemd-run --user --scope -p MemoryMax=8G` for
TSan (TSan reserves huge address space, so `ulimit -v` breaks it).
