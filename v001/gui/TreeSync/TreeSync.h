#ifndef TREESYNC_TREESYNC_H
#define TREESYNC_TREESYNC_H

#include <CtrlLib/CtrlLib.h>

#include <filesystem>

#include "Contracts.h"

// U++'s stock build methods pass -std=c++17; build with v001/gui/bm/*.bm.
static_assert(__cplusplus > 202302L, "TreeSync requires C++26: build with v001/gui/bm/CLANG26.bm or GCC15.bm");

using namespace Upp;

// One side of the sync: a directory tree, local or on a remote machine.
struct TreeEndpoint {
	String path;
	bool   remote = false;
	String host;
	int    port = 22;
	String user;
	bool   use_key = true;
	String key_file;
	String password;   // held in memory only, never written to the SQLite cache
};

// Copy:   copy new/changed files source -> target, never delete    (robocopy /E)
// Mirror: make target match source, deleting extras                (robocopy /MIR)
// Merge:  copy changes both ways; changed on both sides = conflict (no robocopy equivalent)
enum class SyncMode { Copy, Mirror, Merge };

// Options named after their robocopy switches. Content is compared by SHA-256;
// size + time decide whether a cached hash can be reused, and in Merge mode which
// side is newer, so the time flags (/FFT, /DST) still matter.
struct SyncFlags {
	SyncMode mode = SyncMode::Mirror;
	bool   list_only = true;      // /L   report only, change nothing
	bool   fft = false;           // /FFT FAT file times: 2-second tolerance
	bool   dst = false;           // /DST tolerate exact 1-hour (DST) differences
	bool   xo = false;            // /XO  skip if source is older than target
	bool   xn = false;            // /XN  skip if source is newer than target
	bool   xc = false;            // /XC  skip changed files (same time, different hash)
	bool   xx = false;            // /XX  leave extra target files alone (never purge)
	bool   xl = false;            // /XL  skip files that exist only in source
	bool   xj = true;             // /XJ  do not follow symlinks / junctions
	bool   is = false;            // /IS  include same files (rehash / recopy)
	String xf;                    // /XF  exclude file globs, ';'-separated
	String xd;                    // /XD  exclude directory globs, ';'-separated
	int    retries = 3;           // /R:n
	int    wait = 5;              // /W:n seconds between retries
	int    threads = 8;           // /MT:n

	String ToString() const;      // robocopy-style switch summary
};

struct SyncSetup {
	TreeEndpoint source;
	TreeEndpoint target;
	SyncFlags    flags;
};

// Linux: a physical disk (sda, nvme0n1). Windows: not used.
struct DiskInfo : Moveable<DiskInfo> {
	String id;
	String label;
};

// A mounted filesystem: the folder it appears at and the disk it lives on.
// On Windows, a drive letter.
struct MountInfo : Moveable<MountInfo> {
	String dir;
	String disk;    // DiskInfo::id, DISK_NETWORK, or empty on Windows
	String label;
};

inline const char *DISK_NETWORK = "*network";

Vector<DiskInfo>  ListDisks();
Vector<MountInfo> ListMounts();
bool              IsUnder(const String& p, const String& dir);

// Disk -> Mount (Drive on Windows) -> Path, with a browse button.
class PathPicker : public ParentCtrl {
	Label      lbl_disk, lbl_mount, lbl_path;
	DropList   disk, mount;
	EditString path;
	Button     browse;
	Vector<MountInfo> mounts;

	void FillMounts();
	void PickMount();
	void Browse();
	void Commit();

public:
	Event<> WhenPath;   // path changed by mount choice, browse, or Enter

	PathPicker();
	void   LoadDisks();
	void   Set(const String& p);
	String Get() const;
	void   Remote(bool r);

	static constexpr int Height = 67;   // three rows, in Zy units
};

// Second screen: tree parameters and remote login for both sides, plus flags.
class SetupDlg : public TopWindow {
	struct EndpointPane : ParentCtrl {
		Label      lbl_host, lbl_port, lbl_user, lbl_key, lbl_pass;
		PathPicker path;
		Option     remote;
		EditString host;
		EditInt    port;
		EditString user;
		Option     use_key;
		EditString key_file;
		EditString password;

		EndpointPane();
		void Sync();
		void Set(const TreeEndpoint& e);
		TreeEndpoint Get() const;
	};

	struct FlagsPane : ParentCtrl {
		Label      lbl_mode, lbl_xf, lbl_xd, lbl_r, lbl_w, lbl_mt, preview;
		Switch     mode;
		Option     list_only, fft, dst, xo, xn, xc, xx, xl, xj, is;
		EditString xf, xd;
		EditInt    retries, wait, threads;

		FlagsPane();
		void Sync();
		void Set(const SyncFlags& f);
		SyncFlags Get() const;
	};

	TabCtrl      tabs;
	EndpointPane source, target;
	FlagsPane    flags;
	Button       ok, cancel;

public:
	SetupDlg();
	void      Set(const SyncSetup& s);
	SyncSetup Get() const;
};

// Browsable view of one tree, with [+]/[-] expanders and +/- keys.
class DirTree : public TreeCtrl {
	bool follow_links = false;

	void LoadChildren(int id);

public:
	DirTree();
	void Load(const char *caption, const TreeEndpoint& e, bool skip_links);
	void CollapseAll();
	bool Key(dword key, int count) override;
};

// One side of the main window: path picker above its tree.
struct TreePanel : ParentCtrl {
	PathPicker picker;
	DirTree    tree;

	TreePanel();
};

class TreeSyncWindow : public TopWindow {
	MenuBar   menu;
	StatusBar status;
	Splitter  split;
	TreePanel source_panel, target_panel;
	SyncSetup setup;

	void ShowStatus();

	void MainMenu(Bar& bar);
	void FileMenu(Bar& bar);
	void ViewMenu(Bar& bar);
	void EditSetup();
	void LoadTrees();

public:
	TreeSyncWindow();
};

#endif
