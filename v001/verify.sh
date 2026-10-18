#!/usr/bin/env bash
# verify.sh: the full check before code leaves this machine (the pre-push hook runs it).
# For each compiler: a debug build with every test, then a release build with every
# test (the release tests also copy a 4.5 GiB sparse file, which debug builds skip as
# too slow). Stops at the first failure. Ends with release builds in out/<compiler>/,
# the programs the GUI runs.
#
#   ./verify.sh                  # gcc and clang
#   ./verify.sh --compiler gcc   # one compiler
#
# Each step's full output: out/verify/<compiler>-<type>.log. Exit 0 only when all passed.
# The sanitizer runs (build.sh --tsan, --asan, --valgrind, --helgrind) are not part of
# it: they take much longer; run them from the Build page when the threading changes.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
COMPILERS="gcc clang"
while [ $# -gt 0 ]; do
  case "$1" in
    --compiler)
      case "${2:-}" in gcc|clang) COMPILERS="$2" ;; *) echo "--compiler must be gcc or clang" >&2; exit 2 ;; esac
      shift 2 ;;
    -h|--help) sed -n '2,14p' "$0"; exit 0 ;;
    *) echo "unknown option: $1 (see --help)" >&2; exit 2 ;;
  esac
done

LOGS="$HERE/out/verify"
mkdir -p "$LOGS"
SUMMARY=""
started=$(date +%s)

# One step: build.sh with the tests; good when it exits 0 and the tests say "passed".
step() {
  local cc=$1 type=$2 log="$LOGS/$1-$2.log" t0=$(date +%s) flags="--compiler $1 --test"
  [ "$type" = release ] && flags="$flags --release"
  printf ' %-6s %-8s building and testing ... ' "$cc" "$type"
  "$HERE/build.sh" $flags > "$log" 2>&1
  local rc=$? result
  result=$(grep -E '^(passed|FAILED): [0-9]+/[0-9]+ checks' "$log" | tail -1)
  local took=$(( $(date +%s) - t0 ))
  if [ $rc -eq 0 ] && [ "${result#passed}" != "$result" ]; then
    printf 'ok   %3d s  %s\n' "$took" "$result"
    SUMMARY="$SUMMARY $(printf '%-6s %-8s ok      %s' "$cc" "$type" "$result")\n"
    return 0
  fi
  printf 'FAILED (%d s)\n' "$took"
  grep -E '^FAIL |error' "$log" | grep -v uppsrc | head -20 | sed 's/^/     /'
  SUMMARY="$SUMMARY $(printf '%-6s %-8s FAILED  %s' "$cc" "$type" "${result:-exit code $rc}")\n"
  echo " full output: $log"
  return 1
}

echo "verify: debug, then release; every test; compilers: $COMPILERS"
ok=1
for type in debug release; do
  for cc in $COMPILERS; do
    step "$cc" "$type" || { ok=0; break 2; }
  done
done

echo
printf "%b" "$SUMMARY"
echo " took $(( $(date +%s) - started )) s"
if [ $ok = 1 ]; then
  echo " verify: all good; out/ holds release builds"
  exit 0
fi
echo " verify: FAILED (git push --no-verify skips this check)"
exit 1
