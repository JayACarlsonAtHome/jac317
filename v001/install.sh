#!/usr/bin/env bash
# install.sh: puts the built TreeSync where root runs it. You build as yourself (no
# root needed); most syncs need root, and root must not run programs you can change
# (anything running as you could swap them and then run as root). So the release
# programs are copied to a system folder, owned by root:
#
#   /usr/local/bin/treesync, /usr/local/bin/treesync-cli
#   /usr/local/share/doc/treesync/pdf/     the manual, for the Help menu
#   /usr/local/share/doc/treesync/INSTALLED   which build, from which commit, when
#   /usr/local/share/applications/treesync.desktop   TreeSync in the desktop's app list
#   /usr/share/polkit-1/actions/org.treesync.run-as-root.policy   the GUI's root runs ask
#        for the password once every few minutes, not at every run (polkit reads
#        actions only from /usr/share; skipped for a --prefix of your own)
#
# Owned by root:root, programs mode 755, like every program in /usr/bin: only root can
# change them; anyone may start them, but syncing as root still needs sudo or pkexec.
# The installed TreeSync syncs as root for an administrator (root, or wheel / sudo /
# admin): the GUI runs every sync through pkexec, and treesync-cli started by one starts
# itself again through sudo. Anyone else runs it as themselves, inside their own home.
#
# The installed copy only runs: it has no Build page (there is no source next to it).
# The GUI's "Run as root" runs the installed treesync-cli.
#
#   ./install.sh                     # the gcc release build (asks for your sudo password)
#   ./install.sh --compiler clang    # the clang one
#   ./install.sh --dry-run           # only say what it would do
#   ./install.sh --uninstall         # remove what it installed
#   ./install.sh --prefix DIR        # another place (default /usr/local)
#
# Run it again after each build you want root to use. Build first: ./verify.sh (or
# ./build.sh --compiler gcc --release) leaves the release programs in out/<compiler>/.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
COMPILER=gcc
PREFIX=/usr/local
DRY=0
UNINSTALL=0
ARGS=("$@")
while [ $# -gt 0 ]; do
  case "$1" in
    --compiler)  case "${2:-}" in gcc|clang) COMPILER="$2" ;; *) echo "--compiler must be gcc or clang" >&2; exit 2 ;; esac
                 shift 2 ;;
    --prefix)    PREFIX="${2:-}"; [ -n "$PREFIX" ] || { echo "--prefix needs a folder" >&2; exit 2; }; shift 2 ;;
    --dry-run)   DRY=1; shift ;;
    --uninstall) UNINSTALL=1; shift ;;
    -h|--help)   sed -n '2,22p' "$0"; exit 0 ;;
    *) echo "unknown option: $1 (see --help)" >&2; exit 2 ;;
  esac
done

BIN="$PREFIX/bin"
DOC="$PREFIX/share/doc/treesync"
APPS="$PREFIX/share/applications"
POLKIT=/usr/share/polkit-1/actions
POLICY="$POLKIT/org.treesync.run-as-root.policy"
OUT="$HERE/out/$COMPILER"
MANUAL="$HERE/../Manual/pdf"

# Root is needed to write a system folder (not for --dry-run, nor for a --prefix of
# your own, which then stays yours).
need_root() {
  [ "$DRY" = 1 ] && return 1
  [ "$(id -u)" = 0 ] && return 1
  local d="$PREFIX"
  while [ ! -e "$d" ]; do d=$(dirname "$d"); done
  [ ! -w "$d" ] || { [ -e "$BIN" ] && [ ! -w "$BIN" ]; }
}
if need_root; then
  echo "install.sh needs root to write $PREFIX; running it again through sudo"
  exec sudo "$0" "${ARGS[@]}"
fi
run() { if [ "$DRY" = 1 ]; then echo "would: $*"; else "$@"; fi; }
OWNER=()
[ "$(id -u)" = 0 ] && OWNER=(-o root -g root)

if [ "$UNINSTALL" = 1 ]; then
  for f in "$BIN/treesync" "$BIN/treesync-cli"; do
    [ -e "$f" ] && run rm -f "$f" && { [ "$DRY" = 1 ] || echo "removed $f"; }
  done
  [ -d "$DOC" ] && run rm -rf "$DOC" && { [ "$DRY" = 1 ] || echo "removed $DOC"; }
  for f in "$APPS/treesync.desktop" "$POLICY"; do
    [ -e "$f" ] && [ -w "$(dirname "$f")" -o "$DRY" = 1 ] && run rm -f "$f" && { [ "$DRY" = 1 ] || echo "removed $f"; }
  done
  exit 0
fi

