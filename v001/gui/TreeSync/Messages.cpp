// Core messages, shown in a dialog and printed to the terminal (stderr), so a
// run started from a shell keeps a record of them.
#include "TreeSync.h"

#include <unistd.h>

static void Warn(const String& text)
{
	// Straight to fd 2, unbuffered, so it is in the terminal before the dialog blocks.
	String line = ToLower(GetExeTitle()) + ": " + Join(Split(text, '\n'), "\n  ") + "\n";
	if(write(2, line.Begin(), line.GetCount()) < 0)
		;   // nowhere left to report it
	String qtf;
	for(const String& line : Split(text, '\n'))
		qtf << (qtf.GetCount() ? "&&" : "") << DeQtf(line);
	Exclamation(qtf);
}

void RefusePath(const String& p)
{
	Warn(RefusedPathMessage(p));
}
