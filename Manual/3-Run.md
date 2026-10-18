# 3. Run

## 3.1 Start the GUI

There are two copies of TreeSync, for two jobs:

| | The installed TreeSync | The build in the source tree |
|---|---|---|
| Start it | **TreeSync** in the desktop's app list, or `treesync` | `v001/out/gcc/treesync` (or `out/clang/`) |
| Syncs as | **root** for an administrator (each run through `pkexec`); anyone else: themselves | you; root only with **Sync > Run as root** |
| Paths | any (administrator); inside their home (anyone else) | only inside your home, unless Run as root |
| Build page | none: it only runs | yes (File > Build..., and at start if ticked) |
| Comes from | `./install.sh` (see [2.6](2-Build.md)) | `./build.sh` or `./verify.sh` |

For everyday syncs use the **installed** TreeSync. The source-tree build is for
working on TreeSync itself: it starts on the Build page if **Show this page when
TreeSync starts** is ticked (click **Continue to TreeSync using GCC exe** to go on).

Always start the GUI as yourself, not with `sudo`: it is an ordinary desktop program,
and only the syncs run as root.

Options on the command line set up the window and become the saved settings:

```bash
treesync --source=/home/jay/Photos --target=/run/media/jay/Backup/Photos --mirror
```

Without options, the settings saved last time are used
(`~/.config/treesync/settings`, saved on every change).


## 3.2 The main window

```
+-------------------------------------------------------------------------------+
| File  View  Sync                     Elapsed 0:12  Total ~0:16  Done 03:34    |
+--------------------------------------+----------------------------------------+
| Disk  [ nvme0n1 ...          v]      | Disk  [ sdb ...                 v]     |
| Mount [ /home/jay            v]      | Mount [ /run/media/jay/Backup   v]     |
| Path  [ /home/jay/Photos ] [...]     | Path  [ .../Backup/Photos ] [...]      |
| [-] [x] Source: /home/jay/Photos     | [-] Target: .../Backup/Photos          |
|     [+] [x] 2024                     |     [+] 2024                           |
|     [+] [-] 2025                     |                                        |
|         [ ] notes.txt                |                                        |
+-------------------------------------------------------------------------------+
| Pre-scan: source        | Copy                    | Post-scan: target         |
| [==========      ]      | [=====           ]      | [===             ]        |
+-------------------------------------------------------------------------------+
| Copying                                  Rate       Size     Time             |
| [ 12/140] 2024/IMG_0001.jpg  [====   ]   85 MiB/s   130 MiB  0:01             |
+-------------------------------------------------------------------------------+
| log: one line per copied file, warnings, the summary                          |
+-------------------------------------------------------------------------------+
| status bar: the command line the current settings stand for                   |
+-------------------------------------------------------------------------------+
```

- **Left: the source**, **right: the target**. Each side has a **Disk** and
    **Mount** list to jump to a drive, a **Path** box (type a path and press Enter, or
    **...** to browse), and the tree below it.
- **Trees**: click [+] to open a folder, or use the + and - keys. **View > Reload
    trees** reads them again, **View > Collapse all** closes them.
- **The progress boxes** and the list of files in progress appear while a run is
    going. The upper right of the menu bar shows how long it has run, how long the
    whole run will probably take, and when it should be done.
- **The log** shows what happened; **View > Show log file** (Ctrl+L) opens the full
    log of the newest run.

Without root, local paths are limited to your home folder; the status bar says so.

**Help** opens this manual: the whole of it, or one chapter, as a PDF. **F1** opens
the chapter for the window you are in.

## 3.3 Choose what to sync: the checkboxes

Every folder and file in the **source** tree has a checkbox.

- **Check a folder**: everything below it is synced, including files added there
    later.
- **Uncheck a folder**: nothing below it is synced.
- **Uncheck one file** inside a checked folder: the rest of that folder stays checked.
- A folder with only some of its contents checked shows a **dash** (partly checked).
- The **top box** checks or unchecks the whole tree.

What is checked is saved as `--include-path` settings. Folders above a checked item
are created on the target only where something checked goes, and **Mirror never
deletes anything outside the checked paths**. Changing the source folder starts again
with everything checked. If nothing is checked, Run refuses to start.

## 3.4 Setup: mode and options

**File > Setup...** has three tabs.

**Source tree / Target tree**: the path, and for a remote machine the **Remote** box
with **Host**, **Port**, **User** and **Key file** (see 3.10).

**Flags**:

