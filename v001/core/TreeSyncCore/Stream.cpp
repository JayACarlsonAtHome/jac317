// Streaming sync, for Copy and Mirror: three stages run at the same time.
//
//   pre-scan   one thread walks the source a folder at a time, reusing cached
//              hashes, creates folders on the target and queues the files;
//   copy       --threads workers, right behind it: per file, check the target, hash
//              only what is not known yet (big files are compared chunk by chunk),
//              copy through the chunk pipeline, verify;
//   post-scan  one thread, the side channel: once a folder is fully scanned and all
//              its files are done (it is "settled"), checks the target folder,
//              removes what the source does not have (Mirror) and confirms the rest.
//
// Every action is recorded before it runs, so a stopped run can be resumed: the next
// run walks again (quickly, with the hash caches) and a big file continues at its
// last checked chunk. Merge needs both sides complete and keeps the planned engine.
#include "Runner.h"

#include <condition_variable>
#include <deque>

namespace TsRun {

namespace {

struct Item : Moveable<Item> {
	Entry  e;            // the source entry; hash empty when not known yet
	String link_to;      // a later name of a hard-linked file: the first name
	String folder;       // the folder it is in, for settling
};

struct Folder {
	int           pending = 0;     // queued items not finished yet
	bool          walked = false;  // every entry seen and queued
	bool          posted = false;  // handed to the post-scan
	bool          unread = false;  // the source folder could not be listed: remove nothing from the target's
	Index<String> names;           // source names in it
	bool          inside = true;   // in an included folder (NameFilter)
	bool          made = false;    // with includes: made on the target, when something included goes in
	bool          bad = false;     // with includes: could not be made; what is in it is left out
	FolderEntry   src;             // the source folder, to make it from
};

struct Streamer {
	Runner&           r;
	const SyncFlags&  flags;
	bool              dry;
	bool              assume_empty = false;   // dry run with --wipe-target: as if wiped
	NameFilter        filter;

	// pre-scan -> copy workers
	std::mutex              qm;
	std::condition_variable qcv;
	std::deque<Item>        queue;
	bool                    scan_done = false;
	Vector<Item>            links;     // hard links, after all copies (r.mx)

	// settling -> post-scan (folders: r.mx)
	ArrayMap<String, Folder> folders;
	std::mutex              pm;
	std::condition_variable pcv;
	std::deque<String>      settled;
	bool                    post_last = false;

	std::atomic<int64>      entries = 0, found = 0, checked = 0;
	int64                   estimate = 0;
	std::chrono::steady_clock::time_point last_stage = std::chrono::steady_clock::now();
	VectorMap<Tuple<int64, int64>, String> first_name;   // hard links: (dev, ino) -> first rel (pre-scan only)

	Streamer(Runner& r) : r(r), flags(r.flags), dry(r.flags.dry_run), filter(r.flags) {}

	// Deletes a target entry and, for a folder, everything in it (--wipe-target
	// without safe delete).
	bool DeleteTree(TreeFs& fs, const String& rel, char type, String& error)
	{
		if(type == 'd') {
			Vector<FolderEntry> list;
			if(!fs.List(rel, list, error))
				return false;
			for(const FolderEntry& f : list)
				if(f.name != "." && f.name != ".." && !DeleteTree(fs, rel + "/" + f.name, f.type, error))
					return false;
		}
		return fs.Remove(rel, type == 'd', error) == 0;
	}

	// --wipe-target: empty the target before anything is copied. With safe delete
	// each top-level entry is moved (renamed) into <target>.old: instant, nothing lost.
	bool Wipe(Ctx& c, String& error)
	{
		Vector<FolderEntry> list;
		if(!c.fs[1]->List(String(), list, error))
			return Missing(error);   // no target yet: nothing to empty
		int n = 0;
		for(const FolderEntry& f : list) {
			if(f.name == "." || f.name == "..")
				continue;
			if(r.report.Stopping())
				return true;
			n++;
			if(dry) {
				Plan(Format("%-8s %s", "wipe", f.name));
				continue;
			}
			Action a;
			a.kind = ActionKind::Delete;
			a.from.rel = f.name;
			a.from.type = f.type == 'd' ? 'd' : f.type == 'l' ? 'l' : 'f';
			a.note = "wipe";
			int seq = r.Record(a);
			r.SetState(seq, "in_progress");
			String err;
			bool ok = flags.safe_delete ? r.MoveOut(c, 1, f.name, err)
			                            : DeleteTree(*c.fs[1], f.name, a.from.type, err);
			r.SetState(seq, ok ? "done" : "failed", 0, ok ? String() : err);
			Lock l(r.mx);
			if(ok) {
				r.result.done++;
				if(!flags.safe_delete)
					r.report.WhenLine("deleted " + f.name);
			}
			else {
				r.result.failed++;
				r.report.WhenLine("FAILED " + f.name + ": " + err);
				error = "could not empty the target: " + err;
				return false;
			}
		}
		if(dry)
			assume_empty = true;
		r.Line(Format("%s the target first: %d entries%s", dry ? "would empty" : "emptied", n,
		              flags.safe_delete ? " moved to " + r.old_roots[1] : String(" deleted")));
		return true;
	}

