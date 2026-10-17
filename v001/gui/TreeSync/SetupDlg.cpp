// Setup screen: tree paths, remote login, and robocopy-style flags. Stub: the values
// are collected and handed back, but nothing connects or scans yet.
#include "TreeSync.h"

SetupDlg::EndpointPane::EndpointPane()
{
	Add(path.HSizePosZ(8, 8).TopPosZ(8, PathPicker::Height));
	int y = 8 + PathPicker::Height + 8;
	auto row = [&](Label& l, const char* text, Ctrl& c, int w) {
		Add(l.SetLabel(text).LeftPosZ(8, 90).TopPosZ(y, 19));
		Add(c.LeftPosZ(100, w).TopPosZ(y, 19));
		y += 26;
	};

	Add(remote.SetLabel("Remote machine").LeftPosZ(100, 200).TopPosZ(y, 19));
	y += 26;
	row(lbl_host, "Host", host, 200);
	row(lbl_port, "Port", port, 60);
	row(lbl_user, "User", user, 200);
	Add(use_key.SetLabel("Use SSH key").LeftPosZ(100, 200).TopPosZ(y, 19));
	y += 26;
	row(lbl_key, "Key file", key_file, 300);
	row(lbl_pass, "Password", password, 200);

	port.MinMax(1, 65535);
	password.Password();
	remote << [this] { Sync(); };
	use_key << [this] { Sync(); };
	Set(TreeEndpoint());
}

void SetupDlg::EndpointPane::Sync()
{
	bool r = remote;
	path.Remote(r);
	for(Ctrl *c : { (Ctrl *)&host, (Ctrl *)&port, (Ctrl *)&user, (Ctrl *)&use_key })
		c->Enable(r);
	key_file.Enable(r && use_key);
	password.Enable(r && !use_key);
}

void SetupDlg::EndpointPane::Set(const TreeEndpoint& e)
{
	path.Set(e.path);
	remote <<= e.remote;
	host <<= e.host;
	port <<= e.port;
	user <<= e.user;
	use_key <<= e.use_key;
	key_file <<= e.key_file;
	password <<= e.password;
	Sync();
}

TreeEndpoint SetupDlg::EndpointPane::Get() const
{
	TreeEndpoint e;
	e.path = path.Get();
	e.remote = remote;
	e.host = ~host;
	e.port = Nvl((int)~port, 22);
	e.user = ~user;
	e.use_key = use_key;
	e.key_file = ~key_file;
	e.password = ~password;
	return e;
}

String SyncFlags::ToString() const
{
	String s = mode == SyncMode::Copy   ? "/E" :
	           mode == SyncMode::Mirror ? "/MIR" : "MERGE";
	auto sw = [&](bool on, const char *name) { if(on) s << ' ' << name; };
	sw(list_only, "/L");
	sw(fft, "/FFT");
	sw(dst, "/DST");
	sw(xo, "/XO");
	sw(xn, "/XN");
	sw(xc, "/XC");
	sw(xx, "/XX");
	sw(xl, "/XL");
	sw(xj, "/XJ");
	sw(is, "/IS");
	// Patterns are ';'-separated; ones with spaces are quoted, as robocopy needs.
	auto patterns = [&](const char *sw, const String& list) {
		if(list.IsEmpty())
			return;
		s << ' ' << sw;
		for(const String& p : Split(list, ';'))
			s << ' ' << (p.Find(' ') >= 0 ? "\"" + p + "\"" : p);
	};
	patterns("/XF", xf);
	patterns("/XD", xd);
	s << " /R:" << retries << " /W:" << wait << " /MT:" << threads;
	return s;
}

