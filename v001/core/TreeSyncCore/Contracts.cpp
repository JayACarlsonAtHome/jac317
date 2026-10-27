// Native contracts (gcc 16+): C++26 lets the program replace the violation handler, so
// a failed contract_assert prints the same "TreeSync contract ... violated" line as the
// runtime fallback in Contracts.h, instead of the compiler's own text.
#include "Contracts.h"

#if TS_CONTRACTS_NATIVE

#include <contracts>

void
handle_contract_violation(const std::contracts::contract_violation& v)
{
	using std::contracts::assertion_kind;
	const char* kind = v.kind() == assertion_kind::pre  ? "precondition"
	                 : v.kind() == assertion_kind::post ? "postcondition"
	                                                    : "assert";
	const char* expr = v.comment() ? v.comment() : "?";
	if(v.mode() == std::contracts::detection_mode::evaluation_exception)
		std::fprintf(stderr, "TreeSync contract check threw an exception:\n");
	::treesync::contracts::report(kind, expr, v.location());
	if(v.is_terminating())   // enforce: stop here; observe: report and go on
		std::abort();
}

#endif
