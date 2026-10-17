# U++'s allocator clashes with ThreadSanitizer at link time

**Found:** 2026-10-17, building `TreeSyncTest` with clang 21 and `-fsanitize=thread`.
**Status:** worked around with U++'s `USEMALLOC` flag (`./build.sh --compiler clang --tsan`).

U++ Core ships its own memory allocator and replaces the global `operator new` and `operator delete`. ThreadSanitizer replaces them too, so the link fails with duplicate definitions in U++'s `heap.o`:

```
ld: .../upp.out/Core/CLANG26-TSAN.Debug.Debug_Full.Shared/heap.o: in function `operator new(unsigned long)':
ld: .../heap.o: in function `operator delete(void*)':
```

Building with the U++ flag `USEMALLOC` (`umk ... +USEMALLOC`) switches U++ to the system allocator, and the TSan build links and runs. The same applies to AddressSanitizer and other sanitizers that replace the allocator.

With that, the full test suite passes under ThreadSanitizer, including the 8-thread copy, stop and resume test, with no data races reported.
