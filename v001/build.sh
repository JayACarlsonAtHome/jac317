#!/usr/bin/env bash
#
# TreeSync build. Builds with ONE selected compiler, like jac313's --compiler:
#
#   ./build.sh --compiler clang            # clang++ 21+, bm/CLANG26.bm (default)
#   ./build.sh --compiler gcc              # gcc-toolset-15 g++, bm/GCC15.bm
#   ./build.sh --compiler gcc --release    # optimized build
#   ./build.sh --compiler gcc --test       # also build and run the core tests
#   ./build.sh --compiler clang --tsan     # build and run the tests under ThreadSanitizer
#   ./build.sh --compiler gcc --clean      # compile everything, not only what changed
#                                          # (fair build times when comparing compilers)
#
# Packages: core/TreeSyncCore (engine, no GUI), core/TreeSyncCli (the engine in a
# terminal), gui/TreeSync (GUI on top), core/TreeSyncTest (tests of the core).
# Output: v001/out/<compiler>/treesync, treesync-cli, treesync-test (gitignored).
# Both build methods pass -std=c++26; U++'s stock ones pass -std=c++17.
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
COMPILER=clang
MODE=-bsH
TEST=0
TSAN=0
CLEAN=0

while [ $# -gt 0 ]; do
  case "$1" in
    --compiler) COMPILER="${2:-}"; shift 2 ;;
    --release)  MODE=-brsH; shift ;;
    --test)     TEST=1; shift ;;
    --tsan)     TSAN=1; shift ;;
    --clean)    CLEAN=1; shift ;;
    -h|--help)  sed -n '3,17p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

[ "$CLEAN" = 1 ] && MODE="${MODE}a"   # umk -a: rebuild all

. "$HERE/tools.sh"

# The compiler: gcc 15+ or clang 21+, wherever this machine has it. The build methods
# in bm/ name RHEL's paths; a copy with the compiler found here goes in out/bm/.
case "$COMPILER" in
  clang) CXX=$(find_clang); SRC_BM="$HERE/bm/CLANG26.bm" ;;
  gcc)   CXX=$(find_gcc);   SRC_BM="$HERE/bm/GCC15.bm" ;;
  *) echo "--compiler must be gcc or clang" >&2; exit 2 ;;
esac
[ -n "$CXX" ] || { echo "no $COMPILER new enough (gcc 15+ / clang 21+); run ./bootstrap.sh" >&2; exit 1; }
mkdir -p "$HERE/out/bm"
local_bm() {   # local_bm SOURCE.bm -> out/bm/SOURCE.bm with COMPILER = the one found
  local out="$HERE/out/bm/$(basename "$1")"
  sed "s|^COMPILER = .*|COMPILER = \"$CXX\";|" "$1" > "$out"
  echo "$out"
}
BM=$(local_bm "$SRC_BM")

read -r UMK UPPSRC <<< "$(find_upp)"
[ -n "${UMK:-}" ] && [ -n "${UPPSRC:-}" ] || { echo "U++ (umk + uppsrc) not found; run ./bootstrap.sh" >&2; exit 1; }

# ThreadSanitizer: clang only; U++'s own allocator replaces operator new, which
# TSan also intercepts, so USEMALLOC switches U++ to the system allocator.
if [ "$TSAN" = 1 ]; then
  [ "$COMPILER" = clang ] || { echo "--tsan needs --compiler clang" >&2; exit 2; }
  OUT="$HERE/out/clang-tsan"
  mkdir -p "$OUT"
  echo "Building TreeSyncTest with $CXX + ThreadSanitizer"
  "$UMK" "$HERE/core,$HERE/gui,$UPPSRC" TreeSyncTest "$(local_bm "$HERE/bm/CLANG26-TSAN.bm")" $MODE +USEMALLOC "$OUT/treesync-test"
  TSAN_OPTIONS="halt_on_error=1" "$OUT/treesync-test"
  exit $?
fi

OUT="$HERE/out/$COMPILER"
mkdir -p "$OUT"
ASSEMBLY="$HERE/core,$HERE/gui,$UPPSRC"

# The programs are always built, so --test never leaves them older than the tests.
echo "Building TreeSyncCli with $CXX"
"$UMK" "$ASSEMBLY" TreeSyncCli "$BM" $MODE "$OUT/treesync-cli"
echo "Building TreeSync with $CXX"
"$UMK" "$ASSEMBLY" TreeSync "$BM" $MODE "$OUT/treesync"
if [ "$TEST" = 1 ]; then
  echo "Building TreeSyncTest with $CXX"
  "$UMK" "$ASSEMBLY" TreeSyncTest "$BM" $MODE "$OUT/treesync-test"
  "$OUT/treesync-test"
fi
