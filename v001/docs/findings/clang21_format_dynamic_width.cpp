// Toolchain finding: clang 21 + libstdc++ 15 rejects a valid C++26 std::format
// call with a dynamic width ("{:>{}}"); gcc 15 accepts it.
//
// Reproduce (RHEL 9, gcc-toolset-15 provides libstdc++ 15):
//
//     /opt/rh/gcc-toolset-15/root/usr/bin/g++ -std=c++26 clang21_format_dynamic_width.cpp   # PASS
//     clang++ -std=c++26 clang21_format_dynamic_width.cpp                                   # FAIL
//     clang++ -std=c++26 -DWORKAROUND clang21_format_dynamic_width.cpp                      # PASS
//
// See clang21_format_dynamic_width.md. Same bug as jac313
// v002/docs/findings/clang_cpp26_dynamic_format.cpp, reproduced independently.
#include <cstdio>
#include <format>

int main()
{
	int index = 3, digits = 3, count = 108;
#ifdef WORKAROUND
	std::string s = std::vformat("[{:>{}}/{}]", std::make_format_args(index, digits, count));
#else
	std::string s = std::format("[{:>{}}/{}]", index, digits, count);
#endif
	std::puts(s.c_str());   // [  3/108]
}