# The release programs, built as you.
for f in treesync treesync-cli; do
  [ -x "$OUT/$f" ] || { echo "no $OUT/$f: build first (./verify.sh, or ./build.sh --compiler $COMPILER --release)" >&2; exit 1; }
done
type=$(cat "$OUT/.build-type" 2>/dev/null || echo unknown)
[ "$type" = release ] || { echo "$OUT holds a $type build: install a release build (./build.sh --compiler $COMPILER --release)" >&2; exit 1; }

# Each program goes in under a temporary name and is then renamed into place, so a
# TreeSync running at the time keeps its old copy and never sees a half-written file.
put() {   # put SOURCE DEST MODE
  local tmp; tmp="$(dirname "$2")/.$(basename "$2").new"
  run install "${OWNER[@]}" -m "$3" "$1" "$tmp"
  run mv -f "$tmp" "$2"
}
run install -d -m 0755 "$BIN" "$DOC/pdf"
for f in treesync treesync-cli; do
  put "$OUT/$f" "$BIN/$f" 0755
  [ "$DRY" = 1 ] || echo "installed $BIN/$f"
done
if ls "$MANUAL"/*.pdf > /dev/null 2>&1; then
  for p in "$MANUAL"/*.pdf; do put "$p" "$DOC/pdf/$(basename "$p")" 0644; done
  [ "$DRY" = 1 ] || echo "installed the manual in $DOC/pdf"
else
  echo "no manual PDFs in $MANUAL (Manual/build-pdf.sh makes them): the Help menu will say so"
fi

# (as root, git refuses a repository that belongs to someone else unless told it is safe)
git_() { git -c safe.directory="$(cd "$HERE/.." && pwd)" -C "$HERE" "$@"; }
# TreeSync in the desktop's list of applications.
run install -d -m 0755 "$APPS"
desktop="[Desktop Entry]
Type=Application
Name=TreeSync
GenericName=Folder sync
Comment=Sync two folder trees as root, every copy checked by SHA-256
Exec=$BIN/treesync
Icon=emblem-synchronizing
Terminal=false
Categories=Utility;FileTools;"
if [ "$DRY" = 1 ]; then echo "would write $APPS/treesync.desktop"
else printf '%s\n' "$desktop" > "$APPS/.treesync.desktop.new"; chmod 0644 "$APPS/.treesync.desktop.new"
     mv -f "$APPS/.treesync.desktop.new" "$APPS/treesync.desktop"; echo "installed $APPS/treesync.desktop"; fi

# polkit: pkexec of the installed treesync-cli asks once, then remembers it for a few
# minutes (auth_admin_keep) instead of asking at every run.
if [ "$(id -u)" = 0 ] || [ "$DRY" = 1 ]; then
  policy='<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE policyconfig PUBLIC "-//freedesktop//DTD PolicyKit Policy Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/PolicyKit/1/policyconfig.dtd">
<policyconfig>
  <vendor>TreeSync</vendor>
  <action id="org.treesync.run-as-root">
    <description>Run a TreeSync sync as root</description>
    <message>TreeSync needs root to read and write every file of the trees it syncs</message>
    <defaults>
      <allow_any>auth_admin</allow_any>
      <allow_inactive>auth_admin</allow_inactive>
      <allow_active>auth_admin_keep</allow_active>
    </defaults>
    <annotate key="org.freedesktop.policykit.exec.path">'"$BIN/treesync-cli"'</annotate>
  </action>
</policyconfig>'
  if [ "$DRY" = 1 ]; then echo "would write $POLICY"
  else printf '%s\n' "$policy" > "$POLICY.new" && chmod 0644 "$POLICY.new" && mv -f "$POLICY.new" "$POLICY" &&
       echo "installed $POLICY"; fi
else
  echo "not root: no polkit action (the GUI's root runs will ask for the password every time)"
fi

commit=$(git_ rev-parse --short HEAD 2>/dev/null || echo unknown)
dirty=$(git_ status --porcelain 2>/dev/null | grep -q . && echo " (with uncommitted changes)" || true)
info="TreeSync installed $(date '+%Y-%m-%d %H:%M:%S %z')
build:  $COMPILER release, from $OUT
commit: $commit$dirty
by:     ${SUDO_USER:-$(id -un)}
treesync-cli sha256: $(sha256sum "$OUT/treesync-cli" | cut -c1-64)"
if [ "$DRY" = 1 ]; then echo "would write $DOC/INSTALLED:"; echo "$info" | sed 's/^/  /'
else echo "$info" > "$DOC/INSTALLED"; chmod 0644 "$DOC/INSTALLED"; echo; echo "$info"; fi
