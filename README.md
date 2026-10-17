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
- **Compilers**: GCC 14+ or Clang 21+. On RHEL 9 the system GCC 11 is too old, so use `gcc-toolset-15` (`scl enable gcc-toolset-15 bash`) or `clang++`.
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

Like jac313, TreeSync builds with one selected compiler at a time:

```bash
cd v001
./build.sh --compiler clang            # clang 21+, bm/CLANG26.bm (default)
./build.sh --compiler gcc              # gcc-toolset-15, bm/GCC15.bm
./build.sh --compiler gcc --release
./build.sh --compiler gcc --test       # build and run the core tests
```

Output goes to `v001/out/<compiler>/treesync` (and `treesync-test`). U++'s stock build methods pass `-std=c++17` after any package options, so TreeSync ships its own build methods in `v001/bm/` that pass `-std=c++26`. A `static_assert` stops the build if C++26 is not in effect.

The tests build a scratch tree under `$TMPDIR` with awkward names and a folder chain deeper than `PATH_MAX`. They also check option round trips (including shell quoting through a real `sh`), the home-only limit, and the progress-line layout. The scratch tree is removed afterwards.

## Seeing what happens

The point of the GUI is to see what is going on. So the engine reports each step as it happens (scan, hash, compare, copy, skip, conflict) instead of handing the whole job to a tool that only prints a summary.

- **Terminal**: one line per copied file, in the style of Fedora's dnf5, then a total:

  ```
  [  3/108] photos/2024/beach day.jpg                       100% |   12.4 MiB/s |    3.1 MiB |  00m00s
  [  4/108] music/Sigur Rós/Ágætis byrjun/01 Intro.flac      25% |    2.0 MiB/s |    1.0 MiB |  00m00s
  [108/108] Total                                           100% |   14.0 MiB/s |  300.2 MiB |  00m21s
  ```

- **GUI**: Sync > Run (F9) starts `treesync-cli` and shows its output in a log pane under the trees; Sync > Stop (Esc) stops it after the current file or chunk, and the trees reload when it ends. Warnings appear in a dialog and are also printed to the terminal.

**Restarts**: every step is recorded in the SQLite database before the next one starts, so a stopped or crashed run continues where it left off: run the same command again. Big files are copied in chunks, each chunk recorded as it finishes, so a restart continues part-way through the file instead of from the start (for example "resuming big one.bin at chunk 7 of 19"). `--fresh` starts over instead. Inode numbers identify unchanged files, so they are not read again. Details are in [`v001/docs/engine.md`](v001/docs/engine.md).

**Copying**: small files are copied one at a time with `/usr/bin/cp --preserve=all --reflink=auto --sparse=auto`, started with `posix_spawn` (an argument list, never a shell, so spaces are safe) inside the target folder, so deep trees work. Big files use TreeSync's own chunked copy, because `cp` cannot resume part-way through a file. Every copy goes to `NAME.treesync-part`, is read back and checked against the source's SHA-256 (the same value `sha256sum` prints), and only then renamed into place. `rsync` would also work, but it hides the per-file steps the GUI is meant to show.

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
treesync --source=/data --target=/mnt/backup --mirror --fat-file-times --no-dry-run
treesync --source="$HOME/my dir" --target=/backup --target-host=nas --target-user=jay --merge
```

The switches are modelled on robocopy. TreeSync adds `--merge`, which copies changes both ways and reports a conflict when the same file changed on both sides.

| Option | Meaning | robocopy |
|---|---|---|
| `--copy` | Copy new and changed files, never delete | `/E` |
| `--mirror` | Make target match source, deleting extras (default) | `/MIR` |
| `--merge` | Copy both ways; changed on both sides = conflict | — |
| `--dry-run` | Report only, change nothing (default on) | `/L` |
| `--fat-file-times` | 2-second time tolerance (FAT, many network shares) | `/FFT` |
| `--dst-tolerance` | Allow exact 1-hour (DST) time shifts | `/DST` |
| `--hard-links` | Hard-linked files are copied once and relinked at the target (default on); `--no-hard-links` writes each name out as its own copy | |
| `--follow-links` | Write out what symlinks point to (files and folders); off by default, so symlinks are copied as symlinks | off = `/SL` |
| `--include-same` | Include same files (rehash / recopy) | `/IS` |
| `--skip-older` | Skip if source is older than target | `/XO` |
| `--skip-newer` | Skip if source is newer than target | `/XN` |
| `--skip-changed` | Skip changed files (same time, new hash) | `/XC` |
| `--keep-extra` | Keep extra target files (mirror only) | `/XX` |
| `--skip-source-only` | Skip files that exist only in source | `/XL` |
| `--exclude-file=GLOB` | Skip matching files (repeatable) | `/XF` |
| `--exclude-dir=GLOB` | Skip matching folders (repeatable) | `/XD` |
| `--retries=N` | Retries on a failed copy (default 3) | `/R:n` |
| `--retry-wait=N` | Seconds between retries (default 5) | `/W:n` |
| `--threads=N` | Copy threads (default 8; not used yet, copies run one at a time) | `/MT:n` |
| `--big-file-mib=N` | Files this big or bigger are copied in resumable chunks (default 1024) | |
| `--chunk-mib=N` | Chunk size for big files (default 64) | |
| `--preserve-owner` | Copy owner and group, matched by **name** (default on; needs root on the side being written) | `/COPY:O` |
| `--numeric-ids` | Copy owners by number instead of name | |
| `--compress` | Compress SSH traffic with zlib; the other end decompresses on its own. Helps text on slow links, not media | |
| `--fresh` | Start over instead of resuming an unfinished run | |
| `--known-hosts=FILE` | SSH known_hosts file (default `~/.ssh/known_hosts`) | |
| `--db=PATH` | Database file (default `~/.local/share/treesync/treesync.db`) | |
| `--SIDE=PATH` | Tree root; SIDE is `source` or `target` | |
| `--SIDE-host`, `-user`, `-port`, `-key` | Remote login for that side | |

Passwords are not accepted on the command line, because other users can see them with `ps`. Use `--SIDE-key` or the setup screen.

Content is compared by SHA-256. File size and time decide whether a cached hash can be reused and, in Merge mode, which side is newer, so `--fat-file-times` and `--dst-tolerance` still matter.

## Status

Working for local trees and over SFTP, with `treesync-cli` and the GUI: scan with a hash cache, Copy / Mirror / Merge, verified copies, hard links, symlinks, resumable runs and big files. Tested against a Fedora 44 server in both directions. Not yet: browsing remote trees in the GUI, password login from the GUI, copying with several threads, and live per-chunk progress in the GUI log.

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

- **Login**: in the order ssh uses: `--SIDE-key`, ssh-agent, `~/.ssh/id_ed25519` / `id_ecdsa` / `id_rsa`, then a password.
- **Host key**: must already be in `known_hosts` (connect once with `ssh` to check and accept it). An unknown or changed key is refused, with its fingerprint in the message.
- **Copies**: always use the chunked copy, so a stopped remote copy resumes part-way through the file. Each copy is read back over the connection and checked against the source's SHA-256 before it is renamed into place, atomically, with OpenSSH's `posix-rename`.
- **Limits of SFTP**: no inode numbers and whole-second times, so the remote hash cache matches on size and time; no hard links, so hard-linked files are written out as separate copies (the log says so).

## License

Copyright 2026 Jay A Carlson

Licensed under the [Apache License, Version 2.0](LICENSE).