SetupDlg::FlagsPane::FlagsPane()
{
	mode.Add((int)SyncMode::Copy, "Copy (/E)");
	mode.Add((int)SyncMode::Mirror, "Mirror (/MIR)");
	mode.Add((int)SyncMode::Merge, "Merge (both ways)");
	Add(lbl_mode.SetLabel("Mode").LeftPosZ(8, 60).TopPosZ(8, 19));
	Add(mode.LeftPosZ(70, 380).TopPosZ(8, 19));

	struct Flag { Option& o; const char *text; };
	Flag left[] = {
		{ list_only, "/L   List only (dry run)" },
		{ fft,       "/FFT FAT file times (2 s)" },
		{ dst,       "/DST Allow 1-hour DST shift" },
		{ xj,        "/XJ  Skip symlinks/junctions" },
		{ is,        "/IS  Include same files" },
	};
	Flag right[] = {
		{ xo, "/XO Skip older source files" },
		{ xn, "/XN Skip newer source files" },
		{ xc, "/XC Skip changed files" },
		{ xx, "/XX Keep extra target files" },
		{ xl, "/XL Skip source-only files" },
	};
	int y = 36;
	for(Flag& f : left) {
		Add(f.o.SetLabel(f.text).LeftPosZ(8, 220).TopPosZ(y, 19));
		y += 24;
	}
	y = 36;
	for(Flag& f : right) {
		Add(f.o.SetLabel(f.text).LeftPosZ(236, 220).TopPosZ(y, 19));
		y += 24;
	}

	y = 36 + 5 * 24 + 4;
	Add(lbl_xf.SetLabel("/XF files").LeftPosZ(8, 60).TopPosZ(y, 19));
	Add(xf.LeftPosZ(70, 380).TopPosZ(y, 19));
	y += 26;
	Add(lbl_xd.SetLabel("/XD dirs").LeftPosZ(8, 60).TopPosZ(y, 19));
	Add(xd.LeftPosZ(70, 380).TopPosZ(y, 19));
	y += 26;
	Add(lbl_r.SetLabel("/R:").LeftPosZ(8, 30).TopPosZ(y, 19));
	Add(retries.LeftPosZ(40, 50).TopPosZ(y, 19));
	Add(lbl_w.SetLabel("/W:").LeftPosZ(110, 30).TopPosZ(y, 19));
	Add(wait.LeftPosZ(142, 50).TopPosZ(y, 19));
	Add(lbl_mt.SetLabel("/MT:").LeftPosZ(212, 34).TopPosZ(y, 19));
	Add(threads.LeftPosZ(248, 50).TopPosZ(y, 19));
	y += 30;
	Add(preview.LeftPosZ(8, 450).TopPosZ(y, 19));

	retries.MinMax(0, 1000000);
	wait.MinMax(0, 3600);
	threads.MinMax(1, 128);

	for(Ctrl *c = GetFirstChild(); c; c = c->GetNext())
		*c << [this] { Sync(); };
	Set(SyncFlags());
}

void SetupDlg::FlagsPane::Sync()
{
	SyncMode m = (SyncMode)(int)~mode;
	// /MIR already purges, /XX is how to opt out; Merge never purges.
	xx.Enable(m == SyncMode::Mirror);
	xl.Enable(m != SyncMode::Merge);
	preview = Get().ToString();
}

void SetupDlg::FlagsPane::Set(const SyncFlags& f)
{
	mode <<= (int)f.mode;
	list_only <<= f.list_only;
	fft <<= f.fft;
	dst <<= f.dst;
	xo <<= f.xo;
	xn <<= f.xn;
	xc <<= f.xc;
	xx <<= f.xx;
	xl <<= f.xl;
	xj <<= f.xj;
	is <<= f.is;
	xf <<= f.xf;
	xd <<= f.xd;
	retries <<= f.retries;
	wait <<= f.wait;
	threads <<= f.threads;
	Sync();
}

SyncFlags SetupDlg::FlagsPane::Get() const
{
	SyncFlags f;
	f.mode = (SyncMode)(int)~mode;
	f.list_only = list_only;
	f.fft = fft;
	f.dst = dst;
	f.xo = xo;
	f.xn = xn;
	f.xc = xc;
	f.xx = xx;
	f.xl = xl;
	f.xj = xj;
	f.is = is;
	f.xf = ~xf;
	f.xd = ~xd;
	f.retries = Nvl((int)~retries, 3);
	f.wait = Nvl((int)~wait, 5);
	f.threads = Nvl((int)~threads, 8);
	return f;
}

SetupDlg::SetupDlg()
{
	Title("TreeSync - Setup");
	SetRect(0, 0, Zx(520), Zy(440));

	tabs.Add(source.SizePos(), "Source tree");
	tabs.Add(target.SizePos(), "Target tree");
	tabs.Add(flags.SizePos(), "Flags");
	Add(tabs.HSizePosZ(4, 4).VSizePosZ(4, 36));

	Add(ok.SetLabel("OK").RightPosZ(92, 80).BottomPosZ(8, 22));
	Add(cancel.SetLabel("Cancel").RightPosZ(8, 80).BottomPosZ(8, 22));
	Acceptor(ok, IDOK);
	Rejector(cancel, IDCANCEL);
	ok.Ok();
	cancel.Cancel();
}

void SetupDlg::Set(const SyncSetup& s)
{
	source.Set(s.source);
	target.Set(s.target);
	flags.Set(s.flags);
}

SyncSetup SetupDlg::Get() const
{
	SyncSetup s;
	s.source = source.Get();
	s.target = target.Get();
	s.flags = flags.Get();
	return s;
}
