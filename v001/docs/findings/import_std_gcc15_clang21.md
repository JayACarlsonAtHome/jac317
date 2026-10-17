# `import std;` works with gcc 15 and with clang 21 on libstdc++ 15

**Found:** 2026-10-17, RHEL 9: gcc-toolset-15 (g++ 15.2.1) and clang 21.1.8, both on
gcc-toolset-15's libstdc++ 15.
**Status:** works with both; measured by `./compare.sh` (the Build window's Compare).

jac313 (v001/docs/Modules.md) used `import std;` with gcc only, through CMake's native
std-module support, and kept clang on textual includes. Here clang 21 builds libstdc++'s
own std module and uses it, with no libc++ needed.

## gcc 15.2

gcc 15.2 has no `--compile-std-module` option yet (`unrecognized command-line option`),
but it ships the module source, `bits/std.cc`, which `-fsearch-include-path` finds:

```
g++ -std=c++26 -fmodules -fsearch-include-path -c bits/std.cc -o std.o   # once: gcm.cache/std.gcm
g++ -std=c++26 -fmodules program.cpp std.o -o program
```

## clang 21.1 on libstdc++ 15

clang has no libc++ std module on this machine (`std.cppm` is not installed), but it can
precompile libstdc++'s `bits/std.cc` as a module interface:

```
GI=--gcc-install-dir=/opt/rh/gcc-toolset-15/root/usr/lib/gcc/x86_64-redhat-linux/15
clang++ -std=c++26 $GI -Wno-reserved-module-identifier --precompile -x c++-module \
        /opt/rh/gcc-toolset-15/root/usr/include/c++/15/bits/std.cc -o std.pcm          # once
clang++ -std=c++26 $GI -fmodule-file=std=std.pcm program.cpp std.pcm -o program
```

`-Wno-reserved-module-identifier` silences the warning that `std` is a reserved module
name (it is reserved for exactly this). The `.pcm` is large: about 33 MiB.

## Measured

`v001/probe/import_std.cpp` (format, print, filesystem, chrono, map, ranges, strings),
compiled with `#include` and with `import std;`, debug, no optimization:

| | gcc 15.2 | clang 21.1 |
|---|---|---|
| std module, built once | 1.84 s | 1.22 s |
| program with `#include` | 0.83 s | 0.71 s |
| program with `import std;` | 0.58 s | 0.27 s |

Once the std module exists, each file that uses it compiles faster, clang's about 2.6
times. Whether that pays off depends on how many files there are: the module costs
1-2 s up front. TreeSync itself is built by U++'s umk, which does not build modules, so
the program stays on `#include`; the probe shows what the switch would give.
