// TreeSyncRoot: the sync window, every sync run as root (treesync-cli through
// pkexec, the GUI itself stays the user). For administrators: root, wheel, sudo, admin.
#include <TreeSync/TreeSync.h>

GUI_APP_MAIN
{
	TreeSyncMain(true);
}
