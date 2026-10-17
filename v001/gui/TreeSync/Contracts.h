// TreeSync contracts shim, same model as jac313 Setup/contracts.hpp.
// TS_PRE / TS_POST / TS_ASSERT have one call-site spelling and pick a mode:
//   (A) NATIVE   : __cpp_contracts defined -> contract_assert(expr).
//   (B) RUNTIME  : no native contracts (gcc 15, clang 21 today) -> check + abort.
//   (C) DISABLED : TS_CONTRACTS_ENABLED == 0 -> zero-cost no-ops.
// Native pre()/post() belong on the function declarator, which a macro cannot
// reach, so TS_PRE/TS_POST are point checks placed in the function body.
#ifndef TREESYNC_CONTRACTS_H
#define TREESYNC_CONTRACTS_H

#include <cstdio>
#include <cstdlib>
#include <source_location>

namespace treesync::contracts {

[[noreturn]] inline void
violation(const char* kind, const char* expr,
          const std::source_location loc = std::source_location::current())
{
	std::fprintf(stderr, "TreeSync contract %s violated: (%s) at %s:%u in %s\n",
	             kind, expr, loc.file_name(),
	             static_cast<unsigned>(loc.line()), loc.function_name());
	std::abort();
}

} // namespace treesync::contracts

#ifndef TS_CONTRACTS_ENABLED
#  define TS_CONTRACTS_ENABLED 1
#endif

#if TS_CONTRACTS_ENABLED

#  if defined(__cpp_contracts)
#    define TS_CONTRACTS_NATIVE 1
#    define TS_ASSERT(expr) contract_assert(expr)
#    define TS_PRE(expr)    contract_assert(expr)
#    define TS_POST(expr)   contract_assert(expr)
#  else
#    define TS_CONTRACTS_NATIVE 0
#    define TS_ASSERT(expr) \
	((expr) ? (void)0 : ::treesync::contracts::violation("assert", #expr))
#    define TS_PRE(expr) \
	((expr) ? (void)0 : ::treesync::contracts::violation("precondition", #expr))
#    define TS_POST(expr) \
	((expr) ? (void)0 : ::treesync::contracts::violation("postcondition", #expr))
#  endif

#else

#  define TS_CONTRACTS_NATIVE 0
#  define TS_ASSERT(expr) ((void)0)
#  define TS_PRE(expr)    ((void)0)
#  define TS_POST(expr)   ((void)0)

#endif

#endif