	using Lock = Runner::Lock;

	void Stage(int stage, int64 done, int64 total, const String& detail, bool finished = false)
	{
		StageProgress s;
		s.stage = stage;
		s.done = done;
		s.total = total;
		s.detail = detail;
		s.finished = finished;
		Lock l(r.mx);
		r.report.WhenStage(s);
	}

	void Plan(const String& line)
	{
		Lock l(r.mx);
		r.result.plan.Add(line);
		r.report.WhenLine("would " + line);
	}

	void Fail(const String& rel, const String& error)
	{
		Lock l(r.mx);
		r.result.failed++;
		r.report.WhenLine("FAILED " + rel + ": " + error);
	}

	void Conflict(const String& rel, const String& why)
	{
		Lock l(r.mx);
		r.result.conflicts++;
		r.report.WhenLine("CONFLICT " + rel + ": " + why);
	}

	static bool Missing(const String& error)
	{
		String e = ToLower(error);
		return e.Find("no such file") >= 0 || e.Find("not a directory") >= 0;
	}

	// Settling ------------------------------------------------------------------

	void TrySettle(const String& rel)   // r.mx held
	{
		int q = folders.Find(rel);
		if(q < 0)
			return;
		Folder& f = folders[q];
		if(f.walked && f.pending == 0 && !f.posted) {
			f.posted = true;
			{
				std::lock_guard<std::mutex> l(pm);
				settled.push_back(rel);
			}
			pcv.notify_one();
		}
	}

	void Finished(const String& folder)
	{
		Lock l(r.mx);
		folders.GetAdd(folder).pending--;
		TrySettle(folder);
	}

	// Pre-scan ------------------------------------------------------------------

	// The target side of a new source folder: create it (or say so in a dry run).
	// False: leave the folder out (a file is in the way, or it could not be made).
	bool TargetFolder(Ctx& c, const FolderEntry& src, const String& rel)
	{
		Entry t;
		String error = "no such file";
		if(!assume_empty && r.Net(c, [&] { return c.fs[1]->Stat(rel, false, t, error); })) {
			if(t.type == 'd')
				return true;
			Conflict(rel, "a folder in the source, not a folder on the target");
			return false;
		}
		if(r.report.Stopping())
			return false;
		if(!Missing(error)) {
			Fail(rel, error);
			return false;
		}
		if(dry) {
			Plan("create folder " + rel);
			return true;
		}
		Action a;
		a.kind = ActionKind::Mkdir;
		a.from.rel = rel;
		a.from.type = 'd';
		a.from.mode = src.mode;
		Step s;
		s.a = pick(a);
		s.seq = r.Record(s.a);
		int failed = r.result.failed;
		if(!r.RunStep(c, s))
			return false;
		Lock l(r.mx);
		return r.result.failed == failed;
	}

	// With includes: makes rel on the target, and the folders above it, the first time
	// something included goes in it. False: it could not be made.
	bool EnsureFolder(Ctx& c, const String& rel)
	{
		if(rel.IsEmpty())
			return true;
		FolderEntry src;
		{
			Lock l(r.mx);
			Folder& fd = folders.GetAdd(rel);
			if(fd.made || fd.bad)
				return fd.made;
			src = fd.src;
		}
		bool ok = EnsureFolder(c, GetFileFolder(rel)) && TargetFolder(c, src, rel);
		Lock l(r.mx);
		Folder& fd = folders.GetAdd(rel);
		(ok ? fd.made : fd.bad) = true;
		return ok;
	}

