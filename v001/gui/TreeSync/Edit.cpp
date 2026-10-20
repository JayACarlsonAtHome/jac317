// Right-click on a tree: New folder, Rename, Delete (to the .old folder, like a sync's
// deletes) and Delete for good. Each change is one treesync-cli --edit, started the
// way a sync is: as root through the window's session in TreeSyncRoot, as you inside
// your home in TreeSyncHome, over SFTP for a remote tree. Not while a sync runs.
#include "TreeSync.h"

static bool NameOk(const String& name, String& why)
{
	if(name.IsEmpty() || name == "." || name == ".." || name.Find('/') >= 0) {
		why = "A name cannot be empty, \".\" or \"..\", or hold a /.";
		return false;
	}
	if(SkipName(name)) {
		why = name + " is a name TreeSync keeps for itself.";
		return false;
	}
	return true;
}

static String AskName(const char *title, const char *label, String name)
{
	for(;;) {
		if(!EditText(name, title, label))
			return String();
		name = TrimBoth(name);
		String why;
		if(NameOk(name, why))
			return name;
		Exclamation(DeQtf(why));
	}
}

void TreeSyncWindow::EditMenu(Bar& bar, int side)
{
	TreePanel& panel = side ? target_panel : source_panel;
	const TreeEndpoint& e = side ? setup.target : setup.source;
	String rel;
	bool folder = false;
	bool any = panel.tree.Selected(rel, folder) && e.path.GetCount();
	bool idle = child == 0;
	bool own = false;   // TreeSync's own files are not changed by hand
	for(const String& step : Split(rel, '/'))
		own = own || SkipName(step);
	String in = folder ? rel : rel.Mid(0, max(rel.ReverseFind('/'), 0));   // where a new folder goes
	String shown = Nvl(rel, String(side ? "the target" : "the source") + " folder");
	bar.Add(any && idle && !own, "New folder...", [=, this] {
		String name = AskName("New folder", "Name of the new folder", String());
		if(name.GetCount())
			Edit(side, "mkdir", in, name);
	}).Help("Makes a folder inside " + Nvl(in, String("the top folder")));
	bar.Add(any && idle && !own && rel.GetCount(), "Rename...", [=, this] {
		String name = AskName("Rename", "New name", GetFileName(rel));
		if(name.GetCount() && name != GetFileName(rel))
			Edit(side, "rename", rel, name);
	});
	bar.Separator();
	bar.Add(any && idle && !own && rel.GetCount(), "Delete...", [=, this] {
		String old = side && setup.safe_delete_dir.GetCount() ? setup.safe_delete_dir
		           : e.remote ? e.path + ".old" : SafeDeleteFolder(CanonicalPath(e.path), true);
		if(PromptYesNo("[* Delete " + DeQtf(shown) + "?]&&It is moved to [C " + DeQtf(old) + "], like the files a "
		               "sync deletes: it can be brought back from there until [* Clear .old].&&" +
		               (side ? String() : String("[*@6 This is the source:] a Mirror run then removes it from the "
		                                         "target too.")) ))
			Edit(side, "delete", rel, String());
	});
	bar.Add(any && idle && !own && rel.GetCount(), "Delete for good...", [=, this] {
		if(PromptYesNo("[*@6 Delete " + DeQtf(shown) + " for good?]&&" +
		               String(folder ? "The folder and everything in it are deleted. " : "") +
		               "This cannot be undone." +
		               (side ? String() : String("&&[*@6 This is the source:] a Mirror run then removes it from the "
		                                         "target too."))))
			Edit(side, "delete-for-good", rel, String());
	});
	if(!idle)
		bar.Add(false, "(not while a sync runs)", [] {});
}

void TreeSyncWindow::Edit(int side, const String& op, const String& rel, const String& name)
{
	edit_args.Clear();
	edit_args << "--edit=" + op << String("--edit-side=") + (side ? "target" : "source") << "--edit-path=" + rel;
	if(name.GetCount())
		edit_args << "--edit-name=" + name;
	StartSync(false, "--edit");
}
