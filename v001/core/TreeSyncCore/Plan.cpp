// Planner: compares two scans and lists the actions for Copy, Mirror or Merge.
// Order: folders to create (parents first), then copies and links, then deletes
// (deepest first). Folders with equal hashes are skipped with everything inside.
#include "TreeSyncCore.h"

const char *KindName(ActionKind k)
{
	switch(k) {
	case ActionKind::Mkdir:    return "mkdir";
	case ActionKind::Copy:     return "copy";
	case ActionKind::Link:     return "link";
	case ActionKind::Delete:   return "delete";
	case ActionKind::Conflict: return "conflict";
	}
	return "?";
}

static bool Under(const String& rel, const String& dir)
{
	return rel.GetCount() > dir.GetCount() && rel.StartsWith(dir) && rel[dir.GetCount()] == '/';
}

bool TimeSkip(const SyncFlags& flags, const Entry& s, const Entry& d)
{
	int64 tolerance = flags.fat_file_times ? FAT_TOLERANCE_NS : 0;
	int64 dt = s.mtime_ns - d.mtime_ns;
	bool same_time = abs(dt) <= tolerance ||
	                 (flags.dst_tolerance && abs(abs(dt) - 3600000000000LL) <= tolerance);
	if(flags.skip_changed && same_time)
		return true;
	if(flags.skip_older && !same_time && dt < 0)
		return true;
	if(flags.skip_newer && !same_time && dt > 0)
		return true;
	return false;
}

namespace {

struct Planner {
	const SyncFlags& flags;
	Vector<Action>   mkdirs, copies, deletes;
	VectorMap<Tuple<int64, int64>, String> linked[2];   // (dev, ino) -> first rel copied, per direction

	explicit Planner(const SyncFlags& flags) : flags(flags) {}

	void Add(Vector<Action>& list, ActionKind kind, int dir, const Entry& e, const String& note = Null)
	{
		Action& a = list.Add();
		a.kind = kind;
		a.dir = dir;
		a.from = e;
		a.note = note;
	}

	// Copy e in direction dir, or link it to an earlier copy of the same inode.
	// replaces: something is already there and gets overwritten.
	void Copy(int dir, const Entry& e, bool replaces = false)
	{
		if(flags.hard_links && e.type == 'f' && e.nlink > 1) {
			Tuple<int64, int64> id = { e.dev, e.ino };
			int q = linked[dir].Find(id);
			if(q >= 0) {
				Add(copies, ActionKind::Link, dir, e);
				copies.Top().link_to = linked[dir][q];
				copies.Top().replaces = replaces;
				return;
			}
			linked[dir].Add(id, e.rel);
		}
		Add(copies, ActionKind::Copy, dir, e);
		copies.Top().replaces = replaces;
	}

	bool TimeSkip(const Entry& s, const Entry& d) { return ::TimeSkip(flags, s, d); }

	void OneWay(const Vector<Entry>& src, const Vector<Entry>& dst)
	{
		VectorMap<String, int> dmap;
		for(int i = 0; i < dst.GetCount(); i++)
			dmap.Add(dst[i].rel, i);
		Index<String> in_src;
		String pruned;   // folder whose subtree is skipped
		for(const Entry& s : src) {
			in_src.Add(s.rel);
			if(pruned.GetCount() && Under(s.rel, pruned))
				continue;
			pruned.Clear();
			int q = dmap.Find(s.rel);
			const Entry *d = q >= 0 ? &dst[dmap[q]] : nullptr;
			if(!d) {
				if(flags.skip_source_only) {
					if(s.type == 'd')
						pruned = s.rel;
					continue;
				}
				if(s.type == 'd')
					Add(mkdirs, ActionKind::Mkdir, 0, s);
				else
					Copy(0, s);
				continue;
			}
			if(d->type != s.type) {
				Add(copies, ActionKind::Conflict, 0, s,
				    Format("source is a %s, target is a %s", String(s.type, 1), String(d->type, 1)));
				if(s.type == 'd')
					pruned = s.rel;
				continue;
			}
			if(s.type == 'd') {
				if(s.hash == d->hash && !flags.include_same)
					pruned = s.rel;
				continue;
			}
			if(s.hash == d->hash && !flags.include_same)
				continue;
			if(s.hash != d->hash && TimeSkip(s, *d))
				continue;
			Copy(0, s, true);
		}
		if(flags.mode == SyncMode::Mirror && !flags.keep_extra)
			for(int i = dst.GetCount() - 1; i >= 0; i--)   // deepest first
				if(in_src.Find(dst[i].rel) < 0)
					Add(deletes, ActionKind::Delete, 0, dst[i]);
	}

	// Copies whatever exists on only one side to the other, and a changed file
	// in the direction of the side that changed since the last sync. Changed on
	// both sides, or no history to tell: conflict. Merge never deletes.
	void Merge(const Vector<Entry>& src, const Vector<Entry>& dst, const VectorMap<String, String>& last)
	{
		VectorMap<String, int> dmap, smap;
		for(int i = 0; i < dst.GetCount(); i++)
			dmap.Add(dst[i].rel, i);
		for(int i = 0; i < src.GetCount(); i++)
			smap.Add(src[i].rel, i);
		String pruned;
		for(const Entry& s : src) {
			if(pruned.GetCount() && Under(s.rel, pruned))
				continue;
			pruned.Clear();
			int q = dmap.Find(s.rel);
			if(q < 0) {
				if(s.type == 'd')
					Add(mkdirs, ActionKind::Mkdir, 0, s);
				else
					Copy(0, s);
				continue;
			}
			const Entry& d = dst[dmap[q]];
			if(d.type != s.type) {
				Add(copies, ActionKind::Conflict, 0, s, "different types on the two sides");
				if(s.type == 'd')
					pruned = s.rel;
				continue;
			}
			if(s.type == 'd') {
				if(s.hash == d.hash)
					pruned = s.rel;
				continue;
			}
			if(s.hash == d.hash)
				continue;
			String h = last.Get(s.rel, Null);
			if(h == d.hash)
				Copy(0, s, true);            // only the source changed
			else if(h == s.hash)
				Copy(1, d, true);            // only the target changed
			else
				Add(copies, ActionKind::Conflict, 0, s,
				    IsNull(h) ? "differs and there is no earlier sync to tell which side changed"
				              : "changed on both sides");
		}
		for(const Entry& d : dst) {
			if(pruned.GetCount() && Under(d.rel, pruned))
				continue;
			pruned.Clear();
			if(smap.Find(d.rel) >= 0)
				continue;
			if(d.type == 'd')
				Add(mkdirs, ActionKind::Mkdir, 1, d);
			else
				Copy(1, d);
		}
	}
};

}

Vector<Action> PlanSync(const Vector<Entry>& src, const Vector<Entry>& dst, const SyncFlags& flags,
                        const VectorMap<String, String>& last_synced)
{
	Planner p(flags);
	if(flags.mode == SyncMode::Merge)
		p.Merge(src, dst, last_synced);
	else
		p.OneWay(src, dst);
	Vector<Action> all = pick(p.mkdirs);
	all.AppendPick(pick(p.copies));
	all.AppendPick(pick(p.deletes));
	return all;
}
