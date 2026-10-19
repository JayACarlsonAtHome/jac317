// TreeSyncHome: the sync window, every sync run as the user, inside their home.
// For anyone; it does not run as root.
#include <TreeSync/TreeSync.h>

GUI_APP_MAIN
{
	TreeSyncMain(false);
}
