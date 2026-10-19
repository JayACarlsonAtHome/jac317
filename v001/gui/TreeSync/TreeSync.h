#ifndef TREESYNC_TREESYNC_H
#define TREESYNC_TREESYNC_H

// The sync GUI, a presenter on top of TreeSyncCore, which does the real work. Two
// programs show it: TreeSyncRoot (gui/TreeSyncRoot: every sync as root, through
// pkexec) and TreeSyncHome (gui/TreeSyncHome: as the user, inside their home). It only
// syncs; building is TreeSyncBuild's, installing TreeSyncInstall's.
#include <TreeSyncUi/TreeSyncUi.h>

#include <mutex>
#include <thread>
#include <sys/types.h>

// TreeSyncRoot: treesync-cli runs through pkexec, so local paths outside home are
// allowed even though the GUI itself is not root. TreeSyncHome: false.
extern bool run_as_root;
String CompilerTag();                  // "GCC" or "Clang": which compiler built this program
String AppName();                      // "TreeSyncRoot-GCC", "TreeSyncHome-Clang", ...
// The treesync-cli this program runs: next to it, treesync-cli-<gcc|clang> when installed
// (install.sh puts both compilers' side by side), else treesync-cli (the build in out/).
String OwnCli();
void   TreeSyncMain(bool root_app);    // the whole program; root_app: TreeSyncRoot

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
		Label        lbl_mode, lbl_owners, lbl_if, lbl_id, lbl_xf, lbl_xd, preview;
		Switch       mode;
		Switch       owners;        // off / by name / by number: one choice, not two boxes
		Label        lbl_reset;
		Button       reset[3];      // defaults for Copy, Mirror, Merge
		Option       opt[BOOL_OPTION_COUNT];
		Label        lbl_int[INT_OPTION_COUNT];
		EditInt      num[INT_OPTION_COUNT];
		EditString   inf, ind, xf, xd;
		RichTextView help;          // what the option just clicked does
		Size         need;          // the size the layout needs, in pixels

		void ShowHelp(const String& title, const String& text);
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
	bool Key(dword key, int count) override;   // F1: the manual's Run chapter
	SetupDlg();
	void      Set(const SyncSetup& s);
	SyncSetup Get() const;
	void      Screenshots(const String& dir);   // --screenshot: each tab as a PNG
};

// The command line shown in the GUI leaves out --dry-run / --no-dry-run: in the
// GUI the Sync menu decides between a real run and a dry run.
Vector<String> WithoutDryRun(Vector<String> args);

// History.cpp: the runs in the database, what each did, and log files.
void   ShowHistory(const String& db_path);
void   HistoryScreenshot(const String& db_path, const String& png);
void   ShowLogFile(const String& path);
void   ExportForAi(const String& db_path, int64 run, bool logs, bool sql);   // run 0: the newest
// TreeSyncRoot: a treesync-cli only root can change (the installed one); note: what to
// tell the user about it (it differs from this build, or none is installed).
String RootCli(String& note);
String LatestLog(const String& db_path);

// Messages.cpp: core messages shown in a dialog and printed to the terminal.
void   RefusePath(const String& p);

// Browsable view of one tree, local or over SFTP, with [+]/[-] expanders and
// +/- keys. Remote folders are listed by a worker thread (DirTree.cpp).
class DirTree : public TreeCtrl {
	struct Remote;
	struct RemoteJob;
	std::shared_ptr<Remote> remote;
	static void *RemoteWorker(void *job);
	bool   follow_links = false;
	bool   is_remote = false;
	String root_label;
	String root_key;      // the root node's key: a node's path in the tree is its key past it

	// Checkboxes (the source tree): what is checked is synced, a folder with everything
	// below it. The model is the set of checked paths (CheckedPaths); each box shows
	// checked, partly checked (something below is) or not, worked out from it, so
	// folders not listed yet need no state of their own.
	bool                  checks = false;
	CheckedPaths          checked;
	ArrayMap<int, Option> boxes;    // node id -> its box

	void   AddNode(int parent, const Image& img, const String& key, const String& label, bool folder);
	void   SetLabel(int id, const String& key, const String& label);
	String Key(int id) const   { return GetNode(id).key; }   // Get() gives a box's value, not the key
	String Rel(int id) const;
	void   Toggle(int id);
	void   ShowChecks();
	void   DropBoxes(int id);                // the boxes below id, before its children go

