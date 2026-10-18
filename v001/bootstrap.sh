#!/usr/bin/env bash
#
# TreeSync bootstrap: from a fresh machine to a built and tested TreeSync.
# Shell, because nothing can be compiled before there is a compiler.
#
#   ./bootstrap.sh                  # check, install what is missing (asks first), build, test
#   ./bootstrap.sh --dry-run        # only say what it would do
#   ./bootstrap.sh --yes            # do not ask before installing
#   ./bootstrap.sh --compiler gcc   # build with one compiler (default: both)
#   ./bootstrap.sh --no-build       # stop after the checks and installs
#   ./bootstrap.sh --upp-dir DIR    # where U++ goes if it has to be fetched (default ~/u++)
#   ./bootstrap.sh --check          # only check, quietly, one "@req" line each (the GUI's
#                                   # options page uses this); exit 1 if a required one is missing
#
#   1. Detects the Linux family (rhel, fedora, linuxmint).
#   2. Checks each requirement in setup/recipes.conf; for the missing ones shows the
#      install commands (most need sudo), asks, runs them and checks again.
#   3. U++: uses the one already there (tools.sh find_upp); otherwise fetches the latest
#      U++ release for POSIX into ~/u++ and builds its umk, no sudo needed.
#   4. Builds and tests TreeSync with each compiler (build.sh --test).
#   5. Ends with start : stop : duration per step (the j5c Results.txt layout), also
#      appended to Bootstrap-Results.txt in TreeSync's logs folder.
#
# Safe to run again: whatever is already there is left alone.
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
RECIPES="$HERE/setup/recipes.conf"
. "$HERE/tools.sh"

DRY=0; YES=0; BUILD=1; CHECK=0; COMPILERS="gcc clang"; UPP_TARGET="$HOME/u++"
while [ $# -gt 0 ]; do
  case "$1" in
    --dry-run)  DRY=1; shift ;;
    --check)    CHECK=1; shift ;;
    --yes)      YES=1; shift ;;
    --no-build) BUILD=0; shift ;;
    --compiler) case "${2:-}" in gcc|clang) COMPILERS="$2" ;; both) COMPILERS="gcc clang" ;;
                  *) echo "--compiler must be gcc, clang or both" >&2; exit 2 ;; esac; shift 2 ;;
    --upp-dir)  UPP_TARGET="${2:-}"; shift 2 ;;
    -h|--help)  sed -n '3,27p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

# --- --check: report and stop, nothing installed or built -----------------------------
# One line per requirement: @req<TAB>name<TAB>ok|missing<TAB>required|optional<TAB>description
if [ "$CHECK" = 1 ]; then
  field() { sed -n "s/^$1\.$2 *= *//p" "$RECIPES" | head -1; }
  bad=0
  for n in $(sed -n 's/^\([a-z0-9_]*\)\.desc *=.*/\1/p' "$RECIPES"); do
    req=$([ "$(field "$n" required)" = true ] && echo required || echo optional)
    # TREESYNC_PRETEND_MISSING="clang upp": report those as missing (to test the GUI's path)
    if eval "$(field "$n" check)" >/dev/null 2>&1 && [[ " ${TREESYNC_PRETEND_MISSING:-} " != *" $n "* ]]; then st=ok
    else st=missing; [ $req = required ] && bad=1; fi
    printf '@req\t%s\t%s\t%s\t%s\n' "$n" "$st" "$req" "$(field "$n" desc)"
  done
  if [ -n "$(find_upp)" ] && [[ " ${TREESYNC_PRETEND_MISSING:-} " != *" upp "* ]]; then st=ok; else st=missing; bad=1; fi
  printf '@req\tupp\t%s\trequired\tU++ (umk and uppsrc)\n' "$st"
  exit $bad
fi

# --- results table (start : stop : duration, like the j5c build Results.txt) ---------
STARTED=$(date +%s)
RESULTS=""
clock()    { date -d "@$1" +%H:%M:%S; }
duration() { local s=$(( $1 < 0 ? -$1 : $1 )) y d; y=$(( s / 31556952 )); s=$(( s - y * 31556952 ))
             d=$(( s / 86400 )); s=$(( s % 86400 ))
             printf '%03d:%03d:%02d:%02d:%02d' $y $d $(( s / 3600 )) $(( s / 60 % 60 )) $(( s % 60 )); }