	void Queue(Item&& it)
	{
		{
			std::lock_guard<std::mutex> l(qm);
			queue.push_back(pick(it));
		}
		qcv.notify_one();
	}

	void ScanFolder(Ctx& c, const String& rel, Vector<String>& stack, Index<Tuple<int64, int64>>& walked)
	{
		bool inside;
		{
			Lock l(r.mx);
			inside = folders.GetAdd(rel).inside;
		}
		Vector<FolderEntry> list;
		String error;
		if(!r.Net(c, [&] { list.Clear(); return c.fs[0]->List(rel, list, error); })) {
			if(!r.report.Stopping())
				r.Line("warning: cannot read source folder " + Nvl(rel, String("/")) + ": " + error +
				       "; its target folder is left as it is");
			Lock l(r.mx);
			r.result.source.warnings++;
			Folder& fd = folders.GetAdd(rel);
			fd.unread = true;
			fd.walked = true;
			TrySettle(rel);
			return;
		}
		Sort(list, [](const FolderEntry& a, const FolderEntry& b) { return a.name < b.name; });

		Vector<int> subdirs;
		for(int i = 0; i < list.GetCount(); i++) {
			if(r.report.Stopping())
				break;
			FolderEntry& f = list[i];
			if(SkipName(f.name))
				continue;
			String crel = rel.IsEmpty() ? f.name : rel + "/" + f.name;
			entries++;
			if(std::chrono::steady_clock::now() - last_stage > std::chrono::milliseconds(250)) {
				last_stage = std::chrono::steady_clock::now();
				Stage(0, entries, estimate, crel);
				Lock l(r.mx);
				r.report.WhenStatus(Format("Pre-scan: %lld entries%s  %s", (int64)entries,   // %d cuts int64 to int
				                           estimate ? Format(" of ~%lld", estimate) : String(), crel));
			}
			// --follow-links: a symlink stands for what it points to (file or folder).
			if(f.is_link && flags.follow_links) {
				Entry t;
				String e;
				if(c.fs[0]->Stat(crel, true, t, e) && (t.type == 'd' || t.type == 'f')) {
					f.type = t.type;
					f.size = t.size;
					f.mtime_ns = t.mtime_ns;
					f.ctime_ns = t.ctime_ns;
					f.dev = t.dev;
					f.ino = t.ino;
					f.nlink = t.nlink;
					f.mode = t.mode;
					f.uid = t.uid;
					f.gid = t.gid;
				}
			}
			if(f.type == 'd') {
				if(filter.SkipDir(f.name))
					continue;
				if(c.fs[0]->IsLocal() && f.dev) {   // symlink loops (--follow-links)
					Tuple<int64, int64> id = { f.dev, f.ino };
					if(walked.Find(id) >= 0) {
						r.Line("warning: symlink loop skipped: " + crel);
						continue;
					}
					walked.Add(id);
				}
				Lock l(r.mx);
				folders.GetAdd(rel).names.Add(f.name);
				subdirs.Add(i);
				continue;
			}
			if(f.type == 's') {
				r.Line("warning: skipped special file (device, pipe or socket): " + crel);
				continue;
			}
			if((f.type == 'f' || f.type == 'l') && !filter.KeepFile(inside, f.name))
				continue;
			if(filter.include && !EnsureFolder(c, rel))
				continue;

			Item it;
			Entry& e = it.e;
			e.rel = crel;
			e.type = f.type;
			e.size = f.size;
			e.mtime_ns = f.mtime_ns;
			e.ctime_ns = f.ctime_ns;
			e.dev = f.dev;
			e.ino = f.ino;
			e.nlink = f.nlink;
			e.mode = f.mode;
			e.uid = f.uid;
			e.gid = f.gid;
			e.link = f.link;
			it.folder = rel;
			if(e.type == 'l')
				e.hash = SHA256String("symlink\n" + e.link);
			else {
				Lock l(r.mx);
				if(r.caches[0]->Lookup(e, e.hash)) {
					r.result.source.reused++;
					r.caches[0]->Store(e);   // seen in this run
				}
			}
			bool later_link = false;
			if(e.type == 'f' && flags.hard_links && e.nlink > 1 && e.dev) {
				Tuple<int64, int64> id = { e.dev, e.ino };
				int q = first_name.Find(id);
				if(q >= 0) {
					it.link_to = first_name[q];
					later_link = true;
				}
				else
					first_name.Add(id, crel);
			}
			{
				Lock l(r.mx);
				r.result.source.entries++;
				Folder& fd = folders.GetAdd(rel);
				fd.names.Add(f.name);
				fd.pending++;
				if(e.type == 'f') {
					r.unchecked++;
					r.unchecked_bytes += e.size;
				}
				if(later_link)
					links.Add(pick(it));
			}
			if(!later_link)
				Queue(pick(it));
		}
		{
			Lock l(r.mx);
			folders.GetAdd(rel).walked = true;
			TrySettle(rel);
		}
		// Subfolders: their target folder first, then walk them in name order. With
		// includes a folder is made only when something included goes in it (or it is
		// an included folder itself).
		Vector<String> next;
		for(int i : subdirs) {
			String crel = rel.IsEmpty() ? list[i].name : rel + "/" + list[i].name;
			if(r.report.Stopping())
				break;
			bool ok;
			if(filter.include) {
				bool in = filter.Inside(inside, list[i].name);
				{
					Lock l(r.mx);
					Folder& fd = folders.GetAdd(crel);
					fd.inside = in;
					fd.src = list[i];
				}
				ok = !filter.KeepEmptyDir(in) || EnsureFolder(c, crel);
			}
			else
				ok = TargetFolder(c, list[i], crel);
			if(ok) {
				found++;
				next.Add(crel);
			}
		}
		for(int i = next.GetCount() - 1; i >= 0; i--)
			stack.Add(next[i]);
	}

