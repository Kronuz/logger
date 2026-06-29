// A small runnable demonstration of the logger.
//
// Build (when this repo is the top-level project):
//   cmake -B build && cmake --build build && ./build/logger_demo
//
// Shows the level macros, a deferred "log slow ops only" line that is cancelled
// when the op finishes in time, one that fires because the op runs long,
// once-dedup, and stacked indentation. Output goes to stderr, colorized when it
// is a tty.
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
	Logging::config.color = LogColorMode::always;  // force color regardless of tty
	Logging::config.timestamp_gradient = true;     // show the grey-ramp timestamp
	Logging::config.with_timestamp = true;
	Logging::config.with_threads = true;
	Logging::config.log_level = LOG_INFO;
	Logging::add_handler(std::make_unique<StderrLogger>());

	L_INFO("logger demo starting");
	L_NOTICE("a notice: {} = {}", "answer", 42);
	L_WARNING("a warning with a number: {}", 7);

	try {
		throw std::runtime_error("something broke");
	} catch (...) {
		L_EXC("caught while doing work");
	}

	L_INFO_ONCE("this once-line prints only the first time");
	L_INFO_ONCE("this once-line prints only the first time");  // deduped, silent

	{
		L_STACKED(LOG_INFO, "processing batch");
		L_INFO("item A");  // nested one level
		L_INFO("item B");  // nested one level
	}

	fast_op();  // no "too long" line: finished in time
	slow_op();  // logs the "too long" line: ran long

	L_INFO("logger demo done");

	Logging::finish();  // drain and stop the LOG thread before exit
	return 0;
}