dotted()   { local s="$1" w=$2; while [ ${#s} -lt $w ]; do s="$s."; done; printf '%s' "$s"; }
record() {   # record START STOP WHAT RESULT STATUS [COMMENT]
  local r; r=$(printf '%5s' "$4" | tr ' ' '.')
  local line=" $(clock "$1") : $(clock "$2") : $(duration $(( $2 - $1 ))) : $(dotted "$3" 40) : Result = $r : $(dotted "$5" 25)"
  [ -n "${6:-}" ] && line="$line : $6"
  RESULTS="$RESULTS$line"$'\n'
}
summary() {
  local end; end=$(date +%s)
  local header=" Status started on $(date -d "@$STARTED" +%F)  UTC = $(( $(date +%z | sed 's/^\([+-]\)0*\([0-9]*\)[0-9][0-9]$/\1\2/') ))"
  local table
  table="$header"$'\n\n'" Start    : Stop     : Duration         : Command / Comments"$'\n'
  table="$table HH:MM:SS : HH:MM:SS : YYY:DDD:HH:MM:SS :"$'\n'" $(printf '=%.0s' $(seq 1 127))"$'\n'
  table="$table$RESULTS $(clock "$STARTED") : $(clock "$end") : $(duration $(( end - STARTED ))) : End of bootstrap: $1"
  echo; echo "$table"
  [ "$DRY" = 1 ] && return
  local logs="${XDG_DATA_HOME:-$HOME/.local/share}/treesync/logs"
  mkdir -p "$logs"
  local file="$logs/Bootstrap-Results.txt" fresh=0
  [ -f "$file" ] || fresh=1   # before >> creates it
  { [ $fresh = 1 ] && printf '\n%s\n' "$(echo "$table" | sed -n '1,5p')"
    echo "$table" | sed -n '6,$p'; } >> "$file"
}

# --- 1. Linux family -----------------------------------------------------------------
family=unknown
if [ -r /etc/os-release ]; then
  . /etc/os-release
  id_l=$(echo "${ID:-}" | tr '[:upper:]' '[:lower:]'); like_l=$(echo "${ID_LIKE:-}" | tr '[:upper:]' '[:lower:]')
  case "$id_l" in
    rhel|centos|rocky|almalinux|ol) family=rhel ;;
    fedora) family=fedora ;;
    linuxmint|ubuntu|debian|pop) family=linuxmint ;;
    *) case "$like_l" in *rhel*|*fedora*) family=rhel ;; *ubuntu*|*debian*) family=linuxmint ;; esac ;;
  esac
fi
echo "Linux family: $family (${PRETTY_NAME:-unknown})"
[ "$(uname -s)" = Linux ] || { echo "TreeSync is Linux only for now." >&2; exit 1; }

# --- 2. requirements -------------------------------------------------------------------
field() { sed -n "s/^$1\.$2 *= *//p" "$RECIPES" | head -1; }
names=$(sed -n 's/^\([a-z0-9_]*\)\.desc *=.*/\1/p' "$RECIPES")

t0=$(date +%s)
missing=""
for n in $names; do
  if eval "$(field "$n" check)" >/dev/null 2>&1; then
    printf '  ok       %-8s %s\n' "$n" "$(field "$n" desc)"
  else
    req=$(field "$n" required)
    printf '  MISSING  %-8s %s%s\n' "$n" "$(field "$n" desc)" "$([ "$req" = true ] || echo ' (optional)')"
    missing="$missing $n"
  fi
done
record "$t0" "$(date +%s)" "Check requirements" 0 "Good" "missing:${missing:- none}"

if [ -n "$missing" ]; then
  echo; echo "To install (these change the system; most need sudo):"
  plan_ok=1
  for n in $missing; do
    cmd=$(field "$n" "$family")
    if [ -z "$cmd" ]; then echo "  $n: no recipe for '$family'; install it by hand: $(field "$n" desc)"; plan_ok=0
    else echo "  $n: $cmd"; fi
  done
  if [ "$DRY" = 1 ]; then
    echo; echo "(dry run: nothing installed)"
  else
    answer=n
    if [ "$YES" = 1 ]; then answer=y
    elif [ -t 0 ]; then printf '\nRun these now? [y/N] '; read -r answer
    else echo "(not interactive: run again with --yes, or run the commands above by hand)"; fi
    if [ "$answer" = y ] || [ "$answer" = Y ]; then
      for n in $missing; do
        cmd=$(field "$n" "$family"); [ -n "$cmd" ] || continue
        echo; echo "== $n: $cmd"
        t0=$(date +%s); bash -c ". '$HERE/tools.sh'; $cmd"; rc=$?   # recipes may use tools.sh
        if eval "$(field "$n" check)" >/dev/null 2>&1; then st=Good; else st=Failed; fi
        record "$t0" "$(date +%s)" "Install $n" $rc "$st"
      done
    fi
  fi
  # Still missing something required? Then no build.
  for n in $missing; do
    if [ "$(field "$n" required)" = true ] && ! eval "$(field "$n" check)" >/dev/null 2>&1; then
      echo; echo "Still missing: $n ($(field "$n" desc))."
      summary "stopped: requirements missing"
      exit 1
    fi
  done
fi

