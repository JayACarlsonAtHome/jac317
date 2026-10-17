# clang 21 rejects `std::format` with a dynamic width in C++26

**Found:** 2026-10-17, writing `ProgressLine` (`core/TreeSyncCore/Progress.cpp`).
**Status:** open, worked around.
**Same bug as:** jac313 `v002/docs/findings/clang_cpp26_dynamic_format.cpp` (found there with `std::print`). jac317 hit it independently with `std::format`.

## Result

| Build | Result |
|---|---|
| gcc 15.2.1, `-std=c++26` | PASS |
| clang 21.1.8 + libstdc++ 15, `-std=c++26` | **FAIL** |
| clang 21.1.8 + libstdc++ 15, `-std=c++23` | PASS |
| clang 21.1.8, `-std=c++26`, `std::vformat` workaround | PASS |

Reproducer: [`clang21_format_dynamic_width.cpp`](clang21_format_dynamic_width.cpp).

```cpp
std::format("[{:>{}}/{}]", index, digits, count);   // valid C++; clang 21 C++26 rejects it
```

```
error: call to consteval function 'std::basic_format_string<char, int &, int &, int &>::basic_format_string<char[12]>' is not a constant expression
note: undefined function '__check_dynamic_spec<int, unsigned int, long long, unsigned long long>' cannot be used in a constant expression
```

## Cause

libstdc++'s compile-time format-string check calls `__check_dynamic_spec`, a helper that is declared but never defined, whenever a width or precision comes from an argument (`{:>{}}`). gcc's constant evaluator accepts this; clang's does not. Only the C++26 code path reaches the helper, which is why `-std=c++23` builds.

## Workarounds

- What TreeSync does: build the padded counter by hand instead of using a dynamic width.
- What jac313 does: `std::runtime_format(...)`, which skips the compile-time check.
- Also works: `std::vformat(..., std::make_format_args(...))`.

## When to recheck

When either clang or libstdc++ is updated. If the reproducer passes under clang `-std=c++26`, the bug is fixed and `ProgressLine` can go back to `{:>{}}`.
