// Privilege check. TreeSync is meant to run as root: a sync often meets files the
// current user cannot read or replace. Without root it still runs, but local trees
// are limited to the user's home directory.
#include "TreeSyncCore.h"

#include <grp.h>
#include <pwd.h>
#include <unistd.h>

namespace fs = std::filesystem;

bool IsRoot()
{
	return geteuid() == 0;
}

bool InvokingUser(String& name, String& home)
{
	if(!IsRoot())
		return false;
	passwd *pw = nullptr;
	if(String uid = GetEnv("PKEXEC_UID"); uid.GetCount())
		pw = getpwuid((uid_t)atoi(uid));
	else if(String user = GetEnv("SUDO_USER"); user.GetCount())
		pw = getpwnam(user);
	if(!pw || pw->pw_uid == 0 || !pw->pw_dir || !*pw->pw_dir)
		return false;
	name = pw->pw_name;
	home = pw->pw_dir;
	return true;
}

bool IsAdminGroup(const String& group)
{
	return group == "wheel" || group == "sudo" || group == "admin";
}

bool IsAdmin()
{
	if(IsRoot())
		return true;
	int n = getgroups(0, nullptr);
	Buffer<gid_t> ids(max(n, 1));
	n = getgroups(n, ids);
	for(int i = 0; i < n; i++)
		if(group *g = getgrgid(ids[i]); g && IsAdminGroup(g->gr_name))
			return true;
	return false;
}

// Resolves symlinks and "..", so neither can step outside the home directory.
// If the path cannot be resolved (e.g. longer than PATH_MAX), ".." is still removed
// lexically; symlinks in it then go unchecked, which is acceptable because the home
// limit is a convenience: without root the OS permissions still apply.
String CanonicalPath(const String& p)
{
	std::error_code ec;
	fs::path c = fs::weakly_canonical(ToFs(p), ec);
	return FromFs(ec ? fs::absolute(ToFs(p), ec).lexically_normal() : c);
}

String HomeDir()
{
	return CanonicalPath(GetHomeDirectory());
}

bool PathAllowed(const String& p)
{
	return IsRoot() || p.IsEmpty() || IsUnder(CanonicalPath(p), HomeDir());
}

String NotRootMessage()
{
	return "TreeSync is not running as root.\n"
	       "It will still work, but only inside your home directory: " + HomeDir() + "\n" +
	       String(IsAdmin() ? "To sync other folders, use TreeSyncRoot (or treesync-cli as root)."
	                        : "To sync other folders, ask an administrator (a member of wheel) to sync them with TreeSyncRoot.");
}

String RefusedPathMessage(const String& p)
{
	return p + " is outside your home directory.\n"
	       "Without root, TreeSync only works inside " + HomeDir() + ".";
}
