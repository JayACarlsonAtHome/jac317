#!/usr/bin/env bash
# app-folder.sh: the TreeSyncApp folder in your app list, holding the programs:
#
#   TreeSyncBuild    build and test (with bootstrap's checks)   your entry, from this source tree
#   TreeSyncInstall  install for root, or remove                your entry, from this source tree
#   TreeSyncRoot-GCC, TreeSyncRoot-Clang   sync as root (administrators)   install.sh's entries,
#   TreeSyncHome-GCC, TreeSyncHome-Clang   sync as you, inside your home   /usr/local (one or both)
#
# It writes the entries of the first two in ~/.local/share/applications (they run from
# out/<compiler>/, so they are yours, not root's), and the folder itself: on GNOME an app
# folder (gsettings org.gnome.desktop.app-folders), on other desktops (Cinnamon, MATE,
# Xfce, KDE) a menu folder (~/.config/menus/applications-merged). Every entry carries
# the category X-TreeSyncApp, which is what both folders collect. Run as you, any time:
# bootstrap.sh and TreeSyncInstall run it.
#
#   setup/app-folder.sh [--compiler gcc|clang]   whose build the entries start (default:
#                                                gcc, else clang: the first one built)
#   setup/app-folder.sh --remove                 the two entries and the folder go
set -eu
HERE="$(cd "$(dirname "$0")/.." && pwd)"
COMPILER=""
REMOVE=0
while [ $# -gt 0 ]; do
  case "$1" in
    --compiler) COMPILER="${2:-}"; shift 2 ;;
    --remove)   REMOVE=1; shift ;;
    -h|--help)  sed -n '2,19p' "$0"; exit 0 ;;
    *) echo "unknown option: $1 (see --help)" >&2; exit 2 ;;
  esac
done
[ "$(id -u)" != 0 ] || { echo "app-folder.sh: run it as yourself, not as root" >&2; exit 2; }

ID=TreeSyncApp
CATEGORY=X-TreeSyncApp
APPS_LIST="'treesync-build.desktop', 'treesync-install.desktop', 'treesync-root-gcc.desktop', \
'treesync-home-gcc.desktop', 'treesync-root-clang.desktop', 'treesync-home-clang.desktop'"
apps="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
menus="${XDG_CONFIG_HOME:-$HOME/.config}/menus/applications-merged"
dirs="${XDG_DATA_HOME:-$HOME/.local/share}/desktop-directories"
gnome=0
gsettings list-keys org.gnome.desktop.app-folders > /dev/null 2>&1 && gnome=1
folder_schema="org.gnome.desktop.app-folders.folder:/org/gnome/desktop/app-folders/folders/$ID/"

if [ "$REMOVE" = 1 ]; then
  rm -f "$apps/treesync-build.desktop" "$apps/treesync-install.desktop" "$menus/treesyncapp.menu" "$dirs/treesyncapp.directory"
  if [ "$gnome" = 1 ]; then
    now=$(gsettings get org.gnome.desktop.app-folders folder-children)
    new=$(echo "$now" | sed -e "s/, '$ID'//; s/'$ID', //; s/'$ID'//; s/^\[\]$/@as []/")
    gsettings set org.gnome.desktop.app-folders folder-children "$new"
    gsettings reset-recursively "$folder_schema" 2>/dev/null || true
  fi
  echo "removed the TreeSyncApp folder and the TreeSyncBuild / TreeSyncInstall entries"
  exit 0
fi

if [ -z "$COMPILER" ]; then
  for c in gcc clang; do [ -x "$HERE/out/$c/treesync-build" ] && { COMPILER=$c; break; }; done
fi
mkdir -p "$apps"
entry() {   # entry FILE NAME COMMENT EXEC ICON
  printf '%s\n' "[Desktop Entry]" "Type=Application" "Name=$2" "Comment=$3" "Exec=$4" "Icon=$5" \
    "Terminal=false" "Categories=Development;$CATEGORY;" > "$apps/.$1.new"
  mv -f "$apps/.$1.new" "$apps/$1"
}
if [ -n "$COMPILER" ]; then
  out="$HERE/out/$COMPILER"
  [ -x "$out/treesync-build" ] && entry treesync-build.desktop TreeSyncBuild \
    "Build and test TreeSync from $HERE" "$out/treesync-build" applications-engineering
  [ -x "$out/treesync-install" ] && entry treesync-install.desktop TreeSyncInstall \
    "Install TreeSync for root, from $HERE" "$out/treesync-install" system-software-install
  echo "TreeSyncBuild and TreeSyncInstall are in your app list (the $COMPILER build)"
fi
rm -f "$apps/treesync.desktop"   # the one entry before the split (an old bootstrap's)

# Other desktops: an XDG menu folder that collects the category.
mkdir -p "$menus" "$dirs"
printf '%s\n' "[Desktop Entry]" "Type=Directory" "Name=$ID" "Comment=TreeSync: build, install, sync" \
  "Icon=emblem-synchronizing" > "$dirs/treesyncapp.directory"
cat > "$menus/treesyncapp.menu" <<MENU
<!DOCTYPE Menu PUBLIC "-//freedesktop//DTD Menu 1.0//EN" "http://www.freedesktop.org/standards/menu-spec/menu-1.0.dtd">
<Menu>
  <Name>Applications</Name>
  <Menu>
    <Name>$ID</Name>
    <Directory>treesyncapp.directory</Directory>
    <Include><Category>$CATEGORY</Category></Include>
  </Menu>
</Menu>
MENU

# GNOME: an app folder in the app grid, by category and by name.
if [ "$gnome" = 1 ]; then
  now=$(gsettings get org.gnome.desktop.app-folders folder-children)
  case "$now" in
    *"'$ID'"*) ;;
    "@as []"|"[]") gsettings set org.gnome.desktop.app-folders folder-children "['$ID']" ;;
    *) gsettings set org.gnome.desktop.app-folders folder-children "${now%]}, '$ID']" ;;
  esac
  gsettings set "$folder_schema" name "$ID"
  gsettings set "$folder_schema" categories "['$CATEGORY']"
  gsettings set "$folder_schema" apps "[$APPS_LIST]"
  echo "the TreeSyncApp folder is in your app grid (GNOME)"
else
  echo "the TreeSyncApp folder is in your app menu"
fi
