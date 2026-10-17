// Directory tree view. Folders load lazily the first time they are expanded.
// Local trees only for now; a remote endpoint shows a placeholder until the
// transport exists. No hashing here: this is just the browsing view.
#include "TreeSync.h"

DirTree::DirTree()
{
	WhenOpen = [this](int id) { LoadChildren(id); };
}

void DirTree::Load(const char *caption, const TreeEndpoint& e, bool skip_links)
{
	Clear();
	follow_links = !skip_links;
	if(e.path.IsEmpty()) {
		SetRoot(CtrlImg::Dir(), Null, String(caption) + ": (not set)");
		return;
	}
	if(e.remote) {
		SetRoot(CtrlImg::Network(), Null,
		        String(caption) + ": " + e.user + "@" + e.host + ":" + e.path);
		Add(0, Null, Null, "(remote browsing not implemented yet)");
		Open(0);
		return;
	}
	SetRoot(Node(CtrlImg::Dir(), e.path, String(caption) + ": " + e.path).CanOpen());
	Open(0);
}

void DirTree::LoadChildren(int id)
{
	String dir = Get(id);
	if(GetChildCount(id) || dir.IsEmpty())
		return;

	namespace fs = std::filesystem;
	Vector<String> dirs, links, files;
	std::error_code ec;
	for(const fs::directory_entry& e :
	    fs::directory_iterator(dir.ToStd(), fs::directory_options::skip_permission_denied, ec)) {
		String name = e.path().filename().string();
		std::error_code ec2;
		bool is_dir = e.is_directory(ec2);   // follows symlinks
		if(is_dir && e.is_symlink(ec2) && !follow_links)
			links.Add(name);   // /XJ: shown, never descended into
		else if(is_dir)
			dirs.Add(name);
		else
			files.Add(name);
	}
	Upp::Sort(dirs);
	Upp::Sort(links);
	Upp::Sort(files);

	for(const String& d : dirs)
		Add(id, CtrlImg::Dir(), String((fs::path(dir.ToStd()) / d.ToStd()).string()), d, true);
	for(const String& l : links)
		Add(id, CtrlImg::Dir(), Null, l + " ->");
	for(const String& f : files)
		Add(id, CtrlImg::File(), Null, f);
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
