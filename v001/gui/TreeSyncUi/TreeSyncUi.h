#ifndef TREESYNC_UI_H
#define TREESYNC_UI_H

// Shared by the four GUIs: TreeSyncRoot and TreeSyncHome (gui/TreeSync: they sync),
// TreeSyncBuild (builds and tests) and TreeSyncInstall (installs): the display, the
// manual, offscreen screenshots.
#include <CtrlLib/CtrlLib.h>
#include <plugin/png/png.h>
#include <TreeSyncCore/TreeSyncCore.h>

// ~/.config/treesync/build: TreeSyncBuild's choices, and the display all four programs use.
String BuildSettingsPath();

// Display: Wayland (recommended) or X11, chosen in TreeSyncBuild.
bool   WaylandAvailable();
String DisplayNow();                               // "wayland" or "x11": what this program runs on
String DisplayChoice();                            // the saved choice (x11 when there is no Wayland)
String DisplayName(const String& d);
void   ExecOnDisplay(const String& exe, const Vector<String>& args, const String& display);
void   ApplyDisplayChoice();                       // restart on the chosen display if needed

// The manual: Manual/pdf/<chapter>.pdf, three folders above the program (out/<compiler>/),
// or share/doc/treesync/pdf next to an installed bin/, built from the Markdown by
// Manual/build-pdf.sh. Opened in the desktop's PDF viewer. chapter: "TreeSync-Manual"
// (all), "1-Bootstrap", "2-Build", "3-Run", "4-Files-and-Troubleshooting".
void   OpenManual(const String& chapter);
// The Help menu; F1 opens f1_chapter (the one for the program showing the menu).
void   HelpMenu(Bar& bar, const String& f1_chapter);

// --screenshot (a developer aid): draws a window or dialog offscreen into a PNG,
// so the layout can be checked without a screen.
void   SaveScreenshot(Ctrl& c, const String& path);

#endif
