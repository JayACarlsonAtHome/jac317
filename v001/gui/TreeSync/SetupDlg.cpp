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

// Laid out from the measured width of each label in the dialog's font, so text
// never runs into the next control; the dialog is sized to fit (SetupDlg).
SetupDlg::FlagsPane::FlagsPane()
{
	Font font = StdFont();
	auto W = [&](const char *t) { return GetTextSize(t, font).cx; };
	int m = Zx(8), gap = Zx(18), row = Zy(24), h = Zy(19), box = Zx(26);

	mode.Add((int)SyncMode::Copy, "Copy");
	mode.Add((int)SyncMode::Mirror, "Mirror");
	mode.Add((int)SyncMode::Merge, "Merge (both ways)");
	mode.Tip("--copy, --mirror or --merge");
	int lw = max(W("Mode"), W("Owners")) + Zx(12);
	Add(lbl_mode.SetLabel("Mode").LeftPos(m, lw).TopPos(m, h));
	Add(mode.HSizePos(m + lw, m).TopPos(m, h));
	owners.Add(0, "Don't copy");
	owners.Add(1, "By name (needs root)");
	owners.Add(2, "By number (needs root)");
	owners.Tip("--no-preserve-owner, --preserve-owner, or --numeric-ids");
	Add(lbl_owners.SetLabel("Owners").LeftPos(m, lw).TopPos(m + row, h));
	Add(owners.HSizePos(m + lw, m).TopPos(m + row, h));

	// Switches in two columns. Dry run is left out: in the GUI the Sync menu decides.
	Vector<int> shown;
	int col_w = 0;
	for(int i = 0; i < BOOL_OPTION_COUNT; i++) {
		// Dry run: the Sync menu decides. Owners: the Owners choice above.
		if(BOOL_OPTIONS[i].field == &SyncFlags::dry_run || BOOL_OPTIONS[i].field == &SyncFlags::preserve_owner ||
		   BOOL_OPTIONS[i].field == &SyncFlags::numeric_ids) {
			Add(opt[i]);   // kept for Set/Get, never shown
			opt[i].Hide();
			continue;
		}
		shown.Add(i);
		col_w = max(col_w, W(BOOL_OPTIONS[i].text) + box);
	}
	int half = (shown.GetCount() + 1) / 2;
	int y = m + 2 * row + Zy(6);
	for(int k = 0; k < shown.GetCount(); k++) {
		int i = shown[k];
		const BoolOption& o = BOOL_OPTIONS[i];
		int x = k < half ? m : m + col_w + gap;
		Add(opt[i].SetLabel(o.text).LeftPos(x, col_w).TopPos(y + (k % half) * row, h));
		opt[i].Tip(String("--") + o.name + (*o.robocopy ? String("   (robocopy ") + o.robocopy + ")" : String()));
	}
	need.cx = m + 2 * col_w + gap + m;
	y += half * row + Zy(8);

	int xw = max(W("Exclude files"), W("Exclude folders")) + Zx(12);
	Add(lbl_xf.SetLabel("Exclude files").LeftPos(m, xw).TopPos(y, h));
	Add(xf.HSizePos(m + xw, m).TopPos(y, h).Tip("--exclude-file=GLOB, ';' between patterns   (robocopy /XF)"));
	y += Zy(26);
	Add(lbl_xd.SetLabel("Exclude folders").LeftPos(m, xw).TopPos(y, h));
	Add(xd.HSizePos(m + xw, m).TopPos(y, h).Tip("--exclude-dir=GLOB, ';' between patterns   (robocopy /XD)"));
	y += Zy(30);

	// Numbers, two per row.
	int nw = 0;
	for(int i = 0; i < INT_OPTION_COUNT; i++)
		nw = max(nw, W(INT_OPTIONS[i].text) + Zx(10));
	int fw = Zx(80), span = nw + fw + gap;
	for(int i = 0; i < INT_OPTION_COUNT; i++) {
		const IntOption& o = INT_OPTIONS[i];
		int x = m + (i % 2) * span, yy = y + (i / 2) * Zy(26);
		Add(lbl_int[i].SetLabel(o.text).LeftPos(x, nw).TopPos(yy, h));
		Add(num[i].LeftPos(x + nw, fw).TopPos(yy, h));
		num[i].MinMax(o.min, o.max).Tip(String("--") + o.name + "=N" +
		                                (*o.robocopy ? String("   (robocopy ") + o.robocopy + ")" : String()));
	}
	need.cx = max(need.cx, m + 2 * span);
	y += ((INT_OPTION_COUNT + 1) / 2) * Zy(26) + Zy(6);

	Add(preview.HSizePos(m, m).TopPos(y, h));
	y += row + Zy(4);

	int help_h = Zy(120);
	Add(help.HSizePos(m, m).VSizePos(y, m));
	need.cy = y + help_h + m;

	for(Ctrl *c = GetFirstChild(); c; c = c->GetNext())
		*c << [this] { Sync(); };
	// Clicking an option also explains it.
	for(int i = 0; i < BOOL_OPTION_COUNT; i++)
		opt[i] << [this, i] { ShowHelp(BOOL_OPTIONS[i].text, BOOL_OPTIONS[i].help); Sync(); };
	for(int i = 0; i < INT_OPTION_COUNT; i++)
		num[i] << [this, i] { ShowHelp(INT_OPTIONS[i].text, INT_OPTIONS[i].help); Sync(); };
	mode << [this] { ShowHelp("Mode", HELP_MODE[clamp((int)~mode, 0, 2)]); Sync(); };
	owners << [this] {
		int o = ~owners;
		for(int i = 0; i < BOOL_OPTION_COUNT; i++) {
			if(BOOL_OPTIONS[i].field == &SyncFlags::preserve_owner)
				opt[i] <<= o != 0;
			if(BOOL_OPTIONS[i].field == &SyncFlags::numeric_ids)
				opt[i] <<= o == 2;
		}
		ShowHelp("Owners", HELP_OWNERS);
		Sync();
	};
	xf << [this] { ShowHelp("Exclude files", HELP_EXCLUDE_FILES); Sync(); };
	xd << [this] { ShowHelp("Exclude folders", HELP_EXCLUDE_DIRS); Sync(); };
	ShowHelp("Help", "Click an option to see what it does. Hover over it to see its command-line name.");
	Set(SyncFlags());
}

