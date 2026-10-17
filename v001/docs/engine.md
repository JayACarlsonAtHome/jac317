# TreeSync engine design

Status: design, not built yet. Everything here goes in `TreeSyncCore`, never the GUI.

## Principle: SQLite is the record of truth

Every step the engine takes is written to a local SQLite database before the next step starts. If TreeSync is stopped (Stop button, Ctrl+C, crash, power cut), the database says exactly how far it got, and the next run continues from there instead of starting over.

- U++ bundles SQLite 3.51.2 (`uppsrc/plugin/sqlite3`).
- The database runs in WAL mode with each step in its own transaction, so a crash loses at most the step that was in flight.
- The database is local and holds no passwords.

## Tables (first draft)

| Table | One row per | Key columns |
|---|---|---|
| `runs` | sync run | id, started, finished, source, target, command line, state (`running`, `stopped`, `done`, `failed`) |
| `files` | file seen on a side (also the hash cache, kept between runs) | side root, relative path, `st_dev`, `st_ino`, `st_nlink`, size, mtime (ns), ctime (ns), sha256, hashed at |
| `actions` | planned step in a run | run, sequence, relative path, kind (`mkdir`, `copy`, `link`, `delete`, `skip`, `conflict`), state (`planned`, `in_progress`, `done`, `failed`), bytes done, error |
| `chunks` | piece of a big file being copied | action, index, offset, length, sha256, done |

## Restart rules

1. On start, look for the last run in state `running` or `stopped` for the same source, target and options. Offer to resume it (`--resume` on the command line).
2. Skip actions already marked `done`.
3. Before continuing an action, check that the source file is the same file planned earlier (same identity, see below). If it changed, re-hash it and re-plan that one action.
4. **Small files:** an action left `in_progress` is redone from the start. Copies are written to `NAME.treesync-part` in the target folder, flushed to disk, then renamed over `NAME`. So the target never holds a half-written file under its real name, and a restart only has to delete the leftover `.treesync-part`.
5. **Big files** (default 1 GiB and up, adjustable): copied in chunks (default 64 MiB), each read once, hashed, written to the `.treesync-part` file at its offset, flushed, then recorded in `chunks` as done. A restart re-checks the last finished chunk (in case the final write was torn) and continues with the next one. Nothing already copied is copied again.

## Hashes

- **File hash:** SHA-256 of the list of chunk hashes, with the chunk size fixed in the database. This lets a big file's hash be finished after a restart without re-reading the whole file. Note: this differs from `sha256sum` output, but it is the same on both sides, which is what the comparison needs.
- **Folder hash:** SHA-256 of the sorted list of (name, type, hash) for everything inside it. Equal folder hashes mean the whole subtree can be skipped.

## Inodes

**Identity.** A file counts as unchanged since it was last hashed when `st_dev`, `st_ino`, size, mtime and ctime all match. Then the cached hash is reused and the file is not read. The inode is never used alone, because Linux reuses inode numbers after a file is deleted. On filesystems whose inode numbers are not stable (FAT, some FUSE and network filesystems), only size and times are used, and with `--fat-file-times` the time match has a 2-second tolerance.

**Hard links.** Files with the same `st_dev` and `st_ino` and `st_nlink > 1` are one file with several names. It is copied once and the other names are recreated as hard links at the target, not copied again.

**Progress estimates.** `statvfs` gives the number of inodes in use on a filesystem instantly (`df -i`; for example 304,804 on GFS). When a tree is a whole mount, that is an upper bound on how many entries the scan will find, so the progress bar can show a total before the scan finishes. For a sub-folder, the count from the previous run in `files` is used instead. FAT reports 0 inodes, so there is no estimate there until the scan finishes.

## Copying

- **Small files:** `cp --preserve=all --reflink=auto --sparse=auto -- SRC DST`, one file at a time, started with `posix_spawn` (an argument list, never a shell). Each `cp` runs inside the target folder with short relative names, so deep trees work.
- **Big files:** TreeSync's own chunked copy (`pread`, SHA-256, `pwrite` at the chunk's offset), because `cp` cannot resume part-way through a file.
- **Afterwards:** times, permissions and owner are set to match the source (owner only as root), then `.treesync-part` is renamed into place.
- **Progress:** each file and each chunk reports through the core, so the terminal prints dnf-style lines and the GUI shows the same steps.

## Stopping

Stop (button or Ctrl+C) finishes the current chunk or small file, commits it, marks the run `stopped` and exits. A second Ctrl+C stops at once. The restart rules above still hold, because each step's record was written before the step was counted as done.
