// Copy progress lines in the style of Fedora's dnf5:
//
//   [  3/108] photos/2024/beach day.jpg       100% |  12.4 MiB/s |   3.1 MiB |  00m00s
//   [108/108] Total                            100% |  14.4 MiB/s | 300.2 MiB |  00m21s
//
// The engine prints one per file to the terminal; the GUI can show the same lines.
#include "TreeSyncCore.h"

#include <format>

static String Rate(int64 done, double seconds)
{
	return seconds > 0 ? FormatBytes((int64)(done / seconds)) + "/s" : String("---");
}

static std::string Elapsed(double seconds)
{
	int s = (int)seconds;
	if(s >= 3600)
		return std::format("{}h{:02}m", s / 3600, s / 60 % 60);
	return std::format("{:02}m{:02}s", s / 60, s % 60);
}

// Fits name into width code points (UTF-8 safe), cutting from the front so the
// file name itself stays visible, then pads with spaces.
static String FitName(const String& name, int width)
{
	WString w = name.ToWString();
	if(w.GetCount() > width)
		w = "..." + w.Right(max(width - 3, 0));
	return w.ToString() + String(' ', width - w.GetCount());
}

String ProgressLine(const CopyProgress& p, int width, bool bar)
{
	TS_PRE(p.count >= 0 && p.index >= 0 && p.index <= p.count);
	std::string total = std::to_string(p.count);
	std::string index = std::to_string(p.index);
	index.insert(0, total.size() - min(total.size(), index.size()), ' ');
	std::string counter = "[" + index + "/" + total + "] ";
	int pct = p.size > 0 ? (int)min<int64>(100, p.done * 100 / p.size) : 100;
	std::string thermo;
	if(bar) {
		const int W = 20;
		int fill = pct * W / 100;
		thermo = " [" + std::string(fill, '=') + std::string(W - fill, ' ') + "]";
	}
	std::string tail = std::format(" {:>3}%{} | {:>12} | {:>10} | {:>7}", pct, thermo,
	                               Rate(p.done, p.seconds).ToStd(), FormatBytes(p.done).ToStd(),
	                               Elapsed(p.seconds));
	int name_width = max(10, width - (int)counter.size() - (int)tail.size());
	return String(counter) + FitName(p.name, name_width) + String(tail);
}
