#ifndef TREESYNC_BUILD_H
#define TREESYNC_BUILD_H

// TreeSyncBuild: builds and tests TreeSync, from the source tree, as the user who owns
// it, with bootstrap's requirements check; TreeSyncInstall installs what it built. It is
// never installed: the installed TreeSync (TreeSyncRoot, TreeSyncHome) only runs.
#include <TreeSyncUi/TreeSyncUi.h>

String TreeSyncExe(const String& compiler, const String& name);   // out/<compiler>/<name>
void   BuildScreenshots(const String& folder);
void   BuildRun();                            // --build-run: the saved choices, at once
void   ShowBuildWindow();

#endif
