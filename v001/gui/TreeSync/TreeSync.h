#ifndef TREESYNC_TREESYNC_H
#define TREESYNC_TREESYNC_H

// TreeSync GUI: a presenter on top of TreeSyncCore, which does the real work.
#include <CtrlLib/CtrlLib.h>
#include <TreeSyncCore/TreeSyncCore.h>

#include <thread>
#include <sys/types.h>

// Sync > Run as root: treesync-cli runs through pkexec, so local paths outside
// home are allowed even though the GUI itself is not root.
extern bool run_as_root;

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
		Label      lbl_host, lbl_port, lbl_user, lbl_key, lbl_keys;
		PathPicker path;
		Option     remote;
		EditString host;
		EditInt    port;
		EditString user;
		EditString key_file;

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

// Messages.cpp: core messages shown in a dialog and printed to the terminal.
void   WarnNotRoot();
void   RefusePath(const String& p);

// Browsable view of one tree, local or over SFTP, with [+]/[-] expanders and
// +/- keys. Remote folders are listed by a worker thread (DirTree.cpp).
class DirTree : public TreeCtrl {
	struct Remote;
	std::shared_ptr<Remote> remote;
	bool   follow_links = false;
	bool   is_remote = false;
	String root_label;

	void LoadChildren(int id);
	void Fill(int id, const String& parent_key, const Vector<FolderEntry>& entries);
	void Request(int id, const String& rel);
	void Connected(bool ok, const String& error);
	void Listed(int id, const Vector<FolderEntry>& entries, const String& error);
	void StopRemote();

public:
	DirTree();
	~DirTree();
	void Load(const char *caption, const TreeEndpoint& e, bool follow_links, const String& known_hosts);
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
	Splitter  split, vsplit;
	TreePanel source_panel, target_panel;
	LineEdit  log;
	SyncSetup setup;

	// SyncRun.cpp: treesync-cli as a child process.
	pid_t       child = 0;
	int         child_stdin = -1;
	bool&       as_root = run_as_root;
	std::thread reader;

	void Log(const String& line);
	void StartSync(bool dry_run);
	void StopSync();
	void SyncFinished(int code);

	void ShowStatus();
	void MainMenu(Bar& bar);
	void FileMenu(Bar& bar);
	void ViewMenu(Bar& bar);
	void SyncMenu(Bar& bar);
	void EditSetup();
	void LoadTrees();

public:
	void SetSetup(const SyncSetup& s);
	void Close() override;

	TreeSyncWindow();
	~TreeSyncWindow();
};

#endif