# --- 3. U++ ------------------------------------------------------------------------------
t0=$(date +%s)
read -r UMK UPPSRC <<< "$(find_upp)"
if [ -n "${UMK:-}" ]; then
  echo; echo "U++: $UMK with $UPPSRC"
  record "$t0" "$(date +%s)" "U++ found" 0 "Good" "$UMK, $UPPSRC"
else
  url=$(curl -s --max-time 30 https://api.github.com/repos/ultimatepp/ultimatepp/releases/latest |
        sed -n 's/.*"browser_download_url": *"\([^"]*upp-posix-[0-9]*\.tar\.xz\)".*/\1/p' | head -1)
  echo; echo "U++ (umk + uppsrc) not found. Plan, no sudo needed:"
  echo "  download ${url:-<latest upp-posix release: none found, check the network>}"
  echo "  unpack into $UPP_TARGET, then ./configure && make -f umkMakefile (builds umk)"
  [ -n "$url" ] || { record "$t0" "$(date +%s)" "Fetch U++" 1 "Failed" "no release found"; summary "stopped: no U++"; exit 1; }
  if [ "$DRY" = 1 ]; then
    echo "(dry run: not fetched)"; summary "dry run"; exit 0
  fi
  if [ -e "$UPP_TARGET" ]; then
    echo "$UPP_TARGET exists but holds no usable U++; move it away or use --upp-dir." >&2
    record "$t0" "$(date +%s)" "Fetch U++" 1 "Failed" "$UPP_TARGET in the way"; summary "stopped: no U++"; exit 1
  fi
  answer=n
  if [ "$YES" = 1 ]; then answer=y; elif [ -t 0 ]; then printf 'Fetch and build U++ now? [y/N] '; read -r answer; fi
  [ "$answer" = y ] || [ "$answer" = Y ] || { summary "stopped: no U++"; exit 1; }
  work=$(mktemp -d)
  if curl -L --fail --max-time 600 -o "$work/upp.tar.xz" "$url" && tar -C "$work" -xJf "$work/upp.tar.xz" &&
     mkdir -p "$(dirname "$UPP_TARGET")" && mv "$work/upp" "$UPP_TARGET" &&
     ( cd "$UPP_TARGET" && ./configure && make -f umkMakefile -j"$(nproc)" ); then
    rm -rf "$work"
    export UPP_DIR="$UPP_TARGET"
    read -r UMK UPPSRC <<< "$(find_upp)"
    [ -n "${UPPSRC:-}" ] && upp_fix_heap "$UPPSRC"   # the release has the heap bug too
  fi
  if [ -n "${UMK:-}" ]; then
    record "$t0" "$(date +%s)" "Fetch and build U++" 0 "Good" "$(basename "$url") -> $UPP_TARGET"
    [ "$UPP_TARGET" = "$HOME/u++" ] || echo "Note: build.sh looks in ~/u++; for $UPP_TARGET use: export UPP_DIR=$UPP_TARGET"
  else
    record "$t0" "$(date +%s)" "Fetch and build U++" 1 "Failed" "$url"
    summary "stopped: U++ did not build"; exit 1
  fi
fi

# --- 4. build and test -----------------------------------------------------------------
if [ "$DRY" = 1 ]; then
  echo; for c in $COMPILERS; do echo "would run: ./build.sh --compiler $c --test"; done
  summary "dry run"; exit 0
fi
# The pre-push hook (runs verify.sh) is kept in the repository: point git at it.
if git -C "$HERE/.." rev-parse --git-dir > /dev/null 2>&1; then
  git -C "$HERE/.." config core.hooksPath v001/setup/hooks &&
    echo "pre-push hook: v001/setup/hooks/pre-push runs ./verify.sh (git push --no-verify skips it)"
fi
if [ "$BUILD" = 0 ]; then summary "ready to build (--no-build)"; exit 0; fi
all=Good
mkdir -p "$HERE/out"   # for each build's output (a fresh checkout has no out/ yet)
for c in $COMPILERS; do
  echo; echo "== build and test with $c"
  t0=$(date +%s)
  "$HERE/build.sh" --compiler "$c" --test 2>&1 | tee "$HERE/out/bootstrap-$c.txt" 2>/dev/null || true
  rc=${PIPESTATUS[0]}
  checks=$(grep -E '^(passed|FAILED): .*checks' "$HERE/out/bootstrap-$c.txt" 2>/dev/null | tail -1)
  [ "$rc" = 0 ] && st=Good || { st=Failed; all=FAILED; }
  record "$t0" "$(date +%s)" "Build and test with $c" "$rc" "$st" "$checks"
done

# --- 5. summary --------------------------------------------------------------------------
summary "$([ $all = Good ] && echo 'all good' || echo FAILED)"
if [ $all = Good ]; then
  echo; echo "Start TreeSync:"
  for c in $COMPILERS; do echo "  $HERE/out/$c/treesync"; done
fi
[ $all = Good ]
