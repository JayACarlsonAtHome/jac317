// Users and groups on each side, matched by name.
//
// Syncs: --preserve-owner copies owner and group by name, because the same name
// often has different numbers on different machines (jay is 1001 on one, 1000 on
// another). --numeric-ids keeps the numbers instead.
//
// --compare-ids: reports where the two sides differ and suggests one numbering for
// both, as commands to review and run by hand. TreeSync never renumbers accounts
// itself, and only suggests changes for regular accounts (1000 and up): system
// accounts differ between distributions and are left to name mapping.
#include "TreeSyncCore.h"

#include <grp.h>
#include <pwd.h>

void IdTable::Add(bool group, const String& name, int64 id)
{
	VectorMap<String, int64>& by_name = group ? gid : uid;
	VectorMap<int64, String>& by_id = group ? group_of : user_of;
	if(by_name.Find(name) < 0)
		by_name.Add(name, id);
	if(by_id.Find(id) < 0)
		by_id.Add(id, name);
}

// name:x:id:... lines, as in /etc/passwd and /etc/group.
void IdTable::Parse(bool group, const String& text)
{
	for(const String& line : Split(text, '\n')) {
		if(line.StartsWith("#"))
			continue;
		Vector<String> f = Split(line, ':', false);
		if(f.GetCount() >= 3 && f[0].GetCount() && IsDigit(*f[2]))
			Add(group, f[0], ScanInt64(f[2]));
	}
}

bool LoadLocalIds(IdTable& t, String&)
{
	setpwent();
	while(passwd *p = getpwent())
		t.Add(false, p->pw_name, p->pw_uid);
	endpwent();
	setgrent();
	while(group *g = getgrent())
		t.Add(true, g->gr_name, g->gr_gid);
	endgrent();
	return true;
}

int64 MapId(const IdTable& from, const IdTable& to, bool group, int64 id, bool numeric, String *unknown)
{
	if(numeric || id < 0)
		return id;
	const VectorMap<int64, String>& names = group ? from.group_of : from.user_of;
	const VectorMap<String, int64>& ids = group ? to.gid : to.uid;
	String name = names.Get(id, Null);
	if(IsNull(name))
		return id;   // no name for it on this side: keep the number
	int q = ids.Find(name);
	if(q >= 0)
		return ids[q];
	if(unknown)
		*unknown = name;
	return id;       // name unknown on the other side: keep the number
}

static bool Regular(int64 id)
{
	return id >= 1000 && id < 65534;   // not system accounts, not nobody
}

static int64 FreeId(const VectorMap<int64, String>& a, const VectorMap<int64, String>& b, int64 from)
{
	for(int64 id = max<int64>(from, 1000);; id++)
		if(a.Find(id) < 0 && b.Find(id) < 0)
			return id;
}

String CompareIds(const IdTable& a, const IdTable& b, const String& name_a, const String& name_b)
{
	String r = String(DISCLAIMER) + "\n", cmd_a, cmd_b;
	int changes = 0;
	for(int group = 0; group < 2; group++) {
		const char *what = group ? "group" : "user";
		const VectorMap<String, int64>& ia = group ? a.gid : a.uid;
		const VectorMap<String, int64>& ib = group ? b.gid : b.uid;
		const VectorMap<int64, String>& na = group ? a.group_of : a.user_of;
		const VectorMap<int64, String>& nb = group ? b.group_of : b.user_of;
		Vector<String> differ, clash, only_a, only_b;
		int same = 0;

		// Plan the regular renumberings first, so suggestions do not collide.
		VectorMap<int64, String> taken_a = clone(na), taken_b = clone(nb);
		for(int i = 0; i < ia.GetCount(); i++) {
			const String& name = ia.GetKey(i);
			int64 x = ia[i];
			int q = ib.Find(name);
			if(q < 0) {
				only_a.Add(Format("%s %d", name, x));
				continue;
			}
			int64 y = ib[q];
			if(x == y) {
				same++;
				continue;
			}
			String line = Format("  %-20s %s %-8d %s %d", name, name_a, x, name_b, y);
			if(!Regular(x) || !Regular(y)) {
				differ.Add(line + "   (system account: mapped by name, renumbering not suggested)");
				continue;
			}
			// Keep one side's number if the other side has it free; else a new one free on both.
			int64 target;
			if(taken_b.Find(x) < 0)
				target = x;
			else if(taken_a.Find(y) < 0)
				target = y;
			else
				target = FreeId(taken_a, taken_b, max(x, y));
			taken_a.GetAdd(target) = name;
			taken_b.GetAdd(target) = name;
			differ.Add(line + Format("   -> suggest %d on both", target));
			changes++;
			auto renumber = [&](String& cmd, int64 old) {
				if(old == target)
					return;
				cmd << "# " << what << " " << name << ": " << old << " -> " << target << "\n";
				if(group)
					cmd << "groupmod -g " << target << " '" << name << "'\n"
					    << "find / -xdev -gid " << old << " -exec chgrp -h " << target << " {} +\n";
				else
					cmd << "usermod -u " << target << " '" << name << "'   # also fixes the home folder\n"
					    << "find / -xdev -uid " << old << " -exec chown -h " << target << " {} +\n";
			};
			renumber(cmd_a, x);
			renumber(cmd_b, y);
		}
		for(int i = 0; i < ib.GetCount(); i++)
			if(ia.Find(ib.GetKey(i)) < 0)
				only_b.Add(Format("%s %d", ib.GetKey(i), ib[i]));
		for(int i = 0; i < na.GetCount(); i++) {
			int q = nb.Find(na.GetKey(i));
			if(q >= 0 && nb[q] != na[i])
				clash.Add(Format("  %-8d %s: %-20s %s: %s", na.GetKey(i), name_a, na[i], name_b, nb[q]));
		}

		r << "== " << (group ? "Groups" : "Users") << ": " << same << " match by name and number\n";
		if(differ.GetCount())
			r << "Same name, different number:\n" << Join(differ, "\n") << "\n";
		if(clash.GetCount())
			r << "Same number, different name (a raw-number copy would give files to the wrong "
			  << what << "):\n" << Join(clash, "\n") << "\n";
		if(only_a.GetCount())
			r << "Only on " << name_a << ": " << Join(only_a, ", ") << "\n";
		if(only_b.GetCount())
			r << "Only on " << name_b << ": " << Join(only_b, ", ") << "\n";
		r << "\n";
	}
	if(changes == 0) {
		r << "No renumbering suggested: syncs map owners by name (--preserve-owner).\n";
		return r;
	}
	r << "Suggested commands, to review and run as root on each machine. Do it when the\n"
	     "accounts are not logged in and their services are stopped; files on other\n"
	     "filesystems need the find/chown step repeated there (it uses -xdev).\n\n";
	if(cmd_a.GetCount())
		r << "### on " << name_a << "\n" << cmd_a << "\n";
	if(cmd_b.GetCount())
		r << "### on " << name_b << "\n" << cmd_b << "\n";
	return r;
}
