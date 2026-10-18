# TreeSync Manual

TreeSync syncs two directory trees and checks every copy by SHA-256. It has a GUI
(`treesync`) and a command-line program (`treesync-cli`) that does the actual work;
the GUI starts `treesync-cli` and shows what it does.

This manual takes you from a fresh Linux machine to a running sync:

| Part | What it covers |
|---|---|
| [1. Bootstrap](1-Bootstrap.md) | Get the code, install the compilers and libraries, get U++, first build |
| [2. Build](2-Build.md) | `build.sh`, the Build page, debug and release, tests, `verify.sh` and the pre-push hook |
| [3. Run](3-Run.md) | The GUI step by step, the command line, remote trees, root, checking and attributes |
| [4. Files and troubleshooting](4-Files-and-Troubleshooting.md) | Where everything is kept, and what to do when something goes wrong |

> **Warning.** TreeSync is experimental testing software. It copies, overwrites and
> deletes files. Try it on copies first, keep backups, and use **Dry run** before a
> real run. It is provided "AS IS" under the Apache License 2.0 (sections 7 and 8).

In TreeSync, the **Help** menu opens these chapters as PDFs, and **F1** opens the one
for the window you are in (main window and Setup: Run; Build page: Build).

**Rebuilding the PDFs.** The `.md` files here are the sources. After editing them run
`./build-pdf.sh` in this folder: it writes `pdf/`, one PDF per chapter and
`TreeSync-Manual.pdf` with all of them (it needs python3 with its markdown module,
Chrome or Chromium, and pdfunite).

## Quick start

For someone who has done this before. Each step is explained in the parts above.

```bash
# 1. Get the code (the branch in use is dev-work)
git clone ssh://git@192.168.1.26:2222/jay/jac317.git
cd jac317/v001

# 2. Check the machine, install what is missing (it asks first), build and test
./bootstrap.sh

# 3. Build the release programs (with all tests, both compilers)
./verify.sh

# 4. Install them for root (asks for your sudo password)
./install.sh

# 5. Start TreeSync: "TreeSync" in the desktop's app list, or
treesync
```

The installed TreeSync syncs as root for administrators (members of wheel); you build
as yourself in `v001/`, and
run `./install.sh` again after each build you want to use.

In the GUI: pick a **source** and a **target** folder, check what to sync in the
source tree, then **Sync > Dry run** (Shift+F9) to see what would happen and
**Sync > Run** (F9) to do it.

## Words used in this manual

| Word | Meaning |
|---|---|
| **Source** | The tree copied from |
| **Target** | The tree copied to |
| **Copy / Mirror / Merge** | The three modes: add and update only; make the target match the source (removing extras); copy changes both ways |
| **Dry run** | A run that only reports what it would do and changes nothing |
| **Safe delete** | Instead of deleting or overwriting, TreeSync moves the old file into `<target>.old` (on by default) |
| **Run** | One sync, with a number; it is recorded in the database and can be resumed if stopped |
| **Scan only** | Reads and hashes both trees and compares them; changes nothing |
| **`v001/`** | The folder that holds the code, the scripts and (after a build) `out/` with the programs |
