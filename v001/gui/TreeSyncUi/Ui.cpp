// What the four TreeSync GUIs share: the display (Wayland or X11), the
// manual and its Help menu, and offscreen screenshots.
#include "TreeSyncUi.h"

#include <unistd.h>
#include <vector>

String BuildSettingsPath()
{
	return AppendFileName(GetFileFolder(SettingsPath()), "build");
}

// Display: Wayland (recommended) or X Windows (X11, not recommended: any X11 program
// can read the keyboard and the screen of the others). GTK picks by GDK_BACKEND.
bool WaylandAvailable()
{
	return GetEnv("WAYLAND_DISPLAY").GetCount();
}

String DisplayNow()
{
	return Ctrl::IsWayland() ? "wayland" : "x11";
}

String DisplayChoice()
{
	String d = "wayland";
	for(const String& line : Split(LoadFile(BuildSettingsPath()), '\n'))
		if(line == "display=x11")
			d = "x11";
	return WaylandAvailable() ? d : String("x11");
}

String DisplayName(const String& d)
{
	return d == "wayland" ? "Wayland" : "X Windows (X11)";
}

// Starts exe (with args) on the chosen display, in place of this program.
void ExecOnDisplay(const String& exe, const Vector<String>& args, const String& display)
{
	setenv("GDK_BACKEND", display == "wayland" ? "wayland" : "x11", 1);
	setenv("TREESYNC_DISPLAY_SET", "1", 1);   // the started program does not switch again
	std::vector<char *> argv;
	argv.push_back(const_cast<char *>(~exe));
	for(const String& a : args)
		argv.push_back(const_cast<char *>(~a));
	argv.push_back(nullptr);
	execv(exe, argv.data());
	Exclamation("Could not start " + DeQtf(exe) + ": " + DeQtf(strerror(errno)));
}

// At start: if this program came up on the other display than the chosen one, start
// it again on the chosen one (once; GDK_BACKEND is only read when GTK starts).
void ApplyDisplayChoice()
{
	if(GetEnv("TREESYNC_DISPLAY_SET").GetCount() || DisplayNow() == DisplayChoice())
		return;
	ExecOnDisplay(GetExeFilePath(), CommandLine(), DisplayChoice());
}

// The manual -------------------------------------------------------------------------

void OpenManual(const String& chapter)
{
	// In the source tree: Manual/ three folders up; installed: share/doc/treesync next to bin/.
	String folder = NormalizePath(AppendFileName(GetExeFolder(), "../../../Manual"));
	String installed = NormalizePath(AppendFileName(GetExeFolder(), "../share/doc/treesync"));
	if(!DirectoryExists(folder) && DirectoryExists(installed))
		folder = installed;
	String pdf = AppendFileName(folder, "pdf/" + chapter + ".pdf");
	if(!FileExists(pdf)) {
		Exclamation("The manual is not built yet: [C " + DeQtf(pdf) + "] is missing.&&"
		            "Build it with [C " + DeQtf(AppendFileName(folder, "build-pdf.sh")) + "], or read the "
		            "Markdown sources in [C " + DeQtf(folder) + "].");
		return;
	}
	LaunchWebBrowser(pdf);   // the desktop's PDF viewer (xdg-open)
}

void HelpMenu(Bar& bar, const String& f1_chapter)
{
	bar.Add("Manual (all chapters)", [] { OpenManual("TreeSync-Manual"); })
	   .Help("The whole manual as one PDF");
	bar.Separator();
	bar.Add("1. Bootstrap", [] { OpenManual("1-Bootstrap"); })
	   .Help("From a fresh machine to a first build: requirements, U++, bootstrap.sh");
	bar.Add("2. Build", [] { OpenManual("2-Build"); }).Key(f1_chapter == "2-Build" ? K_F1 : 0)
	   .Help("build.sh, the Build page, debug and release, tests, verify.sh and the pre-push hook");
	bar.Add("3. Run", [] { OpenManual("3-Run"); }).Key(f1_chapter == "3-Run" ? K_F1 : 0)
	   .Help("The GUI step by step, the command line, remote trees, root, checking and attributes");
	bar.Add("4. Files and troubleshooting", [] { OpenManual("4-Files-and-Troubleshooting"); })
	   .Help("Where everything is kept, and what to do when something goes wrong");
}

void SaveScreenshot(Ctrl& c, const String& path)
{
	Size sz = c.GetRect().GetSize();
	// Never shown, so lay it out here: a tree adds and places its checkboxes while it
	// paints (a first, thrown-away draw), then the layout puts them where they belong.
	{
		ImageDraw warm(sz);
		c.DrawCtrl(warm);
	}
	c.RefreshLayoutDeep();
	ImageDraw iw(sz);
	iw.DrawRect(sz, SColorFace());
	c.DrawCtrl(iw);
	PNGEncoder().SaveFile(path, iw);
}

