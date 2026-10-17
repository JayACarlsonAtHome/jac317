// Disk -> Mount -> Path selector. Picking a disk narrows the mount list; picking a
// mount sets the path to it; "..." browses from the current path.
#include "TreeSync.h"

#ifdef PLATFORM_WIN32
static const char *MOUNT_LABEL = "Drive";
#else
static const char *MOUNT_LABEL = "Mount";
#endif

PathPicker::PathPicker()
{
	Add(lbl_disk.SetLabel("Disk").LeftPosZ(0, 40).TopPosZ(0, 19));
	Add(disk.HSizePosZ(42, 0).TopPosZ(0, 19));
	Add(lbl_mount.SetLabel(MOUNT_LABEL).LeftPosZ(0, 40).TopPosZ(24, 19));
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
	for(const DiskInfo& d : ListDisks())
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
	fs.ActiveDir(std::filesystem::is_directory(start.ToStd(), ec) ? start : String(~mount));
	if(!fs.ExecuteSelectDir("Tree root"))
		return;
	Set(~fs);
	WhenPath();
}

void PathPicker::Commit()
{
	Set(~path);
	WhenPath();
}

// Select the disk and mount that hold p: the mount with the longest matching prefix.
void PathPicker::Set(const String& p)
{
	path <<= p;
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
	disk.Enable(!r);
	mount.Enable(!r);
	browse.Enable(!r);
}
