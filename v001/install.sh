#!/usr/bin/env bash
# install.sh: puts the built TreeSync where root runs it. You build as yourself (no
# root needed); most syncs need root, and root must not run programs you can change
# (anything running as you could swap them and then run as root). So the release
# programs are copied to a system folder, owned by root. The GCC and the Clang build
# go side by side; install one or both (TreeSyncInstall has a button for each):
#
#   /usr/local/bin/treesync-root-gcc, treesync-home-gcc, treesync-cli-gcc     the GCC build
#   /usr/local/bin/treesync-root-clang, treesync-home-clang, treesync-cli-clang   the Clang one
#   /usr/local/bin/treesync-cli        a link to treesync-cli-gcc (or -clang when only it is
#                                      there), for the terminal
#   /usr/local/share/doc/treesync/pdf/     the manual, for the Help menu
#   /usr/local/share/doc/treesync/INSTALLED-gcc, INSTALLED-clang   which build, commit, when
#   /usr/local/share/applications/treesync-root-gcc.desktop, ...   TreeSyncRoot-GCC,
#        TreeSyncHome-GCC, TreeSyncRoot-Clang, TreeSyncHome-Clang in the desktop's app
#        list, in the TreeSyncApp folder (category X-TreeSyncApp; setup/app-folder.sh
#        makes the folder)
#   /usr/share/polkit-1/actions/org.treesync.run-as-root.policy   TreeSyncRoot's runs
#        ask for the password once per window, and a new window within a few minutes
#        does not ask again (polkit reads
#        actions only from /usr/share; skipped for a --prefix of your own)
#
# Owned by root:root, programs mode 755, like every program in /usr/bin: only root can
# change them; anyone may start them, but syncing as root still needs sudo or pkexec.
# TreeSyncRoot syncs as root, for an administrator (root, or wheel / sudo / admin): it
# runs every sync through pkexec, with the installed treesync-cli of its own compiler.
# TreeSyncHome syncs as the user, inside their home, for anyone. treesync-cli started
# by an administrator starts itself again through sudo (not with --home, which
# TreeSyncHome passes).
#
# The installed copy only runs: TreeSyncBuild and TreeSyncInstall stay in the source
# tree. TreeSyncInstall runs this script through pkexec.
#
#   ./install.sh                         # both release builds (asks for your sudo password)
#   ./install.sh --compiler gcc          # only the GCC one (or clang); the other stays as it is
#   ./install.sh --dry-run               # only say what it would do
#   ./install.sh --uninstall             # remove everything it installed
#   ./install.sh --uninstall --compiler clang   # remove only the Clang build
#   ./install.sh --prefix DIR            # another place (default /usr/local)
#
# Run it again after each build you want root to use. Build first: TreeSyncBuild with
# Type Release, or ./verify.sh, leaves the release programs in out/<compiler>/.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
COMPILERS=""
PREFIX=/usr/local
DRY=0
UNINSTALL=0
ARGS=("$@")
while [ $# -gt 0 ]; do
  case "$1" in
    --compiler)  case "${2:-}" in gcc|clang) COMPILERS="$2" ;; both) COMPILERS="gcc clang" ;;
                 *) echo "--compiler must be gcc, clang or both" >&2; exit 2 ;; esac
                 shift 2 ;;
    --prefix)    PREFIX="${2:-}"; [ -n "$PREFIX" ] || { echo "--prefix needs a folder" >&2; exit 2; }; shift 2 ;;
    --dry-run)   DRY=1; shift ;;
    --uninstall) UNINSTALL=1; shift ;;
    -h|--help)   sed -n '2,40p' "$0"; exit 0 ;;
    *) echo "unknown option: $1 (see --help)" >&2; exit 2 ;;
  esac
done
[ -n "$COMPILERS" ] || COMPILERS="gcc clang"

BIN="$PREFIX/bin"
DOC="$PREFIX/share/doc/treesync"
APPS="$PREFIX/share/applications"
POLKIT=/usr/share/polkit-1/actions
POLICY="$POLKIT/org.treesync.run-as-root.policy"
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
say() { [ "$DRY" = 1 ] || echo "$*"; }
remove() { for f in "$@"; do [ -e "$f" ] || [ -L "$f" ] || continue; run rm -f "$f"; say "removed $f"; done; }
OWNER=()
[ "$(id -u)" = 0 ] && OWNER=(-o root -g root)
tag() { [ "$1" = gcc ] && echo GCC || echo Clang; }

