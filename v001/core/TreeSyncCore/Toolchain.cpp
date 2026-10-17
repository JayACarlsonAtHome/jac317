// Toolchain report: which compiler and library built this binary, and which C++26
// features they really provide. Part of the project's job is proving compilers, so
// the tests print this on every run and the GUI shows it under Help > About.
// Known toolchain bugs live in v001/docs/findings/.
#include "TreeSyncCore.h"

#include <vector>
#include <version>

#if defined(__clang__)
static const char *COMPILER = "clang " __clang_version__;
#elif defined(__GNUC__)
static const char *COMPILER = "gcc " __VERSION__;
#else
static const char *COMPILER = "unknown compiler";
#endif

struct Feature {
	const char *name;
	int64       value;   // 0 = missing
};

// #ifdef cannot live inside a macro, so each feature is spelled out.
#define FEATURE(name, value) list.push_back({ name, (int64)(value) })

static std::vector<Feature> Features()
{
	std::vector<Feature> list;
	// Language
#ifdef __cpp_contracts
	FEATURE("__cpp_contracts", __cpp_contracts);
#else
	FEATURE("__cpp_contracts", 0);
#endif
#ifdef __cpp_reflection
	FEATURE("__cpp_reflection", __cpp_reflection);
#else
	FEATURE("__cpp_reflection", 0);
#endif
#ifdef __cpp_pack_indexing
	FEATURE("__cpp_pack_indexing", __cpp_pack_indexing);
#else
	FEATURE("__cpp_pack_indexing", 0);
#endif
#ifdef __cpp_placeholder_variables
	FEATURE("__cpp_placeholder_variables", __cpp_placeholder_variables);
#else
	FEATURE("__cpp_placeholder_variables", 0);
#endif
#ifdef __cpp_deleted_function
	FEATURE("__cpp_deleted_function", __cpp_deleted_function);
#else
	FEATURE("__cpp_deleted_function", 0);
#endif
#ifdef __cpp_variadic_friend
	FEATURE("__cpp_variadic_friend", __cpp_variadic_friend);
#else
	FEATURE("__cpp_variadic_friend", 0);
#endif
#ifdef __cpp_structured_bindings
	FEATURE("__cpp_structured_bindings", __cpp_structured_bindings);
#else
	FEATURE("__cpp_structured_bindings", 0);
#endif
#ifdef __cpp_constexpr_exceptions
	FEATURE("__cpp_constexpr_exceptions", __cpp_constexpr_exceptions);
#else
	FEATURE("__cpp_constexpr_exceptions", 0);
#endif
#ifdef __cpp_trivial_relocatability
	FEATURE("__cpp_trivial_relocatability", __cpp_trivial_relocatability);
#else
	FEATURE("__cpp_trivial_relocatability", 0);
#endif
	// Library
#ifdef __cpp_lib_format
	FEATURE("__cpp_lib_format", __cpp_lib_format);
#else
	FEATURE("__cpp_lib_format", 0);
#endif
#ifdef __cpp_lib_print
	FEATURE("__cpp_lib_print", __cpp_lib_print);
#else
	FEATURE("__cpp_lib_print", 0);
#endif
#ifdef __cpp_lib_text_encoding
	FEATURE("__cpp_lib_text_encoding", __cpp_lib_text_encoding);
#else
	FEATURE("__cpp_lib_text_encoding", 0);
#endif
#ifdef __cpp_lib_saturation_arithmetic
	FEATURE("__cpp_lib_saturation_arithmetic", __cpp_lib_saturation_arithmetic);
#else
	FEATURE("__cpp_lib_saturation_arithmetic", 0);
#endif
#ifdef __cpp_lib_ranges_concat
	FEATURE("__cpp_lib_ranges_concat", __cpp_lib_ranges_concat);
#else
	FEATURE("__cpp_lib_ranges_concat", 0);
#endif
#ifdef __cpp_lib_inplace_vector
	FEATURE("__cpp_lib_inplace_vector", __cpp_lib_inplace_vector);
#else
	FEATURE("__cpp_lib_inplace_vector", 0);
#endif
#ifdef __cpp_lib_hive
	FEATURE("__cpp_lib_hive", __cpp_lib_hive);
#else
	FEATURE("__cpp_lib_hive", 0);
#endif
#ifdef __cpp_lib_simd
	FEATURE("__cpp_lib_simd", __cpp_lib_simd);
#else
	FEATURE("__cpp_lib_simd", 0);
#endif
#ifdef __cpp_lib_linalg
	FEATURE("__cpp_lib_linalg", __cpp_lib_linalg);
#else
	FEATURE("__cpp_lib_linalg", 0);
#endif
#ifdef __cpp_lib_senders
	FEATURE("__cpp_lib_senders", __cpp_lib_senders);
#else
	FEATURE("__cpp_lib_senders", 0);
#endif
	return list;
}

String ToolchainReport()
{
	String r;
	r << "Compiler:  " << COMPILER << "\n";
#ifdef _GLIBCXX_RELEASE
	r << "Library:   libstdc++ " << _GLIBCXX_RELEASE << " (" << __GLIBCXX__ << ")\n";
#elif defined(_LIBCPP_VERSION)
	r << "Library:   libc++ " << _LIBCPP_VERSION << "\n";
#endif
	r << "Standard:  __cplusplus " << (int64)__cplusplus << "\n";
	r << "Contracts: " << (TS_CONTRACTS_NATIVE ? "native C++26" : "runtime fallback (TS_PRE/TS_POST/TS_ASSERT)") << "\n";
	Vector<String> have, missing;
	for(const Feature& f : Features())
		if(f.value)
			have.Add(String(f.name) + " " + AsString(f.value));
		else
			missing.Add(f.name);
	r << "Has:       " << Join(have, "\n           ") << "\n";
	r << "Missing:   " << Join(missing, "\n           ") << "\n";
	return r;
}
