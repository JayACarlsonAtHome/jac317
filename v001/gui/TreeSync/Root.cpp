// Privilege check. TreeSync is meant to run as root: a sync often meets files the
// current user cannot read or replace. Without root it still runs, but local trees
// are limited to the user's home directory.
#include "TreeSync.h"

#include <unistd.h>

namespace fs = std::filesystem;

bool IsRoot()
{
	return geteuid() == 0;
}

// Resolves symlinks and "..", so neither can step outside the home directory.
// If the path cannot be resolved (e.g. longer than PATH_MAX), ".." is still removed
// lexically; symlinks in it then go unchecked, which is acceptable because the home
// limit is a convenience: without root the OS permissions still apply.
static String Resolved(const String& p)
{
	std::error_code ec;
	fs::path c = fs::weakly_canonical(ToFs(p), ec);
	return FromFs(ec ? fs::absolute(ToFs(p), ec).lexically_normal() : c);
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
		"[* TreeSync is not running as root.]&&"
		"It will still work, but only inside your home directory:&"
		"[C " + DeQtf(HomeDir()) + "]&&"
		"To sync other folders, run TreeSync as root.");
}

void RefusePath(const String& p)
{
	Exclamation(
		"[C " + DeQtf(p) + "] is outside your home directory.&&"
		"Without root, TreeSync only works inside [C " + DeQtf(HomeDir()) + "].");
}
