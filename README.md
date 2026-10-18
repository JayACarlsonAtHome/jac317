# TreeSync

TreeSync keeps two directory trees in sync. The trees can be on the same machine or on different machines.

> **Use at your own risk.** TreeSync is experimental testing software. It copies, overwrites and deletes files, and can suggest commands that change user accounts. Use it at your own risk: try it on copies first and keep backups. It is provided "AS IS", without warranty of any kind, and the authors and contributors are not liable for any damage or loss of data arising from its use. See sections 7 and 8 of the Apache License 2.0 (LICENSE).

## Goals

TreeSync is three things at once:

1. **A tree sync tool**: two directory trees, compared by SHA-256, synced with a GUI that shows what is going on.
2. **A compiler proving ground**: compilers are changing fast, so TreeSync is written in current C++26 and must build cleanly and pass its tests on **both** gcc and clang. Every test run prints a toolchain report: compiler, library and which C++26 features are really there. When a compiler gets something wrong, it is pinned down with a minimal reproducer in [`v001/docs/findings/`](v001/docs/findings/).
3. **Ours to customize**: we choose how it looks and how it acts, instead of taking a framework's or another tool's defaults. Examples so far: GNU-style long options, dnf-style progress lines, `+`/`-` keys on the trees, Wayland first, and the home-only limit without root.

## Overview

