# U++ `Format` reads letters after `%d` as part of the format code

**Found:** twice on 2026-10-17, in `Progress.cpp` (`"%02dm%02ds"`) and `Sync.cpp` (`"in %ds"`).
**Status:** a U++ library behaviour, not a compiler bug. Worked around both times.

U++'s `Format()` treats the letters after a `%` specifier as the name of a custom formatter. `"%02dm"` is read as a format called `dm`, and `"%ds"` as one called `ds`. Neither exists, so the output contains the text `<N/A 'dm' for type 1>` instead of the number. Nothing fails at compile time.

```cpp
Format("%02dm%02ds", 1, 5);    // "<N/A 'dm' for type 1><N/A 'ds' for type 1>"
Format("%d s", 5);             // "5 s"   (a space ends the specifier)
std::format("{:02}m{:02}s", 1, 5);   // "01m05s"
```

## Rule for this project

When a format specifier must be followed directly by letters, use `std::format`, or put a space or other non-letter after the specifier. To check the code:

```bash
grep -rn -E 'Format\([^;]*%[-0-9.*]*[dsifx][a-zA-Z]' --include=*.cpp v001
```