- **Mode**:
    - **Copy**: new and changed files go from source to target; nothing on the target
      is removed. Turns on *Skip older*: a newer file on the target is not overwritten.
    - **Mirror**: the target is made to match the source; files only the target has are
      removed (with safe delete: moved to `<target>.old`).
    - **Merge**: changes go both ways; a file changed on both sides since the last sync
      is a conflict and is left alone. Nothing is removed.
- **Reset to the defaults** of Copy, Mirror or Merge.
- **Owners**: don't copy / by name (normal) / by number. Setting an owner needs root.
- **Switches**: click any option to see what it does in the help box. The important
    ones: *Safe delete* (on; keep it on), *Hard links*, *Follow links*, *Skip older /
    newer / changed*, *Keep extra*, *Wipe target first*, *Low priority*.
- **Numbers**: threads (default 4), retries, chunk size, bandwidth limit, how long to
    wait for a lost network, time tolerance.
- **Include / exclude**: file and folder names with `*` and `?`, separated by `;`.
    A blank box filters nothing; exclude wins over include.

## 3.5 Dry run, then run

1. **Sync > Dry run** (Shift+F9): lists what a run would do (`copy`, `link`,
   `delete`, `attrs`, `mkdir`) and changes nothing. Read it.
2. **Sync > Run** (F9): asks for confirmation, saying what the mode will do and where
   removed and replaced files go, then runs.

While it runs:

- **Pre-scan: source** walks the source. **Copy** shows the bytes copied of the
    total. **Post-scan: target** checks each target folder for files to remove (Mirror).
- Each file is written to `NAME.treesync-part`, read back, checked against the
    source's SHA-256, given the source's permissions, times, owner and extended
    attributes, and only then renamed into place.
- A file whose content is the same but whose permissions or time changed gets only
    its attributes set (an `attrs` step), without copying.

At the end a popup says how it went and how long it took. **Sync > Stop** (Esc) stops
after the current file or chunk.

## 3.6 Stop and resume

A stopped run (Stop, Ctrl+C in a terminal, a crash, a reboot, an unplugged disk) is
**resumed by running it again with the same settings**. Every step was recorded in
the database, so it continues where it was. A big file continues at its last good
chunk: on resume, every chunk already copied is read back and checked first, and from
the first damaged one the file is copied again.

To start over instead, use `--fresh` on the command line.

## 3.7 Checking and attributes

- **Sync > Scan only (SHA-256 check)** (F7): changes nothing. Lists both trees, reads
    and hashes **every file on both sides** afresh, and compares permissions, times,
    owners and extended attributes. Each file's log line ends in `same`, `DIFFERS`,
    `missing` (not on the target) or `failed`. Use it after a big copy to prove the
    target is right.
- **Sync > Sync attributes** (F8): sets only the attributes that differ (mode, time,
    owner, extended attributes) on the target, for files of the same size and for
    folders. No content is copied.
- **Sync > Clear .old folders...**: deletes the safe-delete folders (`<target>.old`)
    for good, after a confirmation that says how many files and bytes. Folders made by
    a root run need **Run as root**. On the command line, without `--no-dry-run` it
    only counts; its result says *Counted, not deleted*, *Good* (deleted) or *Nothing
    to clear*.

## 3.8 History, logs and AI export

- **View > History...** (Ctrl+H): every run, newest first, with its counts (folders,
    copied, deleted, attributes set, failed, conflicts, bytes). Failed runs are red.
    Below:
    - **Actions**: what the selected run did, failures first.
    - **SHA-256 checks**: every checked file under a line naming its folder, with the
      source and target SHA-256 side by side, whether they match, and how the
      attributes compare. Mismatches first, in red; attribute differences in orange.
    - **Show log**, **Export logs to AI.md**, **Export SQL to AI.md**.
- **Logs**: each run writes `logs/YYYY-MM-DD_HHMMSS.log` next to the database. It
    starts with a header; every line has its start, stop and duration to the
    microsecond.
- **View > Export newest run for AI**: writes the run as Markdown files into
    `logs/ai/` (a guide, problems, slowest copies, throughput, the logs in full; or the
    database contents), ready to give to an AI to analyse.

## 3.9 Running as root

A sync often meets files your user cannot read or replace (other users' files,
system files), so the installed TreeSync **syncs as root for an administrator**: root,
or a member of `wheel` (`sudo` or `admin` on Debian, Ubuntu and Mint), the users who
may use sudo. A tool that can overwrite any file as root is as powerful as root, so
nobody else gets that: **anyone else runs the installed TreeSync as themselves**,
inside their own home, with no password asked (the Sync menu shows *Run as root
(administrators only)*, greyed out).

