// A runnable tour of the logger.
//
// Build (when this repo is the top-level project):
//   cmake -B build && cmake --build build && ./build/logger_demo
//
// Shows: the severity palette (each level's rgb marker), the grey-gradient
// timestamp, std::format messages, a deferred "log slow ops only" line that is
// cancelled when the op finishes in time and one that fires because the op ran
// long, once-dedup, stacked indentation, exception logging, and the iTerm2
// terminal integration (a no-op unless stderr is iTerm2). Output goes to stderr;
// here it is forced on so the demo is colorful even through a pipe.
#include <chrono>
#include <memory>
#include <stdexcept>
#include <thread>

#include "logger.h"

using namespace std::chrono_literals;


// Arms a "taking too long" warning, then finishes in time and cancels it: the
// warning is never emitted.
static void fast_op() {
	L_DELAYED_200("fast_op is taking too long");
	std::this_thread::sleep_for(20ms);
	L_DELAYED_N_CLEAR();
}

// Arms the same warning, but runs past the 200ms deadline, so the LOG thread
// fires it before this returns.
static void slow_op() {
	L_DELAYED_200("slow_op is taking too long");
	std::this_thread::sleep_for(350ms);
	L_DELAYED_N_CLEAR();
}


int main() {
	// Color: force it on (always) so the demo is colorful even when stderr is not a
	// tty. Drop this and it auto-detects (tty + NO_COLOR + COLORTERM/TERM). The
	// timestamp uses Xapiand's grey gradient; flip timestamp_gradient off for plain.
	Logging::config.color = LogColorMode::always;
	Logging::config.color_depth = LogColorDepth::automatic;  // collapse to this terminal
	Logging::config.timestamp = LogTimestamp::iso8601;       // or ::datetime / ::epoch
	Logging::config.timestamp_gradient = true;
	Logging::config.with_timestamp = true;
	Logging::config.with_threads = true;
	Logging::config.log_level = LOG_DEBUG;  // let every level through
	Logging::add_handler(std::make_unique<StderrLogger>());

	// --- the severity palette: one line per level, each with its own rgb marker ---
	L_INFO("the severity palette, brightest bar = most severe:");
	L_EMERG("L_EMERG   system is unusable");
	L_ALERT("L_ALERT   action must be taken immediately");
	L_CRIT("L_CRIT    critical condition");
	L_ERR("L_ERR     error condition");
	L_WARNING("L_WARNING warning condition");
	L_NOTICE("L_NOTICE  normal but significant");
	L_INFO("L_INFO    informational");

	// --- std::format messages ---
	L_NOTICE("formatted: {} = {}, {:.2f}% done", "answer", 42, 99.5);

	// --- exception logging (describe + optional backtrace via hooks) ---
	try {
		throw std::runtime_error("something broke");
	} catch (...) {
		L_EXC("caught while doing work");
	}

	// --- once-dedup: the second identical line is swallowed ---
	L_INFO_ONCE("this once-line prints only the first time");
	L_INFO_ONCE("this once-line prints only the first time");  // deduped, silent

	// --- stacked indentation: logs inside the scope nest one level deeper ---
	{
		L_STACKED(LOG_INFO, "processing batch");
		L_INFO("item A");  // nested one level
		L_INFO("item B");  // nested one level
	}

	// --- deferred "log slow ops only": fires only if the op misses its deadline ---
	fast_op();  // no "too long" line: finished in time
	slow_op();  // logs the "too long" line: ran long

	// --- iTerm2 integration: no-ops unless stderr is an iTerm2 terminal ---------
	// A navigable mark, a tab title (saved and restored at finish()), a tab tint,
	// a badge, and a notification.
	Logging::set_mark();
	Logging::tab_title("logger demo");
	Logging::tab_rgb(63, 119, 179);
	Logging::badge("demo");
	Logging::growl("logger demo finished");

	L_INFO("logger demo done");

	// Drain and stop the LOG thread, and restore the terminal (clears the iTerm2
	// badge / tint and pops the saved title) before exit.
	Logging::finish();
	return 0;
}
