#!/usr/bin/env bash
#
# Facts for comparing compilers, one "@cmp<TAB>key<TAB>value" line each (the Build
# window collects them into a table, gcc next to clang):
#
#   ./compare.sh --compiler gcc|clang
#
# version, the sizes of the built programs, the toolchain facts and benchmarks of the
# built treesync-cli (build it first), and the import std probe: probe/import_std.cpp
# compiled with #include and with import std, each timed, and run.
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
COMPILER=clang
while [ $# -gt 0 ]; do
  case "$1" in
    --compiler) COMPILER="${2:-}"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done
GCC_DIR=/opt/rh/gcc-toolset-15/root/usr
case "$COMPILER" in
  gcc)   CXX="$GCC_DIR/bin/g++"; FLAGS="" ;;
  clang) CXX=clang++; FLAGS="--gcc-install-dir=$GCC_DIR/lib/gcc/x86_64-redhat-linux/15" ;;
  *) echo "--compiler must be gcc or clang" >&2; exit 2 ;;
esac
OUT="$HERE/out/$COMPILER"
fact() { printf '@cmp\t%s\t%s\n' "$1" "$2"; }
secs() { local t0=$(date +%s.%N); "$@" > "$PROBE/last.txt" 2>&1; local rc=$?; ELAPSED=$(printf '%.2f' "$(echo "$(date +%s.%N) - $t0" | bc)"); return $rc; }

fact "version" "$($CXX --version | head -1)"
for p in treesync treesync-cli treesync-test; do
  [ -f "$OUT/$p" ] && fact "size: $p (MiB)" "$(echo "scale=1; $(stat -c %s "$OUT/$p") / 1048576" | bc)"
done
if [ -x "$OUT/treesync-cli" ]; then
  "$OUT/treesync-cli" --toolchain | sed 's/^/@cmp\t/'
  echo "running the benchmarks..."
  "$OUT/treesync-cli" --bench | sed 's/^/@cmp\t/'
else
  fact "toolchain" "treesync-cli not built for $COMPILER"
fi

# import std: build the std module from libstdc++'s bits/std.cc, then the probe.
PROBE="$OUT/probe"
rm -rf "$PROBE"; mkdir -p "$PROBE"; cd "$PROBE"
STD_CC="$GCC_DIR/include/c++/15/bits/std.cc"
if secs $CXX -std=c++26 $FLAGS "$HERE/probe/import_std.cpp" -o textual && ./textual > /dev/null; then
  fact "probe: #include compile (s)" "$ELAPSED"
else
  fact "probe: #include compile (s)" "failed: $(grep -m1 error last.txt)"
fi
if [ "$COMPILER" = gcc ]; then
  secs $CXX -std=c++26 -fmodules -fsearch-include-path -c bits/std.cc -o std.o; rc=$?; std_t=$ELAPSED
  [ $rc = 0 ] && secs $CXX -std=c++26 -fmodules -DTS_IMPORT_STD "$HERE/probe/import_std.cpp" std.o -o modules
else
  secs clang++ -std=c++26 $FLAGS -Wno-reserved-module-identifier --precompile -x c++-module "$STD_CC" -o std.pcm; rc=$?; std_t=$ELAPSED
  [ $rc = 0 ] && secs clang++ -std=c++26 $FLAGS -DTS_IMPORT_STD -fmodule-file=std=std.pcm "$HERE/probe/import_std.cpp" std.pcm -o modules
fi
rc=$?
if [ "$rc" = 0 ] && ./modules > /dev/null; then
  fact "probe: import std works" "yes"
  fact "probe: std module build, once (s)" "$std_t"
  fact "probe: import std compile (s)" "$ELAPSED"
else
  fact "probe: import std works" "no: $(grep -m1 -E 'error|unrecognized' last.txt)"
fi
