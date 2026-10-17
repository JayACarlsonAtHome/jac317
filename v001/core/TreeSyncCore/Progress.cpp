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
	std::string total = std::to_string(p.count) + (p.more ? "+" : "");
	std::string index = std::to_string(p.index);
	index.insert(0, total.size() - min(total.size(), index.size()), ' ');
	std::string counter = "[" + index + "/" + total + "] ";
	int pct = p.size > 0 ? (int)min<int64>(100, p.done * 100 / p.size) : 100;
	std::string thermo;
	if(p.verifying)
		thermo = " verify";
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

// Durations -------------------------------------------------------------------

namespace {

struct Parts { int64 years, days, hours, minutes, seconds; };

Parts SplitDuration(int64 total)
{
	if(total < 0)
		total = -total;
	const int64 DAY = 86400;
	const double YEAR = 146097.0 * DAY / 400;   // days in 400 Gregorian years, per year
	Parts p;
	p.years = min<int64>((int64)(total / YEAR), 999);
	int64 rest = total - (int64)(p.years * YEAR);
	p.days = rest / DAY;
	rest %= DAY;
	p.hours = rest / 3600;
	p.minutes = rest / 60 % 60;
	p.seconds = rest % 60;
	return p;
}

// Fixed width so lines up in a column: the number padded (spaces, or zeros for
// minutes and seconds), and a singular unit takes a space in place of the 's'.
std::string Unit(int64 n, const char *name, int width, bool zeros)
{
	// Padded by hand: clang 21 rejects a dynamic width in std::format (docs/findings).
	std::string num = std::to_string(n);
	if((int)num.size() < width)
		num.insert(0, width - num.size(), zeros ? '0' : ' ');
	return num + " " + name + (n == 1 ? " " : "s");
}

}

String DurationClock(int64 seconds)
{
	Parts p = SplitDuration(seconds);
	return String(std::format("{:03}:{:03}:{:02}:{:02}:{:02}", p.years, p.days, p.hours, p.minutes, p.seconds));
}

String DurationWords(int64 seconds)
{
	Parts p = SplitDuration(seconds);
	std::string s;
	if(p.years)
		s += Unit(p.years, "year", 3, false) + " ";
	if(p.years || p.days)
		s += Unit(p.days, "day", 3, false) + " ";
	s += Unit(p.hours, "hour", 2, false) + " " + Unit(p.minutes, "minute", 2, true) + " " +
	     Unit(p.seconds, "second", 2, true);
	String r(s);
	r.TrimEnd(" ");   // a singular "second " at the very end
	return r;
}

String ClockTime(int64 unix_time)
{
	time_t t = (time_t)unix_time;
	struct tm lt;
	localtime_r(&t, &lt);
	return String(std::format("{:02}:{:02}:{:02}", lt.tm_hour, lt.tm_min, lt.tm_sec));
}