# What a compiler's install consists of.
files_of() {   # files_of COMPILER
  local c="$1"
  echo "$BIN/treesync-root-$c $BIN/treesync-home-$c $BIN/treesync-cli-$c $DOC/INSTALLED-$c" \
       "$APPS/treesync-root-$c.desktop $APPS/treesync-home-$c.desktop"
}
# Before the GCC and Clang builds went side by side there was one of each, without the
# compiler in the name (and before TreeSyncRoot / TreeSyncHome, one GUI: treesync).
OLD="$BIN/treesync $BIN/treesync-root $BIN/treesync-home $DOC/INSTALLED $APPS/treesync.desktop \
$APPS/treesync-root.desktop $APPS/treesync-home.desktop"

# The compilers installed once this run is done (a dry run changes nothing, so it works
# that out from what is there and what it would install or remove).
after() {
  local c
  for c in gcc clang; do
    if [[ " $COMPILERS " == *" $c "* ]]; then [ "$UNINSTALL" = 0 ] && echo "$c"
    elif [ -e "$BIN/treesync-cli-$c" ]; then echo "$c"; fi
  done
}
# treesync-cli, for the terminal: a link to the GCC build's, else the Clang build's.
link_cli() {
  local c; c=$(after | head -1)
  if [ -n "$c" ]; then
    run ln -sfn "treesync-cli-$c" "$BIN/treesync-cli"
    say "linked $BIN/treesync-cli -> treesync-cli-$c"
  else
    remove "$BIN/treesync-cli"
  fi
}

if [ "$UNINSTALL" = 1 ]; then
  for c in $COMPILERS; do remove $(files_of "$c"); done
  remove $OLD
  link_cli
  if [ -z "$(after)" ]; then   # nothing left: the shared parts go too
    [ -d "$DOC" ] && run rm -rf "$DOC" && say "removed $DOC"
    [ -e "$POLICY" ] && [ -w "$POLKIT" -o "$DRY" = 1 ] && remove "$POLICY"
  fi
  exit 0
fi

# The release programs, built as you.
for c in $COMPILERS; do
  out="$HERE/out/$c"
  for f in treesync-root treesync-home treesync-cli; do
    [ -x "$out/$f" ] || { echo "no $out/$f: build first (TreeSyncBuild, Type Release; or ./build.sh --compiler $c --release)" >&2; exit 1; }
  done
  type=$(cat "$out/.build-type" 2>/dev/null || echo unknown)
  [ "$type" = release ] || { echo "$out holds a $type build: install a release build (./build.sh --compiler $c --release)" >&2; exit 1; }
done

# Each program goes in under a temporary name and is then renamed into place, so a
# TreeSync running at the time keeps its old copy and never sees a half-written file.
put() {   # put SOURCE DEST MODE
  local tmp; tmp="$(dirname "$2")/.$(basename "$2").new"
  run install "${OWNER[@]}" -m "$3" "$1" "$tmp"
  run mv -f "$tmp" "$2"
}
# An app list entry, written the same way.
desktop() {   # desktop FILE NAME PROGRAM COMMENT ICON
  local text="[Desktop Entry]
Type=Application
Name=$2
GenericName=Folder sync
Comment=$4
Exec=$BIN/$3
Icon=$5
Terminal=false
Categories=Utility;FileTools;X-TreeSyncApp;"
  if [ "$DRY" = 1 ]; then echo "would write $APPS/$1"
  else printf '%s\n' "$text" > "$APPS/.$1.new"; chmod 0644 "$APPS/.$1.new"
       mv -f "$APPS/.$1.new" "$APPS/$1"; echo "installed $APPS/$1"; fi
}
# (as root, git refuses a repository that belongs to someone else unless told it is safe)
git_() { git -c safe.directory="$(cd "$HERE/.." && pwd)" -C "$HERE" "$@"; }
commit=$(git_ rev-parse --short HEAD 2>/dev/null || echo unknown)
by=${SUDO_USER:-}   # who asked: through sudo, pkexec (TreeSyncInstall), or yourself
[ -n "$by" ] || { [ -n "${PKEXEC_UID:-}" ] && by=$(id -un "$PKEXEC_UID" 2>/dev/null); } || true
[ -n "$by" ] || by=$(id -un)
dirty=$(git_ status --porcelain 2>/dev/null | grep -q . && echo " (with uncommitted changes)" || true)