For an administrator:

- **The installed GUI** runs every sync through `pkexec`. The first run asks for your
    password; polkit then remembers it for a few minutes. **Sync > Runs as root
    (installed)** is shown ticked and cannot be turned off. Any path may be chosen.
- **The installed `treesync-cli`**, started from a terminal by an administrator, starts
    itself again through `sudo`: `treesync-cli --source=... --target=...` runs as root
    (sudo asks for your password). `--help` does not need it.
- **The source-tree build** syncs as you, limited to your home folder. Ticking
    **Sync > Run as root (pkexec)** makes its runs use the **installed**
    `treesync-cli`, which only root can change. If that is not this build, the log says
    so (`File > Install for root` updates it); if nothing is installed, it warns that it
    runs a copy your user could change.

A root run keeps its own database and logs under `/root/.local/share/treesync/`,
separate from yours. To resume a root run, run it as root again.

**History and logs of root runs.** The GUI's **History** and **Show log file** read
*your* database and logs, and root's are not readable by you. To look at a root run:
`sudo ls /root/.local/share/treesync/logs/`, or have root export it:
`treesync-cli --export-ai-logs` (the installed one runs as root by itself; it writes into
root's `logs/ai/`). Note that `sudo treesync-cli` does not work on RHEL: sudo does not
search `/usr/local/bin`; the installed `treesync-cli` needs no sudo in front.

## 3.10 Remote trees (SFTP)

Either side can be on another machine over SSH. One-time setup:

```bash
ssh-keygen -t ed25519            # if you have no key yet
ssh-copy-id jay@server           # passwordless login
ssh jay@server                   # once, to check and accept the host key
```

TreeSync logs in with keys only, never passwords. In the GUI: **File > Setup >
Target tree**, tick **Remote**, and fill in Host, Port, User (and Key file if not the
usual one). On the command line: `--target-host=server --target-user=jay`.

If the network drops, the run waits (default up to 30 minutes, *Wait for the
network*), shows **NETWORK UNAVAILABLE**, and carries on when the machine is back.

**Checking copies on the server.** When the login may run commands and the server has
`sha256sum`, the server hashes its own files: copies are checked there and remote
scans hash there, so nothing is read back over the network. The log says which way
it works (`server hashes its own files` or `files are read over SFTP`). An SFTP-only
account still works, just with twice the traffic for a verified copy.

## 3.11 The command line

`treesync-cli` takes the same options the GUI saves. **It does a dry run unless you
give `--no-dry-run`.** The examples use the installed `treesync-cli` (it runs as root);
in the source tree it is `v001/out/gcc/treesync-cli` (it runs as you).

```bash
# What would a mirror do?
treesync-cli --source=/home/jay/Photos --target=/run/media/jay/Backup/Photos --mirror

# Do it
treesync-cli --source=/home/jay/Photos --target=/run/media/jay/Backup/Photos --mirror --no-dry-run

# Copy only new and changed files, 8 threads, at low priority
treesync-cli --source=/data --target=/backup/data --copy --threads=8 --low-priority --no-dry-run

# Only one folder and one file of the source
treesync-cli --source=/data --target=/backup/data --include-path=projects/2026 --include-path=notes.txt --no-dry-run

# To a remote machine
treesync-cli --source=/data --target-host=server --target-user=jay --target=/backup/data --no-dry-run

# Prove a copy: hash both sides, change nothing
treesync-cli --source=/data --target=/backup/data --scan-only

# Set only differing permissions and times
treesync-cli --source=/data --target=/backup/data --sync-attrs --no-dry-run

# Delete <target>.old for good (without --no-dry-run it only counts)
treesync-cli --source=/data --target=/backup/data --clear-old --no-dry-run
```

`treesync-cli --help` lists every option. In a terminal it shows one line per copied
file, `[  6/90] name  100% | 85.3 MiB/s | 130.9 MiB | 00m01s`, and Ctrl+C stops it
(press twice to quit at once).

**Exit codes**: 0 in sync (or, for Scan only, the trees are the same); 1 failures,
conflicts or differences; 2 could not run (bad options, a path refused); 3 stopped.

**Database views** for looking at results directly:

```bash
sqlite3 ~/.local/share/treesync/treesync.db
```
```sql
SELECT run, started, state, copied, failed FROM run_summary ORDER BY run DESC LIMIT 10;
SELECT * FROM failures WHERE run = 42;
SELECT path, file_name, problem FROM hash_mismatches WHERE run = 42;
SELECT path, file_name, attr_diff, result FROM attr_mismatches WHERE run = 42;
```