	void LoadChildren(int id);
	void Fill(int id, const String& parent_key, const Vector<FolderEntry>& entries);
	void Request(int id, const String& rel);
	void Connected(bool ok, const String& error);
	void Listed(int id, const Vector<FolderEntry>& entries, const String& error);
	void StopRemote();

public:
	Event<> WhenChecks;                      // a box was clicked: GetChecked has changed

	DirTree& Checks(bool b = true)           { checks = b; return *this; }
	void     SetChecked(const String& include_paths)   { checked.Set(include_paths); }   // before Load
	String   GetChecked() const              { return checked.Get(); }   // for --include-path
	bool     NothingChecked() const          { return checks && checked.Nothing(); }

	DirTree();
	~DirTree();
	void Load(const char *caption, const TreeEndpoint& e, bool follow_links, const String& known_hosts);
	// At exit: give remote workers up to `seconds` to close their connections and
	// free their memory (a worker in the middle of a network call may take longer).
	static void WaitForWorkers(double seconds);
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

	// Thermometers: the whole run, and one row per file being copied right now.
	// They sit above the log and are shown only while a sync runs.
	Splitter          bottom;          // progress above the log; only the log when idle
	// Three boxes, one per stage of a streamed run, then the files being copied.
	ParentCtrl        progress_pane;
	Splitter          stages;
	ParentCtrl        stage_pane[3];
	LabelBox          stage_box[3];
	ProgressIndicator stage_bar[3];   // [1] is the copy box: bytes of the run
	Label             stage_text[3];
	ArrayCtrl         active;

	void StageShow(int stage, int64 done, int64 total, const String& text);

	// Upper right, in the menu bar, while a sync runs: how long it has run, how long
	// the whole run will probably take, and when it should be done. The rate is that
	// of the last minute, so a resume or a slow start does not skew it.
	Label                         eta;
	Vector<Tuple<double, int64>>  eta_samples;   // (seconds since the start, bytes done), about 1 a second
	int64                         eta_done = 0, eta_total = 0;
	std::chrono::steady_clock::time_point eta_t0;
	void EtaSample(int64 done, int64 total);
	void ShowEta();

	// "Still working": a 1-second ticker while a sync runs. It keeps each file's Time
	// counting and, when no progress has come for a few seconds, says so in the
	// Copy box, so a big file on a slow link never looks stuck.
	std::chrono::steady_clock::time_point last_progress;
	VectorMap<int, std::chrono::steady_clock::time_point> copy_started;   // copy id -> first seen
	String copy_text;            // the Copy box's own line, before any "still working" note
	void Tick();
	// The connection to a remote side is lost and treesync-cli waits for it (@net).
	bool        prescan_done = false;   // the copy total stops growing then
	int64       sync_started = 0;       // time(): for "it took ..." at the end
	bool        sync_dry = false;
	bool        closing = false;        // quitting: no popup
	bool        net_down = false;
	String      net_host;
	int         net_wait = 0;    // seconds it waits before stopping
	std::chrono::steady_clock::time_point net_since;
	SyncSetup setup;

	// SyncRun.cpp: treesync-cli as a child process.
	pid_t       child = 0;
	int         child_stdin = -1;
	pid_t       session = 0;        // TreeSyncRoot: pkexec treesync-cli --root-session, for the window
	int         session_fd = -1;    // its socket: runs go through it (one password per window)
	bool        SessionStart(const String& cli);
	void        SessionEnd();
	bool&       as_root = run_as_root;
	std::thread reader;

	void Log(const String& line);
	void LogText(const String& text);   // several lines at once; the pane keeps the newest 100

	// Lines from treesync-cli wait here; the GUI takes them in batches (Drain), so
	// a run with tens of thousands of lines never floods the event queue.
	std::mutex     inbox_m;
	Vector<String> inbox;
	bool           inbox_posted = false;
	void Drain();
	void ShowProgress(const String& line);   // an @file or @overall line from treesync-cli
	void ProgressVisible(bool on);
	// extra: "" a sync, or --scan-only, --sync-attrs, --clear-old (one of the checks).
	String      run_extra;
	void StartSync(bool dry_run, const String& extra = String());
	void StageLabels(const String& extra);
	bool ConfirmRun(const SyncSetup& s);
	bool ConfirmExtra(const SyncSetup& s, const String& extra);
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
	void DemoProgress();   // --screenshot: the progress boxes with sample values
	void DemoScanProgress();
	double BenchLog(int n);   // --bench-log: seconds to take in n lines of child output
	void Close() override;

	TreeSyncWindow();
	~TreeSyncWindow();
};

#endif
