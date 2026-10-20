# Finds the tools TreeSync builds with; sourced by build.sh, compare.sh and bootstrap.sh.
# Each function prints what it found (or nothing) and never fails the caller.
#
#   find_gcc      a g++ that is gcc 15 or newer (gcc-toolset-15 on RHEL, g++-15, g++)
#   find_clang    a clang++ that is clang 21 or newer (clang++, clang++-21, -22, ...)
#   find_upp      "UMK UPPSRC": umk and its uppsrc, as a pair from one U++ install
#   find_std_cc   COMPILER: the std module source (bits/std.cc) of the libstdc++ it uses
#
# UPP_DIR (a U++ folder holding umk and uppsrc) is tried before the usual places;
# with UPP_ONLY=1 it is the only place (to test a fresh install next to an old one).
#
#   owner_home           as root: the home of the user TreeSync belongs to
#   give_back DIR        as root: DIR's files back to that user

major_of() {   # major version of a compiler, or 0
  "$1" -dumpversion 2>/dev/null | cut -d. -f1 | grep -E '^[0-9]+$' || echo 0
}

find_gcc() {
  for c in /opt/rh/gcc-toolset-1[5-9]/root/usr/bin/g++ g++-16 g++-15 g++; do
    p=$(command -v "$c" 2>/dev/null) || continue
    [ "$(major_of "$p")" -ge 15 ] && { echo "$p"; return; }
  done
}

find_clang() {
  for c in clang++ clang++-23 clang++-22 clang++-21; do
    p=$(command -v "$c" 2>/dev/null) || continue
    [ "$(major_of "$p")" -ge 21 ] && { echo "$p"; return; }
  done
}

# As root (sudo, pkexec or a root login) the user is the one this TreeSync folder belongs
# to: their home holds the U++ they build with (root's does not), and what a root build
# makes in out/ goes back to them, or their next build could not overwrite it.
owner_uid() {
  [ "$(id -u)" = 0 ] || return 0
  local uid; uid=$(stat -c %u "$(dirname "${BASH_SOURCE[0]}")" 2>/dev/null)
  [ -n "$uid" ] && [ "$uid" != 0 ] && echo "$uid"
}
owner_home() {
  local uid; uid=$(owner_uid)
  [ -n "$uid" ] && getent passwd "$uid" | cut -d: -f6
}
give_back() {
  local uid; uid=$(owner_uid)
  [ -n "$uid" ] && [ -d "$1" ] && chown -R "$uid" "$1" 2>/dev/null
  return 0
}

# A umk that cannot start here (built for another distribution: its libraries are
# missing, exit 127) is passed over.
runs_here() { "$1" > /dev/null 2>&1 < /dev/null; [ $? -ne 127 ]; }

find_upp() {
  local g u s home; home=$(owner_home)
  if [ "${UPP_ONLY:-0}" = 1 ]; then
    [ -x "${UPP_DIR:-}/umk" ] && [ -d "$UPP_DIR/uppsrc/Core" ] && echo "$UPP_DIR/umk $UPP_DIR/uppsrc"
    return
  fi
  for g in /run/media/jay/GFS /media/jay/GFS /mnt/GFS; do
    [ -d "$g/upp/uppsrc" ] && { GFS="$g"; break; }
  done
  for u in ${UPP_DIR:+"$UPP_DIR/umk"} ${GFS:+"$GFS/upp-bin/rhel/umk"} "$HOME/u++/umk" ${home:+"$home/u++/umk"} \
           ${GFS:+"$GFS/upp-bin/mint/umk"}; do
    [ -x "$u" ] && runs_here "$u" || continue
    for s in ${UPP_DIR:+"$UPP_DIR/uppsrc"} ${GFS:+"$GFS/upp/uppsrc"} "$HOME/u++/uppsrc" ${home:+"$home/u++/uppsrc"}; do
      [ -d "$s/Core" ] && { echo "$u $s"; return; }
    done
  done
}