- **Two trees**: a source tree and a target tree, on one machine or on two machines.
- **SHA-256 comparison**: each file and each folder gets a SHA-256 hash. Matching hashes mean the files or folders are the same, and those parts of the tree are skipped.
- **Local SQLite cache**: hashes are stored in a local SQLite table, so a later sync only has to re-hash what has changed.
- **U++ GUI**: a desktop interface built with [U++ (Ultimate++)](https://www.ultimatepp.org/) for:
  - picking each tree by disk, mount point and path
  - setting sync options
  - viewing one or both trees side by side, with [+]/[-] to expand and collapse folders (mouse, or the + and - keys)
  - showing sync results (what matched, what was copied, updated or skipped, and any conflicts)

## How it works

1. **Scan**: walk each tree and compute SHA-256 hashes for files. A folder's hash is built from the hashes of everything inside it.
2. **Cache**: save the paths, hashes and metadata in the local SQLite database.
3. **Compare**: compare the two trees by hash. When two folders have the same hash, skip everything under them.
4. **Sync**: copy or update the files that differ, based on the options chosen in the GUI.
5. **Report**: show the results in the GUI.

## Language and build

- **C++26** (`-std=c++26`), plus U++ for the GUI.
- **Compilers**: GCC 15+ and Clang 21+. On RHEL 9 the system GCC 11 is too old; `gcc-toolset-15` is found by itself (`v001/tools.sh` looks in the usual places on RHEL, Fedora and Mint).

### Bootstrap: a fresh machine

```bash
cd v001
./bootstrap.sh              # check, install what is missing (asks first), fetch U++ if needed, build, test
./bootstrap.sh --dry-run    # only say what it would do
```

Like jac313's bootstrap: it detects the Linux family (RHEL, Fedora, Mint/Ubuntu), checks each requirement listed in [`v001/setup/recipes.conf`](v001/setup/recipes.conf) (GCC 15+, Clang 21+, GTK 3 and the libraries U++ GUIs build with, make/git/curl, for tests bc and OpenSSH, and for the checked test runs the ASan + UBSan runtimes and valgrind), shows the install commands for what is missing and runs them after a yes (most need sudo). If there is no U++ it fetches the latest U++ release for POSIX into `~/u++` and builds its `umk`, without sudo. Then it builds and tests with each compiler and ends with start : stop : duration per step, also kept in `Bootstrap-Results.txt` in the logs folder. Safe to run again. New requirements or families are a line in `recipes.conf`, not code. `./bootstrap.sh --check` only checks (a fraction of a second); the GUI's options page runs it every time it opens and shows the result under *This machine*. If something required is missing, the page opens a terminal (GNOME Terminal, Console, Ptyxis, Konsole, Xfce, MATE or xterm, whichever is there) in `v001` with `./bootstrap.sh` typed at the prompt but not run: press Enter to run it, then *Check again*. The GUI itself never installs anything.
- **Contracts**: preconditions and assertions are written with C++26 contracts where the compiler supports them (`__cpp_contracts`). Otherwise they fall back to runtime assertion checks through a small macro wrapper, so the code does not need to change once compilers catch up.

### Layout

TreeSync is a GUI on top of working code:

| Package | Role |
|---|---|
| `v001/core/TreeSyncCore` | The engine side: settings, options, paths, disks, privileges, progress lines. U++ Core only, no GUI library, so it can never depend on the GUI. |
| `v001/gui/TreeSync` | The GUI. It shows what the core is doing and passes settings to it. |
| `v001/core/TreeSyncCli` | `treesync-cli`: the engine in a terminal, no GUI. |
| `v001/core/TreeSyncTest` | Console tests of the core, including real syncs. |

### Building and testing

Before a push, `v001/verify.sh` runs everything: for gcc and clang, a debug build with all tests, then a release build with all tests (the release tests add a 4.5 GiB sparse copy). It stops at the first failure, keeps each step's output in `out/verify/`, and ends with release builds in `out/`. A git pre-push hook kept in the repository runs it (`v001/setup/hooks/pre-push`; `bootstrap.sh` sets `git config core.hooksPath v001/setup/hooks`); `git push --no-verify` skips it once.

Like jac313, TreeSync builds with one selected compiler at a time:

```bash
cd v001
./build.sh --compiler clang            # clang 21+, bm/CLANG26.bm (default)
./build.sh --compiler gcc              # gcc-toolset-15, bm/GCC15.bm
./build.sh --compiler gcc --release
./build.sh --compiler gcc --test       # build and run the core tests
./build.sh --compiler clang --tsan     # run the tests under ThreadSanitizer (data races)
./build.sh --compiler gcc --asan       # ... under AddressSanitizer + UBSan, then treesync-cli --bench too
./build.sh --compiler gcc --valgrind   # ... under valgrind memcheck (uninitialised reads; slow)
./build.sh --compiler gcc --helgrind   # ... under valgrind helgrind (data races, lock order; very slow)
```

**Checked test runs**: ThreadSanitizer (clang only), AddressSanitizer + UBSan, valgrind memcheck and helgrind (gcc and clang) each build `treesync-test` into `v001/out/<compiler>-<check>/` with U++'s `USEMALLOC`, because U++'s own allocator clashes with the sanitizers and hides heap errors from valgrind ([`upp_allocator_vs_sanitizers.md`](v001/docs/findings/upp_allocator_vs_sanitizers.md)). UBSan stops at the first report, so undefined behaviour fails the step: it names the `int` overflow behind [`bench_int_overflow_endless_loop.md`](v001/docs/findings/bench_int_overflow_endless_loop.md) in a second, where the gcc -O3 build ate all memory. Helgrind looks for the same kind of trouble as ThreadSanitizer (races, lock order) but works with gcc builds too; the page runs it once, on gcc when both compilers are chosen. Under valgrind, threads run one at a time, so the tests skip their checks of races between threads (`TREESYNC_VALGRIND`).

Output goes to `v001/out/<compiler>/treesync` (and `treesync-test`). U++'s stock build methods pass `-std=c++17` after any package options, so TreeSync ships its own build methods in `v001/bm/` that pass `-std=c++26`. A `static_assert` stops the build if C++26 is not in effect.

**In the GUI**: TreeSync opens on the compiler and test options page (*Build and test*, *Continue to TreeSync*, or *Exit*, which quits, as does closing the window; switch it off with *Show this page when TreeSync starts*, and it stays under File > Build and test..). *Continue to TreeSync using GCC exe* or *using Clang exe* starts that compiler's build of TreeSync (the newest), so you choose which compiler's program you sync with. The page does the same as the commands above, like jac313's Jac313Runner: choose gcc, clang or both, Debug or Release, the tests, ThreadSanitizer, AddressSanitizer + UBSan, valgrind and helgrind; it runs `build.sh` once per step, streams the output, and ends with start : stop : duration per step (kept in `Build-Results.txt` in the logs folder). `treesync --build-run` does a build with the saved choices without clicking (a developer aid). The page also chooses the **display**: Wayland (recommended) or X Windows / X11 (not recommended: any X11 program can read the keyboard and screen of the others, and X11 is no longer developed). It is for desktops without Wayland; with no Wayland session the choice is X11. TreeSync restarts itself once on the chosen display if it came up on the other one (`GDK_BACKEND`); File > About shows which one it runs on.

**Comparing compilers**: tick *Compare the compilers* (and *Clean build* for fair build times) and the Results screen shows gcc next to clang, rows that differ marked: build time, test results, program sizes, versions, the C++26 feature macros, benchmarks of TreeSync's own hot paths (SHA-256, progress lines, path sorting; `treesync-cli --bench`), and `import std;`, whether it works and how its compile time compares with `#include` on `v001/probe/import_std.cpp`. The facts come from `./compare.sh --compiler gcc|clang`. Both compilers here can `import std;`; see [`v001/docs/findings/import_std_gcc15_clang21.md`](v001/docs/findings/import_std_gcc15_clang21.md). Benchmarks of a Debug build measure unoptimized code; choose Release to compare what the optimizers make of it.

`treesync --screenshot=DIR` (a developer aid) draws the main window and each Setup tab into PNG files without showing anything, so the layout can be checked without a screen.

The tests build a scratch tree under `$TMPDIR` with awkward names and a folder chain deeper than `PATH_MAX`. They also check option round trips (including shell quoting through a real `sh`), the home-only limit, and the progress-line layout. The scratch tree is removed afterwards.

## Choosing what to sync

The source tree in the GUI has a checkbox on every folder and file. Checking a folder checks everything below it, including files added there later; unchecking it unchecks everything below. Unchecking one file inside a checked folder leaves the rest of that folder checked. A folder with only some of its contents checked shows a partly checked box. The top box checks or unchecks the whole tree. What is checked is saved as `--include-path` options (a checked folder is stored once, not file by file) and works together with the include and exclude names: exclude still wins. Changing the source folder starts again with everything checked.

## Seeing what happens

The point of the GUI is to see what is going on. So the engine reports each step as it happens (scan, hash, compare, copy, skip, conflict) instead of handing the whole job to a tool that only prints a summary.

- **Terminal**: one line per copied file, in the style of Fedora's dnf5, then a total:

  ```
  [  3/108] photos/2024/beach day.jpg                       100% |   12.4 MiB/s |    3.1 MiB |  00m00s
  [  4/108] music/Sigur Rós/Ágætis byrjun/01 Intro.flac      25% |    2.0 MiB/s |    1.0 MiB |  00m00s
  [108/108] Total                                           100% |   14.0 MiB/s |  300.2 MiB |  00m21s
  ```

- **File counter**: `[ 35/240+]` means the 35th file copied of 240 that may need copying: the files decided so far plus those found but not compared with the target yet (some will turn out the same and drop out). The `+` shows while the pre-scan is still finding files.
- **Thermometers**: in the terminal, the passing status line of a file in progress carries a text bar, `45% [=========           ]`. In the GUI, three boxes between the trees and the log follow a streamed run: **Pre-scan** (source entries found, of the inode estimate when known), **Copy** (bytes copied of the bytes found so far; while the pre-scan is still finding files the total can grow, and files that turn out the same as the target's drop out of it) and **Post-scan** (target folders checked, of folders found). Under them, one row with its own bar per file being copied right now, so with `--threads=4` up to four move at once. Bars move within a chunk too, about four times a second.
- **How long it took**: at the end of a run the GUI pops up how long it took, as `2 hours 05 minutes 09 seconds` and in the `YYY:DDD:HH:MM:SS` form of the j5c build results (`000:000:02:05:09`), with the start and finish times; `treesync-cli` prints the same line, so it is in the log too.
- **Log lines**: in the log file every line starts with `start : stop : duration :`, the same columns as the j5c `Results.txt`. A copied file runs from when its copy began to when it finished; lines about the whole run (pre-scan and post-scan done, Total, finished) run from the start of the run; anything else shows the moment it happened. The terminal and the GUI show the plain lines.
- **Results table**: each run's log ends with its start, stop and duration in the layout of the j5c build `Results.txt` (`14:02:58 : 16:08:07 : 000:000:02:05:09 : Mirror run 6.... : Result = ....0 : Good....`), and the same line is appended to `Results.txt` in the logs folder, one line per run.
- **Settings are kept**: the GUI saves its setup (trees, logins, mode, switches) to `~/.config/treesync/settings` on every change, written to a new file and renamed into place so a crash never leaves half a file, and loads it at the next start. The file holds the same options `treesync-cli` takes, one per line. Options given on the command line win and become the saved settings. `--dry-run` and `--fresh` are one-shot and not kept.
- **Help**: in File > Setup > Flags, clicking an option shows a plain explanation of what it does underneath (hovering shows its command-line name).
- **GUI**: Sync > Run (F9) syncs for real, after a confirmation that says what will change; Sync > Dry run (Shift+F9) only shows the list of changes. (The `--dry-run` switch is for `treesync-cli`; in the GUI the menu decides.) The GUI starts `treesync-cli` and shows its output in a log pane under the trees; Sync > Stop (Esc) stops it after the current file or chunk, and the trees reload when it ends. Warnings appear in a dialog and are also printed to the terminal.

**Restarts**: every step is recorded in the SQLite database before the next one starts, so a stopped or crashed run continues where it left off: run the same command again. Big files are copied in chunks, each chunk recorded as it finishes, so a restart continues part-way through the file instead of from the start (for example "resuming big one.bin at chunk 7 of 19"). `--fresh` starts over instead. Inode numbers identify unchanged files, so they are not read again. Details are in [`v001/docs/engine.md`](v001/docs/engine.md).

**Copying**: every file is copied by TreeSync itself, in `--chunk-mib` pieces (one piece for a small file), on `--threads` threads. Earlier versions started `/usr/bin/cp` for each small file; starting a process per file cost more than the copy itself, so it was dropped. Like `cp --preserve=all`, a local copy keeps permissions, times, owner (as root) and extended attributes (ACLs, SELinux label, file capabilities, `user.*`; best effort). A local copy also keeps sparse files sparse: all-zero 1 MiB slices are not written, they stay holes (a 400 GiB disk image that is mostly empty takes only its data on the target), and the read-back check still covers them. A file of more than one piece has each piece recorded as it finishes, so a stopped copy continues where it left off. Every copy goes to `NAME.treesync-part`, is read back and checked against the source's SHA-256 (the same value `sha256sum` prints), and only then renamed into place. `rsync` would also work, but it hides the per-file steps the GUI is meant to show.

## Long paths and awkward names

- **Long paths**: folders are opened one component at a time (`openat`), so trees deeper than `PATH_MAX` (4096 bytes) still work.
- **Spaces and other characters**: names with spaces (including leading or trailing ones), tabs, newlines, quotes, backslashes and non-ASCII characters are handled as-is. Paths never go through a shell, so nothing needs quoting. Remote transfers must keep this rule: use an SFTP library, not `ssh`/`scp` command lines.
- **Pasted paths**: the path field accepts `"quoted"` paths, `/shell\ escaped` paths, `file://` URIs, `~`, and a trailing newline from a paste.

## Running as root

TreeSync is meant to run as root, because a sync often meets files the current user cannot read or replace. Without root it still runs, after a warning, but local trees are limited to your home directory.

The GUI itself never runs as root, so it stays an ordinary Wayland client. Sync > Run as root starts `treesync-cli` through `pkexec` instead, and then paths outside home are allowed. In a terminal, run `sudo treesync-cli ...` or `pkexec treesync-cli ...`. Note that root has its own database (`/root/.local/share/treesync/treesync.db`) unless `--db` says otherwise. Paths are resolved before they are checked, so `..` and symlinks cannot step outside home. Remote trees are not limited this way; what they can reach depends on the remote login.

## Command line

TreeSync is Linux-only for now. Options are GNU-style long options: `--name`, `--name=value` or `--name value`, and `--no-name` to turn off a switch that is on by default. The GUI opens with these settings, and its status bar shows the current settings as a command line you can copy. Run `treesync --help` for the full list.

```bash
treesync --source=/data --target=/mnt/backup --mirror --threads=8 --no-dry-run
treesync --source="$HOME/my dir" --target=/backup --target-host=nas --target-user=jay --merge
```

Defaults depend on the mode. `--copy` also turns on `--skip-older`, so a newer file on the target is never overwritten by an older one; `--mirror` makes an exact copy, so the source always wins; `--merge` decides by what changed since the last sync, so the time switches do not apply to it. In the GUI, Setup > Flags > Reset to defaults has one button per mode, and options a mode ignores are greyed out.

The switches are modelled on robocopy. TreeSync adds `--merge`, which copies changes both ways and reports a conflict when the same file changed on both sides.

| Option | Meaning | robocopy |
|---|---|---|
| `--copy` | Copy new and changed files, never delete | `/E` |
| `--mirror` | Make target match source, deleting extras (default) | `/MIR` |
| `--merge` | Copy both ways; changed on both sides = conflict | — |
| `--dry-run` | Report only, change nothing (default on) | `/L` |
| `--dst-tolerance` | Allow 1-hour (DST) time shifts, give or take the time tolerance | `/DST` |
| `--hard-links` | Hard-linked files are copied once and relinked at the target (default on); `--no-hard-links` writes each name out as its own copy | |
| `--follow-links` | Write out what symlinks point to (files and folders); off by default, so symlinks are copied as symlinks | off = `/SL` |
| `--include-same` | Include same files (rehash / recopy) | `/IS` |
| `--wipe-target` | Empty the target before anything is copied: with safe delete its contents move to `<target>.old`, without it they are deleted. Refused for `/`, the home folder itself, system folders and `--merge` | |
| `--skip-older` | Skip if source is older than target | `/XO` |
| `--skip-newer` | Skip if source is newer than target | `/XN` |
| `--skip-changed` | Skip changed files (same time, new hash) | `/XC` |
| `--keep-extra` | Keep extra target files (mirror only) | `/XX` |
| `--skip-source-only` | Skip files that exist only in source | `/XL` |
| `--exclude-file=GLOB` | Skip matching files (repeatable) | `/XF` |
| `--exclude-dir=GLOB` | Skip matching folders (repeatable) | `/XD` |
| `--include-file=GLOB` | Only matching files (repeatable); e.g. `--include-file=win11.qcow2` syncs just that file, with the folders on its way | file list |
| `--include-dir=GLOB` | Only what is inside matching folders, at any depth (repeatable) | |
| `--include-path=REL` | Only this file or folder (relative to the source) and everything below it (repeatable); the GUI's checkboxes. The folders above it are made only as needed, and Mirror deletes nothing outside the checked paths | |
| `--retries=N` | Retries on a failed copy (default 3) | `/R:n` |
| `--retry-wait=N` | Seconds between retries (default 5) | `/W:n` |
| `--time-tolerance=N` | Times of the two sides N seconds apart or less count as equal (default 3; 0 = exact). Clocks and file times differ between machines, network shares and FAT disks | `/FFT` (for all systems) |
| `--threads=N` | Files copied at the same time (default 4); each thread has its own SSH connection to a remote side | `/MT:n` |
| `--big-file-mib=N` | A same-size file this big or bigger, with no saved hash, is compared with the target's copy side by side, stopping at the first difference (default 1024) | |
| `--chunk-mib=N` | Copies are made in chunks this big; a file of more than one chunk resumes at its last checked chunk (default 64) | |
| `--safe-delete` | Files a sync would delete or overwrite are moved to `<target>.old` instead, same folder layout; a name already there gets `.1`, `.2`, ... (default on; `--no-safe-delete` really deletes) | |
| `--safe-delete-dir=PATH` | Where `--safe-delete` moves target-side files (default `<target>.old`; must be on the same disk, outside both trees) | |
| `--preserve-owner` | Copy owner and group, matched by **name** (default on; needs root on the side being written) | `/COPY:O` |
| `--numeric-ids` | Copy owners by number instead of name; switches `--preserve-owner` on (owners are one choice: off, by name or by number, an Owners row in the GUI) | |
| `--bwlimit=N` | Bandwidth limit in KiB/s (0 = none): network traffic, or writes for a local copy; at most one second of burst | `/IPG:n` |
| `--low-priority` | Run at low CPU and disk priority (`nice 19`, `ionice -c2 -n7`) so other work comes first | |
| `--network-wait=N` | Seconds to wait for a lost remote side to come back before stopping (default 1800; 0 = stop at once) | |
| `--compress` | Compress SSH traffic with zlib; the other end decompresses on its own. Helps text on slow links, not media | |
| `--fresh` | Start over instead of resuming an unfinished run | |
| `--known-hosts=FILE` | SSH known_hosts file (default `~/.ssh/known_hosts`) | |
| `--db=PATH` | Database file (default `~/.local/share/treesync/treesync.db`) | |
| `--SIDE=PATH` | Tree root; SIDE is `source` or `target` | |
| `--SIDE-host`, `-user`, `-port`, `-key` | Remote login for that side | |

**Include and exclude** (the four boxes on the Flags tab): a blank list filters nothing, and every list that is filled in narrows what is synced. Patterns are file or folder names with `*` and `?`, separated by `;`. Exclude wins over include. With an include list, folders are made on the target only where something included goes (an included folder itself is always made), and Mirror deletes only what the lists would include: everything else on the target, also inside extra folders, is left alone.

There is no password login: remote sides use SSH keys only, so passwordless SSH must be set up beforehand (`ssh-copy-id user@host`, then check that `ssh user@host` does not ask for a password).

Content is compared by SHA-256. File size and time decide whether a cached hash can be reused and, in Merge mode, which side is newer, so `--time-tolerance` and `--dst-tolerance` still matter. The hash cache itself compares exact times (a file with its own earlier times), apart from DST shifts.

## Status

Working for local trees and over SFTP, with `treesync-cli` and the GUI: scan with a hash cache, Copy / Mirror / Merge, verified copies, hard links, symlinks, resumable runs and big files. Tested against a Fedora 44 server in both directions.

## History and logs

Every `treesync-cli` run (including those the GUI starts) writes its output to a log file, `logs/YYYY-MM-DD_HHMMSS.log` next to the database, and the run's row in the database records it. Log lines carry their start, stop and duration to the microsecond (`14:16:45.102938`), every field padded so the lines stay aligned. In the GUI, **View > History** (Ctrl+H) lists past runs with what each did (failed runs and actions in red, failures first) and opens a run's log; its **SHA-256 checks** tab shows, under a line naming each folder, every file's source and target SHA-256 side by side, whether they match, and how the attributes compare; content mismatches come first and in red, attribute differences in orange. **View > Show log file** (Ctrl+L) opens the newest one.

## Checking and attributes

- **Sync > Scan only (SHA-256 check)** (F7, `--scan-only`) changes nothing. It reads and hashes every file on both sides afresh (no cached hashes), compares mode, time, owner and extended attributes, and records one row per file and folder. The log lists every file whose content differs or that is missing on the target, and every attribute difference; the exit code is 1 when the trees are not the same.
- **Sync > Sync attributes** (F8, `--sync-attrs`) compares the attributes only (no hashing) and sets, on the target, just those that differ, for files of the same size and for folders (mode and owner; a folder keeps its own time). Files whose size differs are left for a sync. With `--dry-run` (the command line's default) it only lists them.
- A normal Copy or Mirror run does the same for files whose content is already equal: a `chmod` or `touch` on the source is carried over as an `attrs` step, without copying the file.
- Every copy records both sides' attributes after it is made, so `attrs_match` shows whether they were kept. Extended attributes are compared by a SHA-256 over their names and values; the SELinux label is left out (policy sets it for where the file lives).
- **Sync > Clear .old folders...** (`--clear-old`) deletes the safe-delete folders for good, after a confirmation that says how many files and bytes. Folders made by a root run need Sync > Run as root.

The database has views for looking at it directly, for example with `sqlite3 ~/.local/share/treesync/treesync.db`:

| View | One row per |
|---|---|
| `run_summary` | run: started, finished, state, source, target, folders, copied, linked, deleted, attrs_fixed, failed, conflicts, bytes, log file |
| `run_actions` | action of a run: kind, path, state, size, error |
| `failures` | failed action, with its error |
| `file_compare` | file or folder checked by a copy, a Scan only or a Sync attributes run: kind (copy, scan, attrs), type, folder_id, path (its folder, relative to the tree root), file_name, src_sha256, dst_sha256, match (true/false), attrs_match (true/false), attr_diff (what differs, e.g. `mode 0664 -> 0600`), result, and both sides' size, mode, uid, gid, mtime_ns and extended-attribute digest. Built from the `file_checks` table, which stores each file's folder as an id into `folders` (every folder path stored once) |
| `hash_mismatches` | file whose content differs, or that is missing on the target (`problem`) |
| `attr_mismatches` | file or folder whose attributes differ, with `attr_diff` and whether it was set (`result`) |

```sql
SELECT run, started, state, copied, failed FROM run_summary ORDER BY run DESC LIMIT 10;
SELECT * FROM failures WHERE run = 42;
SELECT path, file_name, problem FROM hash_mismatches WHERE run = 42;
SELECT path, file_name, attr_diff, result FROM attr_mismatches WHERE run = 42;
```


### Export for AI analysis

History > *Export logs to AI.md* and *Export SQL to AI.md* (for the selected run), View > *Export newest run for AI*, or `treesync-cli --export-ai-logs[=RUN] --export-ai-sql[=RUN]` write a run as Markdown into `logs/ai/`, organized but complete, for an AI (or a person) to analyse:

- `run-N-logs.AI.md`: a guide to reading TreeSync logs, the run, each start of the run (a resumed run has several logs), every problem and event (failures, warnings, network, stops, resumes), the 200 slowest copies, files and bytes per minute, then every log in full.
- `run-N-sql.AI.md`: the schema, all runs, this run's actions by kind and state, failures, biggest copies, chunks, every action, the logs of every run, the hash cache of both trees, the synced files, and queries to start from. It is large (a 50,000-file tree gives about 25 MB); nothing is left out.

Every log of every run is recorded in the `run_logs` table; older logs are found by their "run N" line.

## Sharing the machine

When a lot of other copying is going on, TreeSync can step back:

- `--bwlimit=N` caps the transfer at N KiB/s. It counts what crosses the network (remote reads and writes, including the read-back that verifies each copy, and the hashing of remote files during the scan); for a local copy it counts the writes. 
- `--low-priority` runs TreeSync at `nice 19` and the lowest normal disk priority (`ionice -c2 -n7`). It deliberately does not use the idle disk class, which could stall the sync completely while other copies run. It cannot change the remote server's own priority (SFTP has no way to ask); use `--bwlimit` to ease the load there.

## Safe delete

With `--safe-delete` (on by default), nothing a sync removes or overwrites is lost. Mirror's deletes, and the old version of every replaced file, are moved to a folder next to the tree with the same layout:

```
/backup/data/docs/a.txt   ->   /backup/data.old/docs/a.txt
```

If that name is already taken by an earlier move, the file gets a number on the end: `a.txt.1`, `a.txt.2`, and so on. Moving is a rename, so it is instant and needs no extra space, but it only works on the same disk: when the target is the top folder of a mount, point `--safe-delete-dir` at another folder on that disk. In Merge mode, replaced files on the source side go to `<source>.old`. Empty folders are removed, not moved. TreeSync never cleans up the `.old` folder by itself; **Sync > Clear .old folders...** (`--clear-old`) deletes it when you ask.

## Owners across machines

The same user often has different numbers on different machines (for example `jay` is 1001 on one and 1000 on another, where 1000 is someone else). So `--preserve-owner` matches owners and groups by name; a name the other side does not know keeps its number, with one warning. Owners can only be set with root on the side being written (root here, or `--SIDE-user=root` there); otherwise TreeSync says so once and copies everything else.

To line the two machines up for good:

```bash
treesync-cli --compare-ids --target-host=server
```

lists users and groups whose numbers differ, numbers that mean different accounts on each side, and accounts found on only one side. It then suggests one numbering for both and prints the `usermod` / `groupmod` / `chown` commands for each machine. TreeSync never runs them: review them and run them yourself as root, with the accounts logged out. Only regular accounts (1000 and up) get suggestions; system accounts differ between distributions and are left to name mapping. The `find ... -xdev` step covers one filesystem; repeat it for other mounts that hold the account's files.

## Remote trees (SFTP)

```bash
treesync-cli --source=/data --target-host=server --target-user=jay --target=/backup/data --mirror --no-dry-run
```

- **Login**: keys only, no passwords; set up passwordless SSH first (`ssh-keygen -t ed25519` if needed, then `ssh-copy-id user@host`). Tried in the order ssh uses: `--SIDE-key`, ssh-agent, then `~/.ssh/id_ed25519` / `id_ecdsa` / `id_rsa`.
- **Host key**: must already be in `known_hosts` (connect once with `ssh` to check and accept it). An unknown or changed key is refused, with its fingerprint in the message.
- **Copies**: always use the chunked copy, so a stopped remote copy resumes part-way through the file. Each copy is read back over the connection and checked against the source's SHA-256 before it is renamed into place, atomically, with OpenSSH's `posix-rename`.
- **Browsing**: the GUI lists remote folders over SFTP as you expand them, in a background thread so a slow server never freezes the window; errors (unknown host key, permission denied) show in the tree. Browsing runs as the GUI's user, so with Sync > Run as root the sync itself may see folders the browser cannot.
- **Network outages**: if the connection drops (network down, the router or Starlink restarting, the other machine rebooting), the run pauses instead of failing files. It says `network: lost the connection to HOST; waiting up to 30 min`, and the GUI's Copy box shows **NETWORK UNAVAILABLE** with a timer. Every 5 seconds it tries to connect again; when the machine answers, it says `network: HOST is back after 1 min 35 s`, each thread reconnects and repeats the step it was on, and a big file continues at its last checked chunk. A cut link shows within about a minute (TCP keep-alive and `TCP_USER_TIMEOUT`); a server that is gone shows at once. After `--network-wait` seconds (default 1800) the run stops instead, and running it again resumes it. A source folder that could not be read is never taken as empty: nothing is removed from its target folder. `v001/tests/network-outage.sh` checks all this against a private sshd it kills and restarts.
- **Traffic**: a verified copy sends each file once and reads it back once; `v001/tests/sftp-traffic.sh` measures this on a private sshd (a 128 MiB file: about 128 MiB each way).
- **Limits of SFTP**: no inode numbers and whole-second times, so the remote hash cache matches on size and time; no hard links, so hard-linked files are written out as separate copies (the log says so).

## Inspiration

TreeSync was inspired by **robocopy**, Microsoft's Robust File Copy for Windows: its switches (`/MIR`, `/FFT`, `/XO`, `/XF` and the rest) were the starting point for TreeSync's options, and the robocopy equivalents are listed for reference. No robocopy code was used or copied. TreeSync is written from scratch, for Linux, and made the way its author wanted it: GNU-style long options, SHA-256 comparison, verified copies, resumable runs, safe delete, and a GUI that shows what is going on.

The look owes a lot to the classic file managers: **Norton Commander**, whose two side-by-side panels set the pattern, the **Windows File Manager** with its folder tree and `+`/`-` to open and close folders, and **Midnight Commander**, which carried the two-panel idea to Linux. **Norton Utilities**, the disk tools from the same family, stand for the spirit of it: software that looks after your data. Thanks to all of them.

Two other tools shaped smaller details, again as ideas only: **rsync** (the option names `--bwlimit` and `--numeric-ids`) and **Fedora's dnf5** (the style of the progress lines).

TreeSync is not affiliated with or endorsed by Microsoft, Symantec/Gen Digital, or the Midnight Commander, rsync and dnf projects. Robocopy and the Windows File Manager are Microsoft products, and Norton Commander and Norton Utilities are Norton products; the names are used only to describe where ideas came from.

## License

Copyright 2026 Jay A Carlson

Licensed under the [Apache License, Version 2.0](LICENSE).
