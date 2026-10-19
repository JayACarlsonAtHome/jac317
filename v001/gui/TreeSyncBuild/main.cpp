// TreeSyncBuild: the program you build and test TreeSync with. It runs as you, in the
// source tree (v001/out/<compiler>/treesync-build); root only runs TreeSync, so as
// root it says so and stops.
#include "TreeSyncBuild.h"

GUI_APP_MAIN
{
	const Vector<String>& args = CommandLine();
	for(const String& a : args)
		if(a.StartsWith("--screenshot=")) {   // the screens drawn offscreen into PNGs
			RealizeDirectory(a.Mid(13));
			BuildScreenshots(a.Mid(13));
			Cout() << "screenshots in " << a.Mid(13) << "\n";
			return;
		}
	if(IsRoot()) {
		Exclamation("[* TreeSyncBuild does not run as root.]&&Build TreeSync as the user who owns the source "
		            "(you), then use [* TreeSyncInstall]: root runs the installed TreeSync, it does not build it.");
		SetExitCode(2);
		return;
	}
	if(FindIndex(args, String("--build-run")) >= 0) {   // a developer aid: Build, started at once
		BuildRun();
		return;
	}
	ApplyDisplayChoice();   // Wayland or X11, as chosen in this window
	ShowBuildWindow();
}