	void PreScan(Ctx& c)
	{
		Vector<String> stack;
		Index<Tuple<int64, int64>> walked;
		stack.Add(String());
		found = 1;
		{
			Lock l(r.mx);
			Folder& root = folders.GetAdd(String());
			root.inside = filter.RootInside();
			root.made = true;
		}
		while(stack.GetCount() && !r.report.Stopping())
			ScanFolder(c, stack.Pop(), stack, walked);
		Stage(0, entries, estimate, String(), true);
		r.counting = false;
		{
			std::lock_guard<std::mutex> l(qm);
			scan_done = true;
		}
		qcv.notify_all();
	}

	// Copy workers ---------------------------------------------------------------

	// Are source e and target t (same size, both regular files) the same? Works out
	// whatever hash is missing; a big file is compared chunk by chunk, stopping at the
	// first difference. Hashes worked out here go into the caches.
	bool Same(Ctx& c, Entry& e, Entry& t, String& th, bool& same, String& error)
	{
		int64 big = (int64)flags.big_file_mib << 20;
		if(e.size >= big && (e.hash.IsEmpty() || th.IsEmpty())) {
			One<TreeFile> a = c.fs[0]->Open(e.rel, false, error);
			One<TreeFile> b = a ? c.fs[1]->Open(t.rel, false, error) : One<TreeFile>();
			if(!a || !b)
				return false;
			const int64 SLICE = 1 << 20;
			Buffer<byte> x((size_t)SLICE), y((size_t)SLICE);
			Sha256Stream sha;
			same = true;
			for(int64 off = 0; off < e.size && same; off += SLICE) {
				if(r.report.Stopping()) {
					error = "stopped";
					return false;
				}
				int64 n = min(SLICE, e.size - off);
				if(!a->ReadAt(off, x, n, error) || !b->ReadAt(off, y, n, error))
					return false;
				if(!c.fs[0]->IsLocal())
					r.report.Count(n);
				if(!c.fs[1]->IsLocal())
					r.report.Count(n);
				same = memcmp(~x, ~y, (size_t)n) == 0;
				sha.Put(x, (int)n);
			}
			Lock l(r.mx);
			r.result.source.hashed++;
			r.result.target.hashed++;
			if(same) {
				e.hash = th = sha.FinishString();
				r.caches[0]->Store(e);
				t.hash = th;
				r.caches[1]->Store(t);
			}
			return true;
		}
		if(e.hash.IsEmpty()) {
			if(!c.fs[0]->Hash(e.rel, flags.follow_links, e.hash, error, r.report.stop))
				return false;
			Lock l(r.mx);
			r.result.source.hashed++;
			r.result.source.hashed_bytes += e.size;
			r.caches[0]->Store(e);
		}
		if(th.IsEmpty()) {
			if(!c.fs[1]->Hash(t.rel, false, th, error, r.report.stop))
				return false;
			Lock l(r.mx);
			r.result.target.hashed++;
			r.result.target.hashed_bytes += t.size;
			t.hash = th;
			r.caches[1]->Store(t);
		}
		same = e.hash == th;
		return true;
	}

