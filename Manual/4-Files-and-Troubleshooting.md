# 4. Files and troubleshooting

## 4.1 Where everything is kept

| What | Where |
|---|---|
| The programs | `v001/out/gcc/` and `v001/out/clang/`: `treesync`, `treesync-cli`, `treesync-test` |
| Build and test output | `v001/out/` (`verify/` logs, `bootstrap-*.txt`); ignored by git |
| GUI settings | `~/.config/treesync/settings`: one option per line, as on the command line |
| Build page choices | `~/.config/treesync/build` |
| Database | `~/.local/share/treesync/treesync.db` (SQLite): runs, actions, hash cache, checks |
| Run logs | `~/.local/share/treesync/logs/YYYY-MM-DD_HHMMSS.log` |
| Results of every run | `~/.local/share/treesync/logs/Results.txt`, one line per run |
| AI exports | `~/.local/share/treesync/logs/ai/run-N-logs.AI.md`, `run-N-sql.AI.md` |
| Root runs | the same under `/root/.local/share/treesync/` |
| Safe-delete folder | `<target>.old` next to the target (or `--safe-delete-dir`) |
| Unfinished copies | `NAME.treesync-part` next to where the file goes; removed when done |
| This manual | `Manual/*.md` (the sources) and `Manual/pdf/` (built by `Manual/build-pdf.sh`) |

`--db=PATH` puts the database (and its `logs/` folder) somewhere else.

## 4.2 Troubleshooting

**"TreeSync is not running as root ... only inside your home directory"**
: Local trees outside your home need root: tick **Sync > Run as root**, or run
  `sudo treesync-cli ...`. Inside your home nothing is needed.

**A path is refused**
: Without root, a path that leaves your home (also through `..` or a symlink) is
  refused. Use root, or a path inside your home.

**bootstrap stops: "Still missing: ..."**
: A required package did not install. Run the install command it printed by hand and
  read its error (often a repository that is not enabled, for example CodeReady
  Builder on RHEL for `gtk3-devel`), then run `./bootstrap.sh` again.

**build.sh: "U++ (umk + uppsrc) not found"**
: Run `./bootstrap.sh`, or point to an existing U++ with `export UPP_DIR=/path/to/u++`.

**A gcc release build crashes at start**
: The U++ heap fix is missing (`uppfix`); `./bootstrap.sh` applies it.

**A test fails**
: `./verify.sh` stops and prints the failing checks; the full output is in
  `v001/out/verify/<compiler>-<type>.log`. `git push --no-verify` pushes anyway, but
  fix it first if you can.

**A run is slow**
: Make sure the GUI runs a **release** build (`./verify.sh` leaves them in `out/`).
  Many small files: more threads (`--threads=8`) can help. Over SFTP the network is
  usually the limit; `--compress` helps text, not media.

**"no Wayland session found" on the Build page**
: The GUI was started without `WAYLAND_DISPLAY`, usually through `sudo`. Start it as
  yourself and use **Sync > Run as root** for root runs.

**The host key is refused (remote trees)**
: Connect once with `ssh user@host` to check and accept the key; TreeSync never
  accepts an unknown or changed key by itself.

**A run was stopped and the target has `.treesync-part` files**
: Run the same sync again: it resumes, re-checks what was already copied, and removes
  the part files when done. `--fresh` starts over instead.

**The `.old` folder keeps growing**
: Safe delete never deletes. **Sync > Clear .old folders...** (or
  `treesync-cli --clear-old --no-dry-run`) empties it when you are sure.

**Something else**
: **View > Export newest run for AI** and give the files in `logs/ai/` to an AI, or
  read them yourself: they list every problem of the run first.
