# gcc 15 -Wstringop-overread in SQLite3 Multiple Ciphers: false alarms

Found 2026-10-20 in a clean release build with gcc 15 (gcc-toolset-15). The file is
`uppsrc/plugin/sqlite3/lib/sqlite3mc_amalgamation.c` (SQLite3 Multiple Ciphers 2.2.7,
SQLite 3.51.2), which U++ 2026.1 bundles and compiles through `plugin/sqlite3/lib.c`.
clang does not warn.

## The four warnings

1. `sha1_transform` reading 64 bytes from a region of size 1, 4 and 16 (three warnings,
   `sha1_update`, inlined into `sha1_final` and `GenerateKeySQLCipherCipher` via
   `fastpbkdf2_hmac_sha1`). `sha1_update` calls `sha1_transform(context, data + i)` only
   inside `for (; i + 63 < len; i += 64)`, so 64 bytes are always there. gcc inlines a
   caller with a short buffer and does not follow the loop guard.
2. `strlen` reading 1 or more bytes from a region of size 0 (`sqlite3Strlen30`, inlined
   into `sqlite3ColumnSetColl`): gcc assumes `zColl` may be NULL ("source object is likely
   at address zero"); the function asserts `zColl!=0` and its callers ensure it.

## What TreeSync does

`tools.sh` `upp_quiet_sqlite3mc` (run by `bootstrap.sh`) puts
`#pragma GCC diagnostic ignored "-Wstringop-overread"` (gcc only) before the
`#include` in `lib.c`, keeping the original as `lib.c.orig`. Only that file is affected;
the amalgamation itself is not edited.

## For the owners

Report to SQLite3 Multiple Ciphers (utelle/SQLite3MultipleCiphers): gcc 15 at -O2/-O3
warns as above in `sha1_update`. A change that keeps the code as is but quiets gcc: pass
the block through a local `uint8_t block[64]` (memcpy) or mark the loop with
`__attribute__((access(read_only, 2, 3)))` on a length-taking helper. The `strlen` one is in
SQLite's own code (sqlite.org), a known class of gcc false positive.
