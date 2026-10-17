// Setup screen: tree paths, remote login (keys only), and sync switches.
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
	row(lbl_key, "Key file", key_file, 300);
	key_file.Tip("Optional. Empty: ssh-agent, then ~/.ssh/id_ed25519, id_ecdsa, id_rsa");
	Add(lbl_keys.SetLabel("Keys only, no passwords: set up passwordless SSH first "
	                      "(ssh-copy-id user@host).").LeftPosZ(100, 360).TopPosZ(y, 19));

	port.MinMax(1, 65535);
	remote << [this] { Sync(); };
	Set(TreeEndpoint());
}

void SetupDlg::EndpointPane::Sync()
{
	bool r = remote;
	path.Remote(r);
	for(Ctrl *c : { (Ctrl *)&host, (Ctrl *)&port, (Ctrl *)&user, (Ctrl *)&key_file, (Ctrl *)&lbl_keys })
		c->Enable(r);
}

void SetupDlg::EndpointPane::Set(const TreeEndpoint& e)
{
	path.Set(e.path);
	remote <<= e.remote;
	host <<= e.host;
	port <<= e.port;
	user <<= e.user;
	key_file <<= e.key_file;
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
	e.key_file = ~key_file;
	return e;
}

SetupDlg::FlagsPane::FlagsPane()
{
	mode.Add((int)SyncMode::Copy, "Copy");
	mode.Add((int)SyncMode::Mirror, "Mirror");
	mode.Add((int)SyncMode::Merge, "Merge (both ways)");
	mode.Tip("--copy, --mirror or --merge");
	Add(lbl_mode.SetLabel("Mode").LeftPosZ(8, 90).TopPosZ(8, 19));
	Add(mode.LeftPosZ(100, 360).TopPosZ(8, 19));

	// Switches in two columns, labelled in plain words; the tooltip names the option.
	int half = (BOOL_OPTION_COUNT + 1) / 2;
	for(int i = 0; i < BOOL_OPTION_COUNT; i++) {
		const BoolOption& o = BOOL_OPTIONS[i];
		int col = i < half ? 0 : 1;
		int row = i < half ? i : i - half;
		Add(opt[i].SetLabel(o.text).LeftPosZ(8 + col * 240, 236).TopPosZ(36 + row * 24, 19));
		opt[i].Tip(String("--") + o.name + (*o.robocopy ? String("   (robocopy ") + o.robocopy + ")" : String()));
	}

	int y = 36 + half * 24 + 4;
	Add(lbl_xf.SetLabel("Exclude files").LeftPosZ(8, 90).TopPosZ(y, 19));
	Add(xf.LeftPosZ(100, 360).TopPosZ(y, 19).Tip("--exclude-file=GLOB, ';' between patterns   (robocopy /XF)"));
	y += 26;
	Add(lbl_xd.SetLabel("Exclude folders").LeftPosZ(8, 90).TopPosZ(y, 19));
	Add(xd.LeftPosZ(100, 360).TopPosZ(y, 19).Tip("--exclude-dir=GLOB, ';' between patterns   (robocopy /XD)"));
	y += 26;
	for(int i = 0; i < INT_OPTION_COUNT; i++) {
		const IntOption& o = INT_OPTIONS[i];
		int col = i % 3, row = i / 3;   // three per row
		Add(lbl_int[i].SetLabel(o.text).LeftPosZ(8 + col * 165, 108).TopPosZ(y + row * 26, 19));
		Add(num[i].LeftPosZ(118 + col * 165, 50).TopPosZ(y + row * 26, 19));
		num[i].MinMax(o.min, o.max).Tip(String("--") + o.name + "=N" +
		                                (*o.robocopy ? String("   (robocopy ") + o.robocopy + ")" : String()));
	}
	y += ((INT_OPTION_COUNT + 2) / 3) * 26 + 4;
	Add(preview.HSizePosZ(8, 8).TopPosZ(y, 38));
	preview.SetAlign(ALIGN_TOP);

	for(Ctrl *c = GetFirstChild(); c; c = c->GetNext())
		*c << [this] { Sync(); };
	Set(SyncFlags());
}

void SetupDlg::FlagsPane::Sync()
{
	SyncMode m = (SyncMode)(int)~mode;
	for(int i = 0; i < BOOL_OPTION_COUNT; i++) {
		// Mirror already deletes extras, --keep-extra opts out; Merge never deletes.
		if(BOOL_OPTIONS[i].field == &SyncFlags::keep_extra)
			opt[i].Enable(m == SyncMode::Mirror);
		if(BOOL_OPTIONS[i].field == &SyncFlags::skip_source_only)
			opt[i].Enable(m != SyncMode::Merge);
	}
	preview = CommandLineText(FlagArgs(Get()));
}

void SetupDlg::FlagsPane::Set(const SyncFlags& f)
{
	mode <<= (int)f.mode;
	for(int i = 0; i < BOOL_OPTION_COUNT; i++)
		opt[i] <<= f.*BOOL_OPTIONS[i].field;
	for(int i = 0; i < INT_OPTION_COUNT; i++)
		num[i] <<= f.*INT_OPTIONS[i].field;
	xf <<= f.exclude_files;
	xd <<= f.exclude_dirs;
	Sync();
}

SyncFlags SetupDlg::FlagsPane::Get() const
{
	SyncFlags f;
	f.mode = (SyncMode)(int)~mode;
	for(int i = 0; i < BOOL_OPTION_COUNT; i++)
		f.*BOOL_OPTIONS[i].field = opt[i];
	for(int i = 0; i < INT_OPTION_COUNT; i++)
		f.*INT_OPTIONS[i].field = Nvl((int)~num[i], f.*INT_OPTIONS[i].field);
	f.exclude_files = ~xf;
	f.exclude_dirs = ~xd;
	return f;
}

SetupDlg::SetupDlg()
{
	Title("TreeSync - Setup");
	SetRect(0, 0, Zx(520), Zy(560));

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