	// One source entry: compare with the target, copy if needed.
	void Process(Ctx& c, Item& it)
	{
		Entry& e = it.e;
		if(e.type == 'f') {   // decided below: copied (copy_count) or not
			r.unchecked--;
			r.unchecked_bytes -= e.size;
		}
		Entry t;
		String error = "no such file";
		bool exists = !assume_empty && r.Net(c, [&] { return c.fs[1]->Stat(e.rel, false, t, error); });
		if(r.report.Stopping())
			return;
		if(!exists && !Missing(error)) {
			Fail(e.rel, error);
			return;
		}
		if(exists && t.type == 'd') {
			Conflict(e.rel, "a folder on the target where the source has a file");
			return;
		}
		Action a;
		a.kind = it.link_to.GetCount() ? ActionKind::Link : ActionKind::Copy;
		a.link_to = it.link_to;
		a.replaces = exists;

		if(e.type == 'l') {
			if(exists && t.type == 'l' && t.link == e.link)
				return;
		}
		else if(it.link_to.GetCount()) {
			// A later name of a hard-linked file: already linked to the first name?
			Entry first;
			String ignore;
			if(exists && c.fs[1]->CanHardLink() && c.fs[1]->Stat(it.link_to, false, first, ignore) &&
			   first.ino == t.ino && first.dev == t.dev)
				return;
			// Its hash is the first name's, worked out when that one was copied.
			if(e.hash.IsEmpty()) {
				Entry f = clone(e);
				f.rel = it.link_to;
				Lock l(r.mx);
				r.caches[0]->Lookup(f, e.hash);
			}
		}
		else {
			if(!exists && flags.skip_source_only)
				return;
			bool same = false;
			if(exists && t.type == 'f' && t.size == e.size) {
				String th;
				{
					Lock l(r.mx);
					if(r.caches[1]->Lookup(t, th))
						r.result.target.reused++;
				}
				if(e.hash.GetCount() && th.GetCount())
					same = e.hash == th;
				else if(!r.Net(c, [&] { return Same(c, e, t, th, same, error); })) {
					if(error != "stopped" && !r.report.Stopping())
						Fail(e.rel, error);
					return;
				}
				if(same) {
					Lock l(r.mx);
					t.hash = th;
					r.caches[1]->Store(t);   // seen in this run
				}
				if(same && !flags.include_same)
					return;
			}
			if(exists && !same && t.type == 'f' && TimeSkip(flags, e, t))
				return;
		}

		if(dry) {
			String line = Format("%-8s %s", KindName(a.kind), e.rel);
			if(a.kind == ActionKind::Link)
				line << " -> " << a.link_to;
			if(exists)
				line << "   (replaces the target's version)";
			Plan(line);
			Lock l(r.mx);
			r.overall_total += e.size;
			return;
		}
		if(e.type == 'f') {
			Lock l(r.mx);
			r.copy_count++;
			r.overall_total += e.size;
		}
		a.from = clone(e);
		Step s;
		s.a = pick(a);
		s.seq = r.Record(s.a);
		r.RunStep(c, s);
	}

	void Work(Ctx& c)
	{
		for(;;) {
			Item it;
			{
				std::unique_lock<std::mutex> l(qm);
				qcv.wait(l, [&] { return !queue.empty() || scan_done || r.report.Stopping(); });
				if(r.report.Stopping() || queue.empty())
					return;
				it = pick(queue.front());
				queue.pop_front();
			}
			Process(c, it);
			Finished(it.folder);
		}
	}

	// Post-scan ------------------------------------------------------------------

