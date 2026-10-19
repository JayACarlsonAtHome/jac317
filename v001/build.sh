#!/usr/bin/env bash
#
# TreeSync build. Builds with ONE selected compiler, like jac313's --compiler:
#
#   ./build.sh --compiler clang            # clang++ 21+, bm/CLANG26.bm (default)
#   ./build.sh --compiler gcc              # gcc-toolset-15 g++, bm/GCC15.bm
#   ./build.sh --compiler gcc --release    # optimized build
#   ./build.sh --compiler gcc --test       # also build and run the core tests
#   ./build.sh --compiler clang --tsan     # build and run the tests under ThreadSanitizer
#   ./build.sh --compiler gcc --asan       # ... under AddressSanitizer + UBSan (gcc or clang),
#                                          # then treesync-cli --bench under them too
#   ./build.sh --compiler gcc --valgrind   # ... under valgrind memcheck (slow)
#   ./build.sh --compiler gcc --helgrind   # ... under valgrind helgrind (data races, lock order; very slow)
#   ./build.sh --compiler gcc --clean      # compile everything, not only what changed
#                                          # (fair build times when comparing compilers)
#
# Packages: core/TreeSyncCore (engine, no GUI), core/TreeSyncCli (the engine in a
# terminal), gui/TreeSync (the sync window), shown by gui/TreeSyncRoot (syncs as root)
# and gui/TreeSyncHome (as you, inside your home), gui/TreeSyncBuild (builds and tests,
# with bootstrap's checks), gui/TreeSyncInstall (installs for root), gui/TreeSyncUi
# (what the GUIs share), core/TreeSyncTest (tests of the core).
# Output: v001/out/<compiler>/treesync-root, treesync-home, treesync-cli, treesync-build,
# treesync-install, treesync-test (gitignored).
# Both build methods pass -std=c++26; U++'s stock ones pass -std=c++17.
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
COMPILER=clang
MODE=-bsH
TEST=0
CHECK=""   # tsan, asan, valgrind or helgrind: one checked test run instead of the build
CLEAN=0

while [ $# -gt 0 ]; do
  case "$1" in
    --compiler) COMPILER="${2:-}"; shift 2 ;;
    --release)  MODE=-brsH; shift ;;
    --test)     TEST=1; shift ;;
    --tsan|--asan|--valgrind|--helgrind)
      [ -z "$CHECK" ] || { echo "only one of --tsan, --asan, --valgrind, --helgrind" >&2; exit 2; }
      CHECK="${1#--}"; shift ;;
    --clean)    CLEAN=1; shift ;;
    -h|--help)  sed -n '3,21p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

[ "$CLEAN" = 1 ] && MODE="${MODE}a"   # umk -a: rebuild all

. "$HERE/tools.sh"
trap 'give_back "$HERE/out"' EXIT   # as root: out/ stays the user's

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

# umk links a program again only when one of its objects is newer. Debug and release
# keep their objects apart but write the same program, so after a release build a
# debug build would find the newer release program "up to date" and keep it (and the
# other way round). The type of what is in an output folder is recorded there; a
# build of the other type removes the programs first, so they are linked again.
same_type() {   # same_type OUT
  local want mark="$1/.build-type"
  case "$MODE" in -br*) want=release ;; *) want=debug ;; esac
  if [ "$(cat "$mark" 2>/dev/null)" != "$want" ]; then
    rm -f "$1/treesync-root" "$1/treesync-home" "$1/treesync-cli" "$1/treesync-build" \
          "$1/treesync-install" "$1/treesync-test"
  fi
  echo "$want" > "$mark"
}

read -r UMK UPPSRC <<< "$(find_upp)"
[ -n "${UMK:-}" ] && [ -n "${UPPSRC:-}" ] || { echo "U++ (umk + uppsrc) not found; run ./bootstrap.sh" >&2; exit 1; }

