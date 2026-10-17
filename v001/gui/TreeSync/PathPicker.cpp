// Disk -> Mount -> Path selector. Picking a disk narrows the mount list; picking a
// mount sets the path to it; "..." browses from the current path.
#include "TreeSync.h"


PathPicker::PathPicker()
{
	Add(lbl_disk.SetLabel("Disk").LeftPosZ(0, 40).TopPosZ(0, 19));
	Add(disk.HSizePosZ(42, 0).TopPosZ(0, 19));
	Add(lbl_mount.SetLabel("Mount").LeftPosZ(0, 40).TopPosZ(24, 19));
	Add(mount.HSizePosZ(42, 0).TopPosZ(24, 19));
	Add(lbl_path.SetLabel("Path").LeftPosZ(0, 40).TopPosZ(48, 19));
	Add(path.HSizePosZ(42, 28).TopPosZ(48, 19));
	Add(browse.SetLabel("...").RightPosZ(0, 24).TopPosZ(48, 19));

	disk << [this] { FillMounts(); PickMount(); };
	mount << [this] { PickMount(); };
	path.WhenEnter = [this] { Commit(); };
	browse << [this] { Browse(); };
	LoadDisks();
}

void PathPicker::LoadDisks()
{
	mounts = ListMounts();
	disk.ClearList();
	disk.Add(Null, "All disks");

	// Without root only the home directory is offered, on the disk that holds it.
	String home_disk;
	if(!IsRoot()) {
		String home = HomeDir();
		int best = -1;
		for(int i = 0; i < mounts.GetCount(); i++)
			if(IsUnder(home, mounts[i].dir) &&
			   (best < 0 || mounts[i].dir.GetCount() > mounts[best].dir.GetCount()))
				best = i;
		MountInfo h;
		h.dir = home;
		h.disk = best >= 0 ? mounts[best].disk : String();
		h.label = home + "  (home, not root)";
		home_disk = h.disk;
		mounts.Clear();
		mounts.Add(h);
	}

	for(const DiskInfo& d : ListDisks())
		if(IsRoot() || d.id == home_disk)
			disk.Add(d.id, d.label);
	for(const MountInfo& m : mounts)
		if(m.disk == DISK_NETWORK) {
			disk.Add(DISK_NETWORK, "Network shares");
			break;
		}
	disk.SetIndex(0);
	FillMounts();
}

void PathPicker::FillMounts()
{
	String id = ~disk;
	mount.ClearList();
	for(const MountInfo& m : mounts)
		if(IsNull(id) || m.disk == id)
			mount.Add(m.dir, m.label);
	if(mount.GetCount() == 0)
		mount.Add(Null, "(not mounted)");
	mount.SetIndex(0);
}

void PathPicker::PickMount()
{
	String dir = ~mount;
	if(IsNull(dir))
		return;
	path <<= dir;
	WhenPath();
}

void PathPicker::Browse()
{
	FileSel fs;
	String start = ~path;
	std::error_code ec;
	fs.ActiveDir(std::filesystem::is_directory(ToFs(start), ec) ? start : String(~mount));
	if(!fs.ExecuteSelectDir("Tree root") || !Accept(~fs))
		return;
	Set(~fs);
	WhenPath();
}

void PathPicker::Commit()
{
	String p = CleanPath(~path);
	if(!Accept(p)) {
		path <<= committed;
		return;
	}
	Set(p);
	WhenPath();
}

bool PathPicker::Accept(const String& p)
{
	if(remote || PathAllowed(p))   // the home limit is for local trees only
		return true;
	RefusePath(p);
	return false;
}

// Select the disk and mount that hold p: the mount with the longest matching prefix.
void PathPicker::Set(const String& p)
{
	path <<= p;
	committed = p;
	int best = -1;
	for(int i = 0; i < mounts.GetCount(); i++) {
		const String& d = mounts[i].dir;
		if(IsUnder(p, d) && (best < 0 || d.GetCount() > mounts[best].dir.GetCount()))
			best = i;
	}
	if(best < 0)
		return;
	disk <<= mounts[best].disk;
	if(disk.GetIndex() < 0)
		disk.SetIndex(0);
	FillMounts();
	mount <<= mounts[best].dir;
}

String PathPicker::Get() const
{
	return ~path;
}

// A remote tree has no local disks; only the path field stays editable.
void PathPicker::Remote(bool r)
{
	remote = r;
	disk.Enable(!r);
	mount.Enable(!r);
	browse.Enable(!r);
}
