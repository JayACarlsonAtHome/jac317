# TreeSync

TreeSync keeps two directory trees in sync. The trees can be on the same machine or on different machines.

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

### Building the GUI

Like jac313, TreeSync builds with one selected compiler at a time:

```bash
cd v001/gui
./build.sh --compiler clang            # clang 21+, bm/CLANG26.bm (default)
./build.sh --compiler gcc              # gcc-toolset-15, bm/GCC15.bm
./build.sh --compiler gcc --release
```

Output goes to `v001/gui/out/<compiler>/treesync`. U++'s stock build methods pass `-std=c++17` after any package options, so TreeSync ships its own build methods in `v001/gui/bm/` that pass `-std=c++26`. A `static_assert` stops the build if C++26 is not in effect.

## Long paths and awkward names

- **Long paths**: folders are opened one component at a time (`openat`), so trees deeper than `PATH_MAX` (4096 bytes) still work.
- **Spaces and other characters**: names with spaces (including leading or trailing ones), tabs, newlines, quotes, backslashes and non-ASCII characters are handled as-is. Paths never go through a shell, so nothing needs quoting. Remote transfers must keep this rule: use an SFTP library, not `ssh`/`scp` command lines.
- **Pasted paths**: the path field accepts `"quoted"` paths, `/shell\ escaped` paths, `file://` URIs, `~`, and a trailing newline from a paste.

## Running as root

TreeSync is meant to run as root, because a sync often meets files the current user cannot read or replace. Without root it still runs, after a warning, but local trees are limited to your home directory. Paths are resolved before they are checked, so `..` and symlinks cannot step outside home. Remote trees are not limited this way; what they can reach depends on the remote login.

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
| `--skip-links` | Do not follow symlinked folders (default on) | `/XJ` |
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
| `--threads=N` | Copy threads (default 8) | `/MT:n` |
| `--SIDE=PATH` | Tree root; SIDE is `source` or `target` | |
| `--SIDE-host`, `-user`, `-port`, `-key` | Remote login for that side | |

Passwords are not accepted on the command line, because other users can see them with `ps`. Use `--SIDE-key` or the setup screen.

Content is compared by SHA-256. File size and time decide whether a cached hash can be reused and, in Merge mode, which side is newer, so `--fat-file-times` and `--dst-tolerance` still matter.

## Status

Early development.

## License

Copyright 2026 Jay A Carlson

Licensed under the [Apache License, Version 2.0](LICENSE).
