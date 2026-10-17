// Disks and mount points for the path picker.
// Physical disks come from /sys/block; mounts from /proc/self/mounts, each traced
// back to its disk through partitions and device-mapper (LVM, LUKS) slaves.
// std::filesystem does the path work; it cannot enumerate mounts, so getmntent does.
#include "TreeSyncCore.h"

#include <fstream>

#include <mntent.h>

namespace fs = std::filesystem;

static String ReadSys(const fs::path& p)
{
	std::ifstream in(p);
	std::string line;
	std::getline(in, line);
	return TrimBoth(String(line));
}

// /dev/mapper/vg-root -> dm-0 -> nvme0n1p3 -> nvme0n1
static std::string DiskOfBlock(const std::string& name, int depth = 0)
{
	std::error_code ec;
	if(depth > 8 || name.empty())
		return {};
	fs::path sys = fs::path("/sys/class/block") / name;
	if(fs::exists(sys / "partition", ec))
		return fs::canonical(sys, ec).parent_path().filename().string();
	for(const fs::directory_entry& slave : fs::directory_iterator(sys / "slaves", ec))
		return DiskOfBlock(slave.path().filename().string(), depth + 1);
	if(fs::exists(fs::path("/sys/block") / name, ec))
		return name;
	return {};
}

static bool IsNetworkFs(const String& type)
{
	static const char *net[] = { "nfs", "nfs4", "cifs", "smb3", "fuse.sshfs", "fuse.rclone" };
	for(const char *t : net)
		if(type == t)
			return true;
	return false;
}

Vector<DiskInfo> ListDisks()
{
	Vector<DiskInfo> list;
	std::error_code ec;
	for(const fs::directory_entry& e : fs::directory_iterator("/sys/block", ec)) {
		String id = e.path().filename().string();
		if(id.StartsWith("loop") || id.StartsWith("ram") || id.StartsWith("zram") ||
		   id.StartsWith("dm-"))
			continue;
		DiskInfo& d = list.Add();
		d.id = id;
		d.label = id;
		String model = ReadSys(e.path() / "device" / "model");
		if(model.GetCount())
			d.label << "  " << model;
		int64 sectors = ScanInt64(ReadSys(e.path() / "size"));
		if(!IsNull(sectors) && sectors > 0)
			d.label << "  (" << FormatBytes(sectors * 512) << ")";
		if(ReadSys(e.path() / "removable") == "1")
			d.label << "  removable";
	}
	Sort(list, [](const DiskInfo& a, const DiskInfo& b) { return a.id < b.id; });
	return list;
}

Vector<MountInfo> ListMounts()
{
	Vector<MountInfo> list;
	Index<String> seen;
	FILE *f = setmntent("/proc/self/mounts", "r");
	if(!f)
		return list;
	while(mntent *m = getmntent(f)) {
		String dir = m->mnt_dir;
		String type = m->mnt_type;
		String dev = m->mnt_fsname;
		String disk;
		if(IsNetworkFs(type))
			disk = DISK_NETWORK;
		else if(dev.StartsWith("/dev/") && !dev.StartsWith("/dev/loop")) {
			std::error_code ec;
			fs::path real = fs::canonical(ToFs(dev), ec);
			disk = DiskOfBlock((ec ? ToFs(dev) : real).filename().string());
		}
		if(disk.IsEmpty() || seen.Find(dir) >= 0)
			continue;
		seen.Add(dir);
		MountInfo& mi = list.Add();
		mi.dir = dir;
		mi.disk = disk;
		mi.label = dir + "  (" + type;
		std::error_code ec;
		fs::space_info sp = fs::space(ToFs(dir), ec);
		if(!ec)
			mi.label << ", " << FormatBytes((int64)sp.capacity);
		mi.label << ")";
	}
	endmntent(f);
	Sort(list, [](const MountInfo& a, const MountInfo& b) { return a.dir < b.dir; });
	return list;
}

// 1024-based, one decimal, IEC units as dnf uses them: "953.9 GiB".
String FormatBytes(int64 n)
{
	static const char *unit[] = { "B", "KiB", "MiB", "GiB", "TiB", "PiB" };
	double v = (double)n;
	int u = 0;
	while(v >= 1024 && u < 5) {
		v /= 1024;
		u++;
	}
	return u ? Format("%.1f %s", v, unit[u]) : Format("%d B", (int)n);
}

// True when p is dir itself or somewhere below it (/home is not under /hom).
bool IsUnder(const String& p, const String& dir)
{
	fs::path rel = ToFs(p).lexically_normal().lexically_relative(ToFs(dir).lexically_normal());
	return !rel.empty() && *rel.begin() != "..";
}
