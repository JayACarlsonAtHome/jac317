// Privilege check. TreeSync is meant to run as root (Administrator on Windows):
// a sync often meets files the current user cannot read or replace. Without root
// it still runs, but local trees are limited to the user's home directory.
#include "TreeSync.h"

#ifdef PLATFORM_POSIX
#include <unistd.h>
#endif

#ifdef PLATFORM_WIN32
#include <shlobj.h>
#endif

namespace fs = std::filesystem;

bool IsRoot()
{
#ifdef PLATFORM_POSIX
	return geteuid() == 0;
#elif defined(PLATFORM_WIN32)
	return IsUserAnAdmin();
#else
	return false;
#endif
}

String RootName()
{
#ifdef PLATFORM_WIN32
	return "Administrator";
#else
	return "root";
#endif
}

// Resolves symlinks and "..", so neither can step outside the home directory.
static String Resolved(const String& p)
{
	std::error_code ec;
	fs::path c = fs::weakly_canonical(p.ToStd(), ec);
	return ec ? p : String(c.string());
}

String HomeDir()
{
	return Resolved(GetHomeDirectory());
}

bool PathAllowed(const String& p)
{
	return IsRoot() || p.IsEmpty() || IsUnder(Resolved(p), HomeDir());
}

void WarnNotRoot()
{
	if(IsRoot())
		return;
	Exclamation(
		"[* TreeSync is not running as " + DeQtf(RootName()) + ".]&&"
		"It will still work, but only inside your home directory:&"
		"[C " + DeQtf(HomeDir()) + "]&&"
		"To sync other folders, run TreeSync as " + DeQtf(RootName()) + ".");
}

void RefusePath(const String& p)
{
	Exclamation(
		"[C " + DeQtf(p) + "] is outside your home directory.&&"
		"Without " + DeQtf(RootName()) + ", TreeSync only works inside "
		"[C " + DeQtf(HomeDir()) + "].");
}