run install -d -m 0755 "$BIN" "$DOC/pdf" "$APPS"
remove $OLD
[ -L "$BIN/treesync-cli" ] || remove "$BIN/treesync-cli"   # a copy from before: a link now
for c in $COMPILERS; do
  out="$HERE/out/$c"
  for f in treesync-root treesync-home treesync-cli; do
    put "$out/$f" "$BIN/$f-$c" 0755
    say "installed $BIN/$f-$c"
  done
  desktop "treesync-root-$c.desktop" "TreeSyncRoot-$(tag $c)" "treesync-root-$c" \
          "Sync two folder trees as root (administrators), every copy checked by SHA-256; the $(tag $c) build" \
          emblem-synchronizing
  desktop "treesync-home-$c.desktop" "TreeSyncHome-$(tag $c)" "treesync-home-$c" \
          "Sync two folder trees inside your home, as you, every copy checked by SHA-256; the $(tag $c) build" \
          user-home
  info="TreeSync $(tag $c) build installed $(date '+%Y-%m-%d %H:%M:%S %z')
build:  $c release, from $out
commit: $commit$dirty
by:     $by
treesync-cli sha256: $(sha256sum "$out/treesync-cli" | cut -c1-64)"
  if [ "$DRY" = 1 ]; then echo "would write $DOC/INSTALLED-$c:"; echo "$info" | sed 's/^/  /'
  else echo "$info" > "$DOC/INSTALLED-$c"; chmod 0644 "$DOC/INSTALLED-$c"; echo; echo "$info"; echo; fi
done
link_cli

if ls "$MANUAL"/*.pdf > /dev/null 2>&1; then
  for p in "$MANUAL"/*.pdf; do put "$p" "$DOC/pdf/$(basename "$p")" 0644; done
  say "installed the manual in $DOC/pdf"
else
  echo "no manual PDFs in $MANUAL (Manual/build-pdf.sh makes them): the Help menu will say so"
fi

# polkit: TreeSyncRoot pkexecs the installed treesync-cli-<compiler> --root-session once per
# window; polkit then remembers the password for
# a few minutes (auth_admin_keep), so a new window does not ask again. One action per program.
if [ "$(id -u)" = 0 ] || [ "$DRY" = 1 ]; then
  action() {   # action COMPILER
    cat <<ACTION
  <action id="org.treesync.run-as-root.$1">
    <description>Run a TreeSync sync as root ($(tag "$1") build)</description>
    <message>TreeSync needs root to read and write every file of the trees it syncs</message>
    <defaults>
      <allow_any>auth_admin</allow_any>
      <allow_inactive>auth_admin</allow_inactive>
      <allow_active>auth_admin_keep</allow_active>
    </defaults>
    <annotate key="org.freedesktop.policykit.exec.path">$BIN/treesync-cli-$1</annotate>
  </action>
ACTION
  }
  policy="<?xml version=\"1.0\" encoding=\"UTF-8\"?>
<!DOCTYPE policyconfig PUBLIC \"-//freedesktop//DTD PolicyKit Policy Configuration 1.0//EN\"
 \"http://www.freedesktop.org/standards/PolicyKit/1/policyconfig.dtd\">
<policyconfig>
  <vendor>TreeSync</vendor>
$(action gcc)
$(action clang)
</policyconfig>"
  if [ "$DRY" = 1 ]; then echo "would write $POLICY"
  else printf '%s\n' "$policy" > "$POLICY.new" && chmod 0644 "$POLICY.new" && mv -f "$POLICY.new" "$POLICY" &&
       echo "installed $POLICY"; fi
else
  echo "not root: no polkit action (each TreeSyncRoot window will ask for the password)"
fi
