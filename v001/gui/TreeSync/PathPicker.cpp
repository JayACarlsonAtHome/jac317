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

	// TreeSyncHome only offers the home directory, on the disk that holds it; TreeSyncRoot
	// (its runs are root's) offers every disk. The title says which one this is.
	String home_disk;
	if(!run_as_root) {
		String home = HomeDir();
		int best = -1;
		for(int i = 0; i < mounts.GetCount(); i++)
			if(IsUnder(home, mounts[i].dir) &&
			   (best < 0 || mounts[i].dir.GetCount() > mounts[best].dir.GetCount()))
				best = i;
		MountInfo h;
		h.dir = home;
		h.disk = best >= 0 ? mounts[best].disk : String();
		h.label = home;
		home_disk = h.disk;
		mounts.Clear();
		mounts.Add(h);
	}

	for(const DiskInfo& d : ListDisks())
		if(run_as_root || d.id == home_disk)
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

// A remote machine's folders, listed over SFTP the way the main window's trees list
// them (same login and known_hosts check): pick one. Starts at "/".
bool PickRemoteFolder(TreeEndpoint e, const String& known_hosts, String& path)
{
	if(e.host.IsEmpty()) {
		PromptOK("Enter the remote machine's host first.");
		return false;
	}
	TopWindow dlg;
	DirTree   tree;
	Label     hint;
	Button    ok, cancel;
	dlg.Title("Remote folder: " + Nvl(e.user, GetUserName()) + "@" + e.host).Sizeable();
	dlg.SetRect(0, 0, Zx(520), Zy(440));
	dlg.Add(tree.HSizePosZ(8, 8).VSizePosZ(8, 44));
	dlg.Add(hint.SetLabel("Open the folders, select the tree root, then OK.").LeftPosZ(8, 300).BottomPosZ(12, 19));
	dlg.Add(ok.SetLabel("OK").RightPosZ(100, 84).BottomPosZ(8, 26));
	dlg.Add(cancel.SetLabel("Cancel").RightPosZ(8, 84).BottomPosZ(8, 26));
	ok.Ok() << dlg.Acceptor(IDOK);
	cancel.Cancel() << dlg.Rejector(IDCANCEL);
	e.path = "/";
	tree.Load("Folders", e, false, known_hosts);
	if(dlg.Execute() != IDOK)
		return false;
	String rel;
	bool folder = false;
	if(!tree.Selected(rel, folder))
		return false;
	if(!folder)   // a file: its folder
		rel = GetFileFolder(rel);
	path = "/" + rel;
	return true;
}

void PathPicker::Browse()
{
	if(remote) {
		String p = ~path;
		if(WhenRemoteBrowse && WhenRemoteBrowse(p)) {
			Set(p);
			WhenPath();
		}
		return;
	}
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
	if(remote || run_as_root || PathAllowed(p))   // the home limit is for local trees run as this user
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
// A remote tree has no local disks: the boxes say so, and only the path field stays.
void PathPicker::Remote(bool r)
{
	if(r && !remote) {
		disk.ClearList();
		disk.Add(Null, "Remote machine (host and user: Setup)");
		disk.SetIndex(0);
		mount.ClearList();
		mount.Add(Null, "(remote)");
		mount.SetIndex(0);
	}
	else if(!r && remote) {
		LoadDisks();
		remote = r;
		Set(committed);
	}
	remote = r;
	disk.Enable(!r);
	mount.Enable(!r);
	browse.Enable(!r || WhenRemoteBrowse);
}
