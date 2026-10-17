#ifndef TREESYNC_TREESYNC_H
#define TREESYNC_TREESYNC_H

#include <CtrlLib/CtrlLib.h>

#include <filesystem>

#include "Contracts.h"

#ifndef PLATFORM_LINUX
#error "TreeSync is Linux-only for now"
#endif

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

// Copy:   copy new/changed files source -> target, never delete    (--copy,   robocopy /E)
// Mirror: make target match source, deleting extras                (--mirror, robocopy /MIR)
// Merge:  copy changes both ways; changed on both sides = conflict (--merge)
enum class SyncMode { Copy, Mirror, Merge };

// Sync switches; Options.cpp maps each to its --long-option (and robocopy switch).
// Content is compared by SHA-256; size + time decide whether a cached hash can be
// reused and, in Merge mode, which side is newer, so the time switches still matter.
struct SyncFlags {
	SyncMode mode = SyncMode::Mirror;
	bool   dry_run = true;
	bool   fat_file_times = false;   // 2-second time tolerance
	bool   dst_tolerance = false;    // allow exact 1-hour shifts
	bool   skip_links = true;
	bool   include_same = false;
	bool   skip_older = false;
	bool   skip_newer = false;
	bool   skip_changed = false;
	bool   keep_extra = false;
	bool   skip_source_only = false;
	String exclude_files;            // globs, ';'-separated
	String exclude_dirs;             // globs, ';'-separated
	int    retries = 3;
	int    retry_wait = 5;           // seconds
	int    threads = 8;
};

struct BoolOption {
	const char *name;                // --name / --no-name
	bool SyncFlags::*field;
	const char *robocopy;
	const char *text;
};

struct IntOption {
	const char *name;                // --name=N
	int SyncFlags::*field;
	const char *robocopy;
	const char *text;
	int min, max;
};

constexpr int BOOL_OPTION_COUNT = 10;
constexpr int INT_OPTION_COUNT = 3;
extern const BoolOption BOOL_OPTIONS[];
extern const IntOption  INT_OPTIONS[];

struct SyncSetup {
	TreeEndpoint source;
	TreeEndpoint target;
	SyncFlags    flags;
};

enum { PARSE_OK, PARSE_HELP, PARSE_ERROR };

int            ParseCommandLine(const Vector<String>& args, SyncSetup& s, String& error);
Vector<String> FlagArgs(const SyncFlags& f);
Vector<String> ToArgs(const SyncSetup& s);
String         CommandLineText(const Vector<String>& args);   // shell-quoted, ready to paste
String         Usage();

// A physical disk (sda, nvme0n1).
struct DiskInfo : Moveable<DiskInfo> {
	String id;
	String label;
};

// A mounted filesystem: the folder it appears at and the disk it lives on.
struct MountInfo : Moveable<MountInfo> {
	String dir;
	String disk;    // DiskInfo::id or DISK_NETWORK
	String label;
};

inline const char *DISK_NETWORK = "*network";

Vector<DiskInfo>  ListDisks();
Vector<MountInfo> ListMounts();
bool              IsUnder(const String& p, const String& dir);

// FsPath.cpp: long-path and UTF-8 safe conversions and folder listing.
struct FolderEntry : Moveable<FolderEntry> {
	String name;
	bool   is_dir = false;    // follows symlinks
	bool   is_link = false;
};

std::filesystem::path ToFs(const String& p);
String                FromFs(const std::filesystem::path& p);
bool                  ListFolder(const String& dir, Vector<FolderEntry>& out, String& error);
String                CleanPath(String p);

// Disk -> Mount -> Path, with a browse button.
class PathPicker : public ParentCtrl {
	Label      lbl_disk, lbl_mount, lbl_path;
	DropList   disk, mount;
	EditString path;
	Button     browse;
	Vector<MountInfo> mounts;
	String     committed;   // last accepted path, restored when a path is refused
	bool       remote = false;

	bool Accept(const String& p);
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
		Label      lbl_mode, lbl_xf, lbl_xd, preview;
		Switch     mode;
		Option     opt[BOOL_OPTION_COUNT];
		Label      lbl_int[INT_OPTION_COUNT];
		EditInt    num[INT_OPTION_COUNT];
		EditString xf, xd;

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

bool   IsRoot();
String HomeDir();
bool   PathAllowed(const String& p);   // always true as root; else only inside HomeDir()
void   WarnNotRoot();
void   RefusePath(const String& p);

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

public:
	void SetSetup(const SyncSetup& s);

private:

	void MainMenu(Bar& bar);
	void FileMenu(Bar& bar);
	void ViewMenu(Bar& bar);
	void EditSetup();
	void LoadTrees();

public:
	TreeSyncWindow();
};

#endif