	// Removes an extra entry from the target (Mirror), folders contents first. What the
	// name filters leave out is left alone, and so is the folder holding it. inside: rel
	// is in an included folder. True: it is gone (or would be, in a dry run).
	bool RemoveExtra(Ctx& c, const String& rel, char type, bool inside)
	{
		if(r.report.Stopping())
			return false;
		if(type == 'd') {
			bool in = filter.Inside(inside, GetFileName(rel)), all = true;
			Vector<FolderEntry> list;
			String error;
			if(r.Net(c, [&] { list.Clear(); return c.fs[1]->List(rel, list, error); }))
				for(const FolderEntry& f : list) {
					if(SkipName(f.name))
						continue;
					if(f.type == 'd' ? filter.SkipDir(f.name) : !filter.KeepFile(in, f.name))
						all = false;
					else
						all = RemoveExtra(c, rel + "/" + f.name, f.type, in) && all;
				}
			if(!all) {
				r.Line("kept folder " + rel + ": it holds what the name filters leave out");
				return false;
			}
		}
		if(dry) {
			Plan(Format("%-8s %s", "delete", rel));
			return true;
		}
		Action a;
		a.kind = ActionKind::Delete;
		a.from.rel = rel;
		a.from.type = type == 'd' ? 'd' : type == 'l' ? 'l' : 'f';
		Step s;
		s.a = pick(a);
		s.seq = r.Record(s.a);
		r.RunStep(c, s);
		return true;
	}

	void CheckFolder(Ctx& c, const String& rel)
	{
		if(assume_empty)   // a dry run of --wipe-target: nothing would be left to check
			return;
		Index<String> names;
		bool inside, made;
		{
			Lock l(r.mx);
			const Folder& fd = folders.GetAdd(rel);
			if(fd.unread)   // what the source has there is not known: delete nothing
				return;
			names = clone(fd.names);
			inside = fd.inside;
			made = fd.made;
		}
		Vector<FolderEntry> list;
		String error;
		if(!r.Net(c, [&] { list.Clear(); return c.fs[1]->List(rel, list, error); })) {
			if(r.report.Stopping())
				return;
			if(filter.include && !made && Missing(error))   // nothing included went there
				return;
			if(!(dry && Missing(error)))   // a dry run did not create it
				r.Line("warning: cannot read target folder " + Nvl(rel, String("/")) + ": " + error);
			return;
		}
		bool purge = flags.mode == SyncMode::Mirror && !flags.keep_extra;
		for(const FolderEntry& f : list) {
			if(SkipName(f.name) || names.Find(f.name) >= 0)
				continue;
			// What the name filters leave out is left alone on the target too.
			if(f.type == 'd' ? filter.SkipDir(f.name) : !filter.KeepFile(inside, f.name))
				continue;
			if(purge)
				RemoveExtra(c, rel.IsEmpty() ? f.name : rel + "/" + f.name, f.type, inside);
		}
	}

