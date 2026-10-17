# U++ `Format("%d", int64)` cuts the value to `int`

**Found:** 2026-10-17, while making `--bench-log`'s `@overall` lines 64-bit.
**Status:** U++ behaviour (uppsrc/Core/Format.cpp). Use `%lld` for `int64`.

## Where

`IntDoubleRegister` registers the same `"d"` formatter for `INT_V` and `INT64_V`, and
that formatter casts to `int`:

```cpp
String IntFormatter(const Formatting& f)
{
	if(f.format.GetCount() == 0 && f.id[0] == 'd' && f.id[1] == 0)
		return AsString((int)f.arg);
	...
	snprintf(q, 1000, '%' + f.format + f.id, (int)f.arg);
```

Only `"lld"`, `"lli"`, `"llo"`, `"llx"`, `"llX"` go to `Int64Formatter`. So
`Format("%d", (int64)3000000000)` prints a wrong (wrapped) number, with no warning
from the compiler: `Format` takes `Value`s, not a printf argument list.

## In TreeSync

- `gui/TreeSync/SyncRun.cpp`: `--bench-log`'s `@overall` lines (`i * 1024` also
  overflowed `int` past 2,097,152 lines).
- `core/TreeSyncCore/Stream.cpp`: the pre-scan status, `entries` and the statvfs
  inode `estimate` (large XFS file systems report more than 2^31 inodes).

All now `%lld`. `std::format` (used in treesync-cli) has no such trap.
