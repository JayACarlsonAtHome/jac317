# TreeSync engine design

Status: built for local trees (`core/TreeSyncCore`: `Db.cpp`, `Scan.cpp`, `Plan.cpp`, `Copy.cpp`, `Sync.cpp`), run by `treesync-cli` and the GUI, and covered by `TreeSyncTest`. Everything here lives in `TreeSyncCore`, never the GUI.

Differences from the first design, and gaps:
- **Merge never deletes**: something present on only one side is copied to the other.
- **Type changes are conflicts**: when a name is a file on one side and a folder on the other, nothing is changed.
- **Remote trees**: built on SFTP (`SftpFs.cpp`), behind the same `TreeFs` interface as local trees (`LocalFs.cpp`). Local to local, small files still go through `cp`; every other copy uses the chunked copy, so it is resumable.
- **Threads**: `--threads=N` (default 4) copies files in parallel. Runs go in four phases: folders and conflicts in order, file copies in parallel, hard links (they need their first copy), deletes deepest first. Each copy thread gets its own SFTP connection to a remote side; local sides are shared. One lock covers what the threads share (database, report, counters, owner tables); file data never moves under it. `--bwlimit` is one total across all threads. The tests pass under ThreadSanitizer with no data races reported (`./build.sh --compiler clang --tsan`).
- **Not yet**: resuming a scan part-way through hashing one big file (the files hashed before the stop are kept).
- **Retries** are skipped for errors waiting will not fix (permission denied, no space, read-only, quota, unsupported, missing file).
- **Flushing**: only multi-chunk copies are flushed after each chunk; a one-chunk copy that is cut short is simply copied again.
- **Verified copies are cached**: after a copy passes its SHA-256 check, the target file's row goes straight into `files`, so the next scan of the target does not read it again.

## Streaming (Copy and Mirror)

Copy and Mirror runs are streamed: three stages run at the same time, so copying starts as soon as the scanner finds something instead of after both trees are scanned.

- **Pre-scan** (one thread) walks the source a folder at a time, reusing cached hashes, creates folders on the target and queues the files.
- **Copy** (`--threads` workers) takes files right behind it: checks the target, hashes only what is not known yet (a big file is compared chunk by chunk, stopping at the first difference), and copies through the chunk pipeline: read and hash, write, read back and check, per chunk.
- **Post-scan** (one thread, the side channel) takes each folder once it is fully scanned and all its files are done, removes what the source does not have (Mirror, safe delete) and confirms the rest.

Each action is recorded when it is decided, before it runs. A stopped run is resumed by running it again: the walk is quick with the hash caches, and a big file continues at its last checked chunk. Merge needs both sides complete to tell which side changed, so it keeps the planned engine described below.

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
5. **Big files** (default 1 GiB and up, adjustable): copied in chunks (default 64 MiB), each read once, chunk-hashed, written to the `.treesync-part` file at its offset, flushed, then recorded in `chunks` as done. A restart re-checks the last finished chunk (in case the final write was torn) and continues with the next one. Nothing already copied is copied again.

## Hashes

Two kinds, each doing one job:

- **Chunk hashes, while copying:** each chunk of a big file is hashed as it is copied and recorded in `chunks`. They exist so a stopped copy can resume: a restart re-checks the last finished chunk against its recorded hash and continues from there. They are working data, not the file's identity.
- **Real hash, when the copy is finished:** a plain SHA-256 of the whole file, the same value `sha256sum` prints. This is the hash stored in `files`, compared between the two sides, and used for folder hashes. Once a copy completes, the target `.treesync-part` file is read back and hashed. Only if that matches the source's real hash is it renamed into place. So every copy is verified against what is actually on the target disk.
- The source's real hash comes from the scan, or from the cache in `files` when the file is unchanged (see Inodes).
- If the real hash of the copy does not match, the action is marked `failed`, the `.treesync-part` file is kept for inspection, and the error is reported. Nothing is renamed over the target.

**Folder hash:** SHA-256 of the sorted list of (name, type, real hash) for everything inside it. Equal folder hashes mean the whole subtree can be skipped.

## Inodes

**Identity.** A file counts as unchanged since it was last hashed when `st_dev`, `st_ino`, size, mtime and ctime all match. Then the cached hash is reused and the file is not read. The inode is never used alone, because Linux reuses inode numbers after a file is deleted. On filesystems whose inode numbers are not stable (FAT, some FUSE and network filesystems), only size and times are used, and with `--fat-file-times` the time match has a 3-second tolerance.

**Hard links.** Files with the same `st_dev` and `st_ino` and `st_nlink > 1` are one file with several names. It is copied once and the other names are recreated as hard links at the target, not copied again.

**Progress estimates.** `statvfs` gives the number of inodes in use on a filesystem instantly (`df -i`; for example 304,804 on GFS). When a tree is a whole mount, that is an upper bound on how many entries the scan will find, so the progress bar can show a total before the scan finishes. For a sub-folder, the count from the previous run in `files` is used instead. FAT reports 0 inodes, so there is no estimate there until the scan finishes.

## Copying

- **Small files:** `cp --preserve=all --reflink=auto --sparse=auto -- SRC DST`, one file at a time, started with `posix_spawn` (an argument list, never a shell). Each `cp` runs inside the target folder with short relative names, so deep trees work.
- **Big files:** TreeSync's own chunked copy (`pread`, SHA-256, `pwrite` at the chunk's offset), because `cp` cannot resume part-way through a file.
- **Afterwards:** the `.treesync-part` file is read back for its real SHA-256 and checked against the source. Then times, permissions and owner are set to match the source (owner only as root), and it is renamed into place.
- **Progress:** each file and each chunk reports through the core, so the terminal prints dnf-style lines and the GUI shows the same steps.

## Stopping

Stop (button or Ctrl+C) finishes the current chunk or small file, commits it, marks the run `stopped` and exits. A second Ctrl+C stops at once. The restart rules above still hold, because each step's record was written before the step was counted as done.

## Future: GST time (10 ps)

Times will later be stored as GST (Galactic Standard Time), which resolves to 10 picoseconds. Today file times are nanoseconds (what Linux `stat` gives, `int64`) and run times are whole UTC seconds. A 64-bit count of 10 ps steps only spans about ±2.9 years, so GST times need two fields: whole seconds (`int64`) plus a fraction in 10 ps units (0 to 99,999,999,999), or a 128-bit value. SQLite integers are 64-bit, so that means two columns per time. Comparisons that use time tolerances (`--fat-file-times`, `--dst-tolerance`) then work on the pair.
