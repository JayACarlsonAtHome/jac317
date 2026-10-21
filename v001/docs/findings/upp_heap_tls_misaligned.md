# U++'s per-thread heap is a byte array: gcc -O3 release builds crash at start

**Found:** 2026-10-17, the Build window's gcc 15.2 Release build: every program
(`treesync`, `treesync-cli --help`, `treesync-test`) dies with SIGSEGV (exit 139)
before `main`. gcc Debug, clang Debug and clang Release all work.
**Status:** U++ bug, present in 18549, 18619 and U++ master on GitHub (checked
2026-10-17; still in 18761). One-word fix, verified below. upp_basefix carries it as
patch 0001, so TreeSync built from `~/upp_basefix` has it; `tools.sh` `upp_fix_heap`
still applies it to any other U++.

## Where

`uppsrc/Core/sheap.cpp`, `MakeHeap()`:

```cpp
static thread_local byte sHeap__[sizeof(Heap)]; // zero initialization is fine for us
heap_tls__ = (Heap *)sHeap__;
```

Each thread's small-block allocator lives in a `byte` array, which only has to be
1-byte aligned, and is then used as a `Heap`. That is undefined behaviour: a `Heap` must
be aligned like a `Heap`. Where the array lands depends on how the linker lays out the
thread-local variables, so it differs between compilers, build modes and programs.

## What gcc makes of it

gcc -O3 vectorizes the loop over `empty[]` in `Heap::WorkPage` (sheap.cpp:47) with
`pcmpeqq (%rcx)`, an SSE instruction whose memory operand must be 16-byte aligned,
which gcc may assume from `Heap`'s type. The array came out at `0x7ffff7e73299` (odd),
so the first allocation, during U++'s static initialization (`AddIniInfo` from
`Log.cpp:452`), faults:

```
#0  Upp::Heap::WorkPage (this=0x7ffff7e73299, k=1) at Core/sheap.cpp:47      if(empty[i]) {
#1  Upp::Heap::AllocK (this=0x7ffff7e73299, k=1) at Core/sheap.cpp:117
...
#8  Upp::AddIniInfo (id="user_log", ...)
#11 __static_initialization_and_destruction_0 () at Core/Log.cpp:452
```

(Found with a gcc -O3 -g build relinked without umk's `-Wl,-s`.) clang and gcc -O0
have the same misaligned object but happen not to use an instruction that requires
alignment there, or the array happens to land aligned.

## Fix

```cpp
alignas(Heap) static thread_local byte sHeap__[sizeof(Heap)]; // zero initialization is fine for us
```

Verified: the same gcc 15.2 Release build with only this line changed (a patched copy
of `Core` placed ahead of uppsrc in the assembly) runs, and `treesync-test` passes.

## Reproduce

```
./build.sh --compiler gcc --release --test     # treesync-test: Segmentation fault (exit 139)
out/gcc/treesync-cli --help                    # same, before main
```