# The checked test runs, each in out/<compiler>-<check>/. U++'s own allocator replaces
# operator new, which the sanitizers also intercept and which hides heap errors from
# valgrind, so USEMALLOC switches U++ to the system allocator
# (docs/findings/upp_allocator_vs_sanitizers.md).
sanitizer_bm() {   # sanitizer_bm NAME FLAGS -> out/bm/NAME.bm: this compiler's bm + FLAGS
  local out="$HERE/out/bm/$1.bm"
  sed -e "s|^COMPILER = .*|COMPILER = \"$CXX\";|" \
      -e "s|^COMMON_OPTIONS = \"\(.*\)\";|COMMON_OPTIONS = \"\1 $2 -fno-omit-frame-pointer\";|" \
      -e "s|^COMMON_LINK = \"\(.*\)\";|COMMON_LINK = \"\1 $2\";|" "$SRC_BM" > "$out"
  echo "$out"
}
if [ -n "$CHECK" ]; then
  OUT="$HERE/out/$COMPILER-$CHECK"
  mkdir -p "$OUT"
  same_type "$OUT"
  ASSEMBLY="$HERE/core,$HERE/gui,$UPPSRC"
  case "$CHECK" in
  tsan)
    # ThreadSanitizer: clang only (data races between threads).
    [ "$COMPILER" = clang ] || { echo "--tsan needs --compiler clang" >&2; exit 2; }
    echo "Building TreeSyncTest with $CXX + ThreadSanitizer"
    "$UMK" "$ASSEMBLY" TreeSyncTest "$(local_bm "$HERE/bm/CLANG26-TSAN.bm")" $MODE +USEMALLOC "$OUT/treesync-test"
    TSAN_OPTIONS="halt_on_error=1" "$OUT/treesync-test"
    ;;
  asan)
    # AddressSanitizer (out of bounds, use after free, leaks) + UndefinedBehaviorSanitizer
    # (signed overflow and the like: docs/findings/bench_int_overflow_endless_loop.md).
    # -fno-sanitize-recover: the first UB report ends the run, so the step fails.
    SAN_BM=$(sanitizer_bm "$(basename "$SRC_BM" .bm)-ASAN" "-fsanitize=address,undefined -fno-sanitize-recover=undefined")
    echo "Building TreeSyncTest and TreeSyncCli with $CXX + AddressSanitizer + UBSan"
    "$UMK" "$ASSEMBLY" TreeSyncTest "$SAN_BM" $MODE +USEMALLOC "$OUT/treesync-test"
    "$UMK" "$ASSEMBLY" TreeSyncCli "$SAN_BM" $MODE +USEMALLOC "$OUT/treesync-cli"
    export ASAN_OPTIONS="detect_leaks=1" UBSAN_OPTIONS="print_stacktrace=1"
    "$OUT/treesync-test"
    echo "treesync-cli --bench under AddressSanitizer + UBSan"
    "$OUT/treesync-cli" --bench
    echo "bench: no sanitizer reports"
    ;;
  valgrind|helgrind)
    # valgrind on the ordinary build (no special compile). memcheck also finds reads of
    # uninitialised memory, which ASan does not; helgrind finds data races and locks
    # taken in different orders, for gcc builds too (ThreadSanitizer is clang only here).
    # 20-100 times slower, one thread at a time: TREESYNC_VALGRIND tells the tests to
    # skip their checks of races between threads.
    command -v valgrind > /dev/null || { echo "valgrind not found; run ./bootstrap.sh" >&2; exit 1; }
    echo "Building TreeSyncTest with $CXX for valgrind $CHECK"
    "$UMK" "$ASSEMBLY" TreeSyncTest "$BM" $MODE +USEMALLOC "$OUT/treesync-test"
    if [ "$CHECK" = valgrind ]; then
      TREESYNC_VALGRIND=1 valgrind --tool=memcheck --error-exitcode=99 --track-origins=yes \
               --suppressions="$HERE/setup/valgrind.supp" \
               --leak-check=full --errors-for-leak-kinds=definite --show-leak-kinds=definite \
               "$OUT/treesync-test"
    else
      TREESYNC_VALGRIND=1 valgrind --tool=helgrind --error-exitcode=99 \
               --suppressions="$HERE/setup/valgrind.supp" "$OUT/treesync-test"
    fi
    ;;
  esac
  exit $?
fi

OUT="$HERE/out/$COMPILER"
mkdir -p "$OUT"
same_type "$OUT"
ASSEMBLY="$HERE/core,$HERE/gui,$UPPSRC"

# The programs are always built, so --test never leaves them older than the tests.
echo "Building TreeSyncCli with $CXX"
"$UMK" "$ASSEMBLY" TreeSyncCli "$BM" $MODE "$OUT/treesync-cli"
rm -f "$OUT/treesync"   # the one GUI before TreeSyncRoot and TreeSyncHome
for p in TreeSyncRoot:treesync-root TreeSyncHome:treesync-home TreeSyncBuild:treesync-build \
         TreeSyncInstall:treesync-install; do
  echo "Building ${p%%:*} with $CXX"
  "$UMK" "$ASSEMBLY" "${p%%:*}" "$BM" $MODE "$OUT/${p#*:}"
done
if [ "$TEST" = 1 ]; then
  echo "Building TreeSyncTest with $CXX"
  "$UMK" "$ASSEMBLY" TreeSyncTest "$BM" $MODE "$OUT/treesync-test"
  "$OUT/treesync-test"
fi
