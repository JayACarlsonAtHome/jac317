#!/usr/bin/env bash
#
# TreeSync GUI build. Builds with ONE selected compiler, like jac313's --compiler:
#
#   ./build.sh --compiler clang            # clang++ 21+, bm/CLANG26.bm (default)
#   ./build.sh --compiler gcc              # gcc-toolset-15 g++, bm/GCC15.bm
#   ./build.sh --compiler gcc --release    # optimized build
#
# Output: v001/gui/out/<compiler>/treesync (gitignored).
# Both build methods pass -std=c++26; U++'s stock ones pass -std=c++17.
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
PKG=TreeSync
COMPILER=clang
MODE=-bsH

while [ $# -gt 0 ]; do
  case "$1" in
    --compiler) COMPILER="${2:-}"; shift 2 ;;
    --release)  MODE=-brsH; shift ;;
    -h|--help)  sed -n '3,10p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

case "$COMPILER" in
  clang) BM="$HERE/bm/CLANG26.bm" ;;
  gcc)   BM="$HERE/bm/GCC15.bm" ;;
  *) echo "--compiler must be gcc or clang" >&2; exit 2 ;;
esac

# Find the GFS mount (RHEL: /run/media, Mint: /media, servers: /mnt).
GFS=""
for g in /run/media/jay/GFS /media/jay/GFS /mnt/GFS; do
  [ -d "$g/upp/uppsrc" ] && { GFS="$g"; break; }
done

# umk + uppsrc: RHEL-built umk first, then $HOME/u++, then Mint's.
UMK=""; UPPSRC=""
for u in "$GFS/upp-bin/rhel/umk" "$HOME/u++/umk" "$GFS/upp-bin/mint/umk"; do
  [ -x "$u" ] && { UMK="$u"; break; }
done
for s in "$GFS/upp/uppsrc" "$HOME/u++/uppsrc"; do
  [ -d "$s" ] && { UPPSRC="$s"; break; }
done
[ -n "$UMK" ]    || { echo "umk not found" >&2; exit 1; }
[ -n "$UPPSRC" ] || { echo "uppsrc not found" >&2; exit 1; }

OUT="$HERE/out/$COMPILER"
mkdir -p "$OUT"
echo "Building $PKG with $COMPILER ($BM)"
"$UMK" "$HERE,$UPPSRC" "$PKG" "$BM" $MODE "$OUT/treesync"