	void PostScan(Ctx& c)
	{
		for(;;) {
			String rel;
			{
				std::unique_lock<std::mutex> l(pm);
				pcv.wait(l, [&] { return !settled.empty() || post_last || r.report.Stopping(); });
				if(r.report.Stopping() || settled.empty())
					break;
				rel = settled.front();
				settled.pop_front();
			}
			CheckFolder(c, rel);
			checked++;
			Stage(2, checked, found, rel);
		}
		Stage(2, checked, found, String(), true);
	}
};

}

bool RunStreaming(Runner& r, TreeFs *side[2], const String& key, bool fresh, String& error)
{
	Db& db = r.db;
	SyncReport& report = r.report;
	SyncResult& result = r.result;
	const SyncFlags& flags = r.flags;
	const String& src = r.ids[0];
	const String& dst = r.ids[1];

	// A run for the same trees and switches that did not finish: continue it (same
	// run, so a big file's recorded chunks are found again).
	if(!flags.dry_run) {
		if(fresh)
			Stmt(db, "UPDATE runs SET state = 'abandoned' WHERE source = ? AND target = ? "
			         "AND state IN ('running', 'stopped')").Text(1, src).Text(2, dst).Run();
		else {
			Stmt q(db, "SELECT id FROM runs WHERE source = ? AND target = ? AND args = ? "
			           "AND state IN ('running', 'stopped') ORDER BY id DESC LIMIT 1");
			q.Text(1, src).Text(2, dst).Text(3, key);
			if(q.Step()) {
				r.run = result.run = q.Int(0);
				result.resumed = true;
				report.WhenLine(Format("resuming run %d", r.run));
			}
		}
		if(!result.resumed) {
			Stmt(db, "INSERT INTO runs (started, source, target, args, state) VALUES (?, ?, ?, ?, 'running')")
				.Int(1, (int64)time(nullptr)).Text(2, src).Text(3, dst).Text(4, key).Run();
			r.run = result.run = db.LastId();
			report.WhenLine(Format("run %d", r.run));
		}
		Stmt(db, "UPDATE runs SET state = 'running' WHERE id = ?").Int(1, r.run).Run();
	}

	if(!side[0]->RootExists()) {
		error = "source " + src + " does not exist or is not a folder";
		return false;
	}
	if(!side[1]->RootExists() && !flags.dry_run && !side[1]->MakeRoot(error))
		return false;

	HashCache cache0(db, src, flags, !side[0]->StableIds());
	HashCache cache1(db, dst, flags, !side[1]->StableIds());
	r.caches[0] = &cache0;
	r.caches[1] = &cache1;

	Streamer s(r);
	s.estimate = side[0]->EntryEstimate();
	// A resumed run emptied the target when it started: doing it again would throw
	// away what it has copied since.
	if(flags.wipe_target && result.resumed)
		report.WhenLine("not emptying the target again: this run already did when it started");
	else if(flags.wipe_target) {
		String why;
		if(!WipeAllowed(side[1]->RootPath(), why)) {
			error = why;
			return false;
		}
		if(!s.Wipe(r.main, error))
			return false;
	}
	result.source.estimate = s.estimate;

	// Contexts: the pre-scan and the post-scan each get their own (their own SFTP
	// connections to a remote side), the copy workers one each.
	Ctx pre, post;
	Array<Ctx> workers;
	String cerr;
	if(!r.Connect(pre, cerr) || !r.Connect(post, cerr)) {
		error = cerr;
		return false;
	}
	int n = max(flags.threads, 1);
	for(int t = 1; t < n; t++) {
		Ctx& c = workers.Add();
		if(!r.Connect(c, cerr)) {
			workers.Drop();
			report.WhenLine("warning: copying with fewer threads, a connection failed: " + cerr);
			break;
		}
	}
	report.WhenLine(Format("streaming: pre-scan, copy (%d threads), post-scan at the same time",
	                       workers.GetCount() + 1));

	r.counting = true;
	std::thread pre_t([&] { s.PreScan(pre); });
	std::thread post_t([&] { s.PostScan(post); });
	std::vector<std::thread> work_t;
	for(Ctx& c : workers)
		work_t.emplace_back([&s, &c] { s.Work(c); });
	s.Work(r.main);
	pre_t.join();
	for(std::thread& t : work_t)
		t.join();
	if(!report.Stopping())   // pre-scan may still have queued a few after the main loop left
		s.Work(r.main);

	// Hard links last: each needs its first name copied.
	{
		Vector<Item> links;
		{
			Runner::Lock l(r.mx);
			links = pick(s.links);
		}
		for(Item& it : links) {
			if(report.Stopping())
				break;
			s.Process(r.main, it);
			s.Finished(it.folder);
		}
	}
	{
		std::lock_guard<std::mutex> l(s.pm);
		s.post_last = true;
	}
	s.pcv.notify_all();
	post_t.join();

	bool stopped = report.Stopping();
	result.stopped = stopped;
	r.caches[0] = r.caches[1] = nullptr;
	if(!stopped && !flags.dry_run) {
		cache0.Finish();   // forget files that are gone from the trees
		cache1.Finish();
	}
	else
		db.Exec("COMMIT");

	if(flags.dry_run) {
		report.WhenLine(Format("dry run: %d actions, nothing changed", result.plan.GetCount()));
		if(flags.safe_delete)
			report.WhenLine("deleted and replaced files would move to " + r.old_roots[1]);
		return true;
	}
	Stmt(db, "UPDATE runs SET state = ?, finished = ? WHERE id = ?")
		.Text(1, stopped ? "stopped" : result.failed ? "failed" : "done")
		.Int(2, (int64)time(nullptr)).Int(3, r.run).Run();
	report.WhenOverall(r.overall_total, r.overall_total);
	if(r.copy_count == 0)
		report.WhenLine("nothing to copy");
	else {
		CopyProgress total;
		total.index = r.copy_index;
		total.count = r.copy_count;
		total.name = stopped ? "Total (stopped)" : "Total";
		total.done = total.size = result.bytes;
		total.seconds = Since(r.started);
		report.WhenLine(ProgressLine(total));
	}
	return true;
}

}