find_std_cc() {
  local inc
  inc=$(echo | "$1" -std=c++26 -xc++ -E -v - 2>&1 | grep -E '^ /.*/c\+\+/[0-9]+$' | head -1 | tr -d ' ')
  [ -n "$inc" ] && [ -f "$inc/bits/std.cc" ] && echo "$inc/bits/std.cc"
}

# U++ heap fix (docs/findings/upp_heap_tls_misaligned.md): U++ keeps each thread's heap
# in a byte array that may be misaligned; gcc -O3 programs then crash at start.
#   upp_heap_fixed [UPPSRC]   true if fixed (or no U++ yet: nothing to fix)
#   upp_fix_heap   [UPPSRC]   adds alignas(Heap); keeps the original as sheap.cpp.orig
upp_sheap() {
  local s="${1:-}"
  [ -n "$s" ] || s=$(find_upp | cut -d' ' -f2)
  [ -n "$s" ] && echo "$s/Core/sheap.cpp"
}
upp_heap_fixed() {
  local f; f=$(upp_sheap "${1:-}")
  [ -z "$f" ] || ! grep -qE '^[[:space:]]*static thread_local byte sHeap__\[sizeof\(Heap\)\]' "$f"
}
upp_fix_heap() {
  local f; f=$(upp_sheap "${1:-}")
  [ -n "$f" ] || return 0
  upp_heap_fixed "${1:-}" && return 0
  cp -n "$f" "$f.orig" &&
  sed -i -E 's/^([[:space:]]*)static thread_local byte sHeap__\[sizeof\(Heap\)\]/\1alignas(Heap) static thread_local byte sHeap__[sizeof(Heap)]/' "$f" &&
  upp_heap_fixed "${1:-}"
}

# SQLite3 Multiple Ciphers warnings (docs/findings/sqlite3mc_stringop_overread.md): gcc 15
# -Wstringop-overread flags sha1_update and sqlite3Strlen30 in the amalgamation U++ bundles;
# both are false alarms. Silenced for that one file in U++'s plugin/sqlite3/lib.c, which
# includes it; gcc only (clang does not warn).
#   upp_sqlite3mc_quiet [UPPSRC]   true if done (or no U++ yet: nothing to do)
#   upp_quiet_sqlite3mc [UPPSRC]   adds the pragma; keeps the original as lib.c.orig
upp_sqlite3_lib() {
  local s="${1:-}"
  [ -n "$s" ] || s=$(find_upp | cut -d' ' -f2)
  [ -n "$s" ] && echo "$s/plugin/sqlite3/lib.c"
}
upp_sqlite3mc_quiet() {
  local f; f=$(upp_sqlite3_lib "${1:-}")
  [ -z "$f" ] || [ ! -f "$f" ] || grep -q 'Wstringop-overread' "$f"
}
upp_quiet_sqlite3mc() {
  local f; f=$(upp_sqlite3_lib "${1:-}")
  [ -n "$f" ] || return 0
  upp_sqlite3mc_quiet "${1:-}" && return 0
  cp -n "$f" "$f.orig" &&
  sed -i 's|^#include "lib/sqlite3mc_amalgamation.c"|#if defined(__GNUC__) \&\& !defined(__clang__)\n#pragma GCC diagnostic ignored "-Wstringop-overread"   // false alarms (TreeSync docs/findings/sqlite3mc_stringop_overread.md)\n#endif\n&|' "$f" &&
  upp_sqlite3mc_quiet "${1:-}"
}

# The checked test runs (build.sh --asan, --valgrind): true when both compilers have
# their AddressSanitizer + UBSan runtimes and valgrind is installed.
checks_present() {
  local g c f
  g=$(find_gcc); c=$(find_clang)
  [ -n "$g" ] && [ -n "$c" ] && command -v valgrind > /dev/null || return 1
  for f in libasan.so libubsan.so; do
    [[ "$("$g" -print-file-name=$f)" = /* ]] || return 1   # a bare name: not found
  done
  [ -e "$("$c" -print-runtime-dir)/libclang_rt.asan.a" ]
}
