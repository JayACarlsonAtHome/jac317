// The report at the end of a run that copied files: the numbers that show the copies are
// the source. After hours of copying, a bare "Good" is not enough: the bytes and the
// SHA-256 of both sides are shown, and any number that differs on purpose (bytes a
// stopped run had copied, zeros kept as holes) says why where it is shown.
#include "TreeSyncCore.h"

#include <grp.h>
#include <pwd.h>
#include <time.h>

void CopyProofs::Add(CopyProof&& p)
{
	files++;
	size[0] += p.size[0];
	size[1] += p.size[1];
	if(p.sha[0].GetCount() && p.sha[0] == p.sha[1])
		matched++;
	if(p.attr_diff.GetCount())
		attr_diffs++;
	resumed += p.resumed;
	if(p.disk >= 0) {
		disk += p.disk;
		disk_known += p.size[1];
	}
	int at = 0;
	while(at < biggest.GetCount() && biggest[at].size[0] >= p.size[0])
		at++;
	if(at < KEEP) {
		biggest.Insert(at, pick(p));
		if(biggest.GetCount() > KEEP)
			biggest.Drop();
	}
}

// 429562527744 -> "429,562,527,744": every byte, readable.
static String ExactBytes(int64 n)
{
	String digits = AsString(n < 0 ? -n : n), out;
	for(int i = 0; i < digits.GetCount(); i++) {
		if(i && (digits.GetCount() - i) % 3 == 0)
			out.Cat(',');
		out.Cat(digits[i]);   // Cat: << would append a char as its number
	}
	return (n < 0 ? "-" : "") + out;
}

static String FileCount(int n)
{
	return Format("%d file%s", n, n == 1 ? "" : "s");
}

// 2026-09-19 23:38:18.273011253: the time to the nanosecond, as the file system keeps it.
static String TimeToNs(int64 ns)
{
	time_t t = (time_t)(ns / 1000000000);
	struct tm tm;
	localtime_r(&t, &tm);
	return Format("%04d-%02d-%02d %02d:%02d:%02d.%09d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
	              tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(ns % 1000000000));
}

static String OwnerNames(int64 uid, int64 gid)
{
	if(uid < 0)
		return "?";
	String u = AsString(uid), g = AsString(gid);
	if(struct passwd *pw = getpwuid((uid_t)uid))
		u = pw->pw_name;
	if(gid >= 0)
		if(struct group *gr = getgrgid((gid_t)gid))
			g = gr->gr_name;
	return u + ":" + g;
}

Vector<String> CopyReport(const CopyProofs& c, int64 this_run)
{
	Vector<String> out;
	if(c.files == 0)
		return out;
	bool all = c.matched == c.files;
	out.Add(Format("check: %s copied and read back; ", FileCount(c.files)) +
	        (all ? String(c.files == 1 ? "its SHA-256 matches the source's" : "the SHA-256 of every one matches its source's")
	             : Format("SHA-256 MISMATCH: only %d match their source", c.matched)));
	out.Add("  bytes: " + ExactBytes(c.size[0]) + " on the source, " + ExactBytes(c.size[1]) + " in the copies: " +
	        (c.size[0] == c.size[1] ? String("the same") : "DIFFERENT by " + ExactBytes(c.size[1] - c.size[0])));
	if(c.resumed > 0)
		out.Add("  this run copied " + FormatBytes(this_run) + "; the other " + FormatBytes(c.resumed) + " (" +
		        ExactBytes(c.resumed) + " bytes) were copied by the stopped run before it, read back again and kept");
	if(c.disk_known > 0 && c.disk < c.disk_known - c.disk_known / 50)
		out.Add("  on disk the copies take " + FormatBytes(c.disk) + " for " + FormatBytes(c.disk_known) +
		        " of data: runs of zeros are kept as holes (sparse), which read back as zeros, so the content "
		        "and the SHA-256 are the same");
	out.Add(c.attr_diffs ? Format("  warning: %d of %s did not keep all of mode, time and owner (the warnings above say what)",
	                              c.attr_diffs, FileCount(c.files))
	                     : String(c.files == 1 ? "  mode, time and owner: kept" : "  mode, time and owner: kept on all"));
	if(c.biggest.GetCount())
		out.Add(c.files == 1 ? String("  the file:") : Format("  the %d biggest:", c.biggest.GetCount()));
	for(const CopyProof& p : c.biggest) {
		bool match = p.sha[0].GetCount() && p.sha[0] == p.sha[1];
		out.Add("    " + p.rel);
		out.Add("      source  " + ExactBytes(p.size[0]) + " bytes  SHA-256 " + p.sha[0]);
		out.Add("      copy    " + ExactBytes(p.size[1]) + " bytes  SHA-256 " + p.sha[1] + (match ? "  MATCH" : "  MISMATCH"));
		out.Add("      time    " + (p.mtime_ns[0] == p.mtime_ns[1] ? TimeToNs(p.mtime_ns[0]) + " on both"
		                            : "source " + TimeToNs(p.mtime_ns[0]) + ", copy " + TimeToNs(p.mtime_ns[1])));
		String mode0 = Format("%o", p.mode[0] & 07777), mode1 = Format("%o", p.mode[1] & 07777);
		String own0 = OwnerNames(p.uid[0], p.gid[0]), own1 = OwnerNames(p.uid[1], p.gid[1]);
		out.Add("      mode    " + (mode0 == mode1 ? mode0 + " on both" : "source " + mode0 + ", copy " + mode1) +
		        ";  owner " + (own0 == own1 ? own0 + " on both" : "source " + own0 + ", copy " + own1));
		if(p.resumed > 0 || (p.disk >= 0 && p.disk < p.size[1] - p.size[1] / 50)) {
			String note = "      note   ";
			if(p.resumed > 0)
				note << " " << FormatBytes(p.resumed) << " of it was copied by the stopped run, read back and kept;";
			if(p.disk >= 0 && p.disk < p.size[1] - p.size[1] / 50)
				note << " takes " << FormatBytes(p.disk) << " on disk: its runs of zeros are holes;";
			note.Trim(note.GetCount() - 1);
			out.Add(note);
		}
	}
	return out;
}

String CopyReportShort(const CopyProofs& c)
{
	if(c.files == 0)
		return String();
	String s = FileCount(c.files) + " checked: ";
	s << (c.size[0] == c.size[1] ? ExactBytes(c.size[0]) + " bytes on both sides"
	                             : ExactBytes(c.size[0]) + " bytes on the source, " + ExactBytes(c.size[1]) + " in the copies");
	s << (c.matched == c.files ? ", SHA-256 match" : Format(", SHA-256 MISMATCH on %d", c.files - c.matched));
	if(c.resumed > 0)
		s << " (" << FormatBytes(c.resumed) << " of it copied by the stopped run)";
	return s;
}