void SetupDlg::FlagsPane::ShowHelp(const String& title, const String& text)
{
	// [2: the dialog's text size; each line of the text is its own paragraph.
	String qtf = "[2 [* " + DeQtf(title) + "]";
	for(const String& line : Split(text, '\n'))
		qtf << "&" << DeQtf(line);
	help.SetQTF(qtf + "]");
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
	preview = CommandLineText(WithoutDryRun(FlagArgs(Get())));
}

void SetupDlg::FlagsPane::Set(const SyncFlags& f)
{
	mode <<= (int)f.mode;
	owners <<= !f.preserve_owner ? 0 : f.numeric_ids ? 2 : 1;
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
	// Big enough for the Flags tab as laid out (tab header and buttons around it).
	SetRect(0, 0, max(Zx(520), flags.need.cx + Zx(16)), max(Zy(560), flags.need.cy + Zy(80)));
	Sizeable();

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

void SetupDlg::Screenshots(const String& dir)
{
	const char *name[] = { "setup-source", "setup-target", "setup-flags" };
	for(int i = 0; i < 3; i++) {
		tabs.Set(i);
		SaveScreenshot(*this, AppendFileName(dir, String(name[i]) + ".png"));
	}
	// The Flags tab again, with the help for symlinks showing.
	for(int i = 0; i < BOOL_OPTION_COUNT; i++)
		if(BOOL_OPTIONS[i].field == &SyncFlags::follow_links)
			flags.ShowHelp(BOOL_OPTIONS[i].text, BOOL_OPTIONS[i].help);
	SaveScreenshot(*this, AppendFileName(dir, "setup-flags-help.png"));
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
