# TreeSync

TreeSync keeps two directory trees in sync. The trees can be on the same machine or on different machines.

## Overview

- **Two trees**: a source tree and a target tree, on one machine or on two machines.
- **SHA-256 comparison**: each file and each folder gets a SHA-256 hash. Matching hashes mean the files or folders are the same, and those parts of the tree are skipped.
- **Local SQLite cache**: hashes are stored in a local SQLite table, so a later sync only has to re-hash what has changed.
- **U++ GUI**: a desktop interface built with [U++ (Ultimate++)](https://www.ultimatepp.org/) for:
  - setting sync options
  - viewing one or both trees
  - showing sync results (what matched, what was copied, updated or skipped, and any conflicts)

## How it works

1. **Scan**: walk each tree and compute SHA-256 hashes for files. A folder's hash is built from the hashes of everything inside it.
2. **Cache**: save the paths, hashes and metadata in the local SQLite database.
3. **Compare**: compare the two trees by hash. When two folders have the same hash, skip everything under them.
4. **Sync**: copy or update the files that differ, based on the options chosen in the GUI.
5. **Report**: show the results in the GUI.

## Status

Early development.
