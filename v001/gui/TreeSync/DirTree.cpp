// Directory tree view. Folders load lazily the first time they are expanded.
// Local trees are listed directly. A remote tree is listed over SFTP by a worker
// thread that owns the connection (SFTP calls can take a second or more on a busy
// server, and the window must not freeze); results come back with PostCallback.
// Browsing runs as the GUI's user, with the same known_hosts check and login order
// as a sync. No hashing here: this is just the browsing view.
#include "TreeSync.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <pthread.h>

// Shared between the tree (GUI thread) and its worker. The worker is detached, so
// a slow connect never blocks closing the window; `alive` (GUI thread only) tells
// late results that the tree they were for is gone or was reloaded.
struct DirTree::RemoteJob {
	std::shared_ptr<Remote> r;
	DirTree                *tree;
	TreeEndpoint            e;
	String                  known_hosts;
};

struct DirTree::Remote {
	std::mutex                          m;
	std::condition_variable             cv;
	std::deque<std::pair<int, String>>  queue;   // (node id, rel)
	bool                                quit = false;
	bool                                alive = true;
};

static std::atomic<int> live_workers;

void DirTree::WaitForWorkers(double seconds)
{
	for(int i = 0; live_workers > 0 && i < seconds * 50; i++)
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
}

DirTree::DirTree()
{
	WhenOpen = [this](int id) { LoadChildren(id); };
}

DirTree::~DirTree()
{
	StopRemote();
}

void DirTree::StopRemote()
{
	if(!remote)
		return;
	remote->alive = false;
	{
		std::lock_guard<std::mutex> lock(remote->m);
		remote->quit = true;
	}
	remote->cv.notify_all();
	remote.reset();
}

void DirTree::Load(const char *caption, const TreeEndpoint& e, bool follow, const String& known_hosts)
{
	StopRemote();
	Clear();
	follow_links = follow;
	is_remote = e.remote;
	if(e.path.IsEmpty()) {
		SetRoot(CtrlImg::Dir(), Null, String(caption) + ": (not set)");
		return;
	}
	if(!e.remote) {
		SetRoot(Node(CtrlImg::Dir(), e.path, String(caption) + ": " + e.path).CanOpen());
		Open(0);
		return;
	}

	String where = Nvl(e.user, GetUserName()) + "@" + e.host + ":" + e.path;
	root_label = String(caption) + ": " + where;
	SetRoot(Node(CtrlImg::Network(), "/", root_label + "   (connecting...)").CanOpen());
	Add(0, Null, Null, "(connecting to " + e.host + "...)");

	// Remote node keys are "/" + rel, so the root ("/") is never an empty, Null key.
	auto r = std::make_shared<Remote>();
	remote = r;
	auto job = new RemoteJob{ r, this, clone(e), known_hosts };
	live_workers++;
	pthread_t t;
	if(pthread_create(&t, nullptr, RemoteWorker, job) == 0)
		pthread_detach(t);
	else {
		delete job;
		live_workers--;
		Connected(false, "cannot start a thread for the remote tree");
	}
}

// The worker owns its job and frees everything itself before it counts itself
// out, so WaitForWorkers really means "all freed". A plain pthread for the same
// reason: std::thread frees its own state only after the function has returned.
void *DirTree::RemoteWorker(void *arg)
{
	struct Done { ~Done() { live_workers--; } } done;   // destroyed last
	One<RemoteJob> job((RemoteJob *)arg);
	std::shared_ptr<Remote> r = job->r;
	DirTree *tree = job->tree;
	{
		SftpFs fs(job->e, job->known_hosts, false);
		String error;
		bool ok = fs.Connect(error);
		Upp::PostCallback([r, tree, ok, error] {
			if(r->alive)
				tree->Connected(ok, error);
		});
		for(;;) {
			std::pair<int, String> item;
			{
				std::unique_lock<std::mutex> lock(r->m);
				r->cv.wait(lock, [&] { return r->quit || !r->queue.empty(); });
				if(r->quit)
					break;
				item = r->queue.front();
				r->queue.pop_front();
			}
			Vector<FolderEntry> entries;
			String err;
			if(!ok)
				err = "not connected";
			else
				fs.List(item.second, entries, err);
			Upp::PostCallback([r, tree, id = item.first, entries = pick(entries), err]() mutable {
				if(r->alive)
					tree->Listed(id, entries, err);
			});
		}
	}
	job.Clear();
	r.reset();
	return nullptr;
}

void DirTree::Connected(bool ok, const String& error)
{
	Set(0, "/", root_label + (ok ? String() : "   (not connected)"));
	if(ok) {
		RemoveChildren(0);
		Add(0, Null, Null, "(loading...)");
		Request(0, "");
		Open(0);
	}
	else {
		RemoveChildren(0);
		Add(0, Null, Null, "(" + error + ")");
	}
}

void DirTree::Request(int id, const String& rel)
{
	if(!remote)
		return;
	{
		std::lock_guard<std::mutex> lock(remote->m);
		remote->queue.emplace_back(id, rel);
	}
	remote->cv.notify_one();
}

// Folders first, then links that are not followed, then files; each sorted.
void DirTree::Fill(int id, const String& parent_key, const Vector<FolderEntry>& entries)
{
	Vector<String> dirs, links, files;
	for(const FolderEntry& e : entries) {
		String child = is_remote ? String() : AppendFileName(parent_key, e.name);
		// --follow-links off, or (local) a link leaving home without root: never descended into
		if(e.is_dir && e.is_link && (!follow_links || (!is_remote && !PathAllowed(child))))
			links.Add(e.name);
		else if(e.is_dir)
			dirs.Add(e.name);
		else
			files.Add(e.name);
	}
	Upp::Sort(dirs);
	Upp::Sort(links);
	Upp::Sort(files);

	for(const String& d : dirs) {
		String key = is_remote ? (parent_key == "/" ? "/" + d : parent_key + "/" + d)
		                       : AppendFileName(parent_key, d);
		Add(id, CtrlImg::Dir(), key, d, true);
	}
	for(const String& l : links)
		Add(id, CtrlImg::Dir(), Null, l + " ->");
	for(const String& f : files)
		Add(id, CtrlImg::File(), Null, f);
}

void DirTree::Listed(int id, const Vector<FolderEntry>& entries, const String& error)
{
	if(!IsValid(id))
		return;
	RemoveChildren(id);
	if(error.GetCount())
		Add(id, Null, Null, "(" + error + ")");
	else
		Fill(id, String(Get(id)), entries);
}

void DirTree::LoadChildren(int id)
{
	String key = Get(id);
	if(GetChildCount(id) || key.IsEmpty())
		return;

	if(is_remote) {
		Add(id, Null, Null, "(loading...)");
		Request(id, key.Mid(1));   // drop the leading "/"
		return;
	}
	Vector<FolderEntry> entries;
	String error;
	if(!ListFolder(key, entries, error)) {
		Add(id, Null, Null, "(" + error + ")");
		return;
	}
	Fill(id, key, entries);
}

// [+]/[-] on screen come from TreeCtrl; this adds the same on the keyboard,
// both keypad and main-row + and -.
bool DirTree::Key(dword key, int count)
{
	int id = GetCursor();
	if(id >= 0) {
		if(key == K_ADD || key == '+') {
			Open(id);
			return true;
		}
		if(key == K_SUBTRACT || key == '-') {
			Close(id);
			return true;
		}
	}
	return TreeCtrl::Key(key, count);
}

void DirTree::CollapseAll()
{
	for(int i = 0; i < GetChildCount(0); i++)
		CloseDeep(GetChild(0, i));
}
