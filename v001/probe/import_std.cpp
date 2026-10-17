// The import std probe: a small program that uses the parts of the standard library
// TreeSync does (format, print, filesystem, chrono, containers, algorithms, strings),
// built twice by compare.sh: with #include (textual) and with import std (modules),
// so the two can be timed against each other on each compiler.
#ifdef TS_IMPORT_STD
import std;
#else
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <map>
#include <print>
#include <ranges>
#include <string>
#include <vector>
#endif

int main()
{
	namespace fs = std::filesystem;
	std::vector<std::string> paths;
	for(int i = 0; i < 1000; i++)
		paths.push_back(std::format("dir{}/sub{}/file {}.txt", i % 7, i % 5, i));
	std::ranges::sort(paths);
	std::map<std::string, int> per_folder;
	for(const std::string& p : paths)
		per_folder[fs::path(p).parent_path().string()]++;
	auto t0 = std::chrono::steady_clock::now();
	auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	std::println("{} paths in {} folders, first {}, {:.3f} s", paths.size(), per_folder.size(), paths.front(), elapsed);
	return paths.size() == 1000 && per_folder.size() == 35 ? 0 : 1;
}
