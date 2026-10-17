#!/usr/bin/env bash
#
# Opens a terminal window in v001 with ./bootstrap.sh typed at the prompt, not run:
# the user reads it, may edit it, and presses Enter. Used by the GUI's options page
# when requirements are missing (the GUI never installs anything itself).
#
#   setup/fix-in-terminal.sh            # open the terminal
#   setup/fix-in-terminal.sh --which    # only say which terminal would be used
#   setup/fix-in-terminal.sh --prompt   # (inside the terminal) the prompt itself
set -u
HERE="$(cd "$(dirname "$0")/.." && pwd)"

if [ "${1:-}" = --prompt ]; then
  cd "$HERE" || exit 1
  echo "TreeSync: some requirements are missing (the options page lists them)."
  echo "The bootstrap checks again, shows what it would install and asks before it"
  echo "installs anything (most installs need your sudo password)."
  echo
  echo "Press Enter to run it, edit the command first, or Ctrl+C to leave it."
  echo
  # Typed but not run: readline puts it on the line, Enter runs it.
  if read -e -i "./bootstrap.sh" -p "$(basename "$HERE")\$ " cmd; then
    [ -n "$cmd" ] && eval "$cmd"
    echo
    echo "Done. Back in TreeSync, press 'Check again' on the options page."
  fi
  exec bash -i   # leave the terminal open
fi

# Terminals and how each runs a command: first one found wins.
self="$HERE/setup/fix-in-terminal.sh"
for t in ptyxis kgx gnome-terminal konsole xfce4-terminal mate-terminal x-terminal-emulator xterm; do
  command -v "$t" >/dev/null 2>&1 || continue
  case "$t" in
    ptyxis|kgx|gnome-terminal) run=("$t" -- bash "$self" --prompt) ;;
    xfce4-terminal|mate-terminal) run=("$t" -x bash "$self" --prompt) ;;
    *) run=("$t" -e bash "$self" --prompt) ;;
  esac
  if [ "${1:-}" = --which ]; then echo "$t"; exit 0; fi
  setsid "${run[@]}" >/dev/null 2>&1 < /dev/null &
  exit 0
done
echo "no terminal program found; run this by hand: cd $HERE && ./bootstrap.sh" >&2
exit 1
