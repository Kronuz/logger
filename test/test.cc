// Smoke test for the logger library — Phases 1 through 3.
//
// Phase 1 (immediate fan-out): emit on the calling thread through a file sink.
// Phase 2 (scheduler routing): the three paths and the cancel/swap mechanism.
// Phase 3 (richness): exception decoration, `once` dedup, backpressure drops.
//
// Uses CHECK (not assert) so the test verifies even in NDEBUG/Release builds.
// The scheduler is timing-based, so the test uses generous windows and never
// asserts tight races.
//
// Build: cmake -B build && cmake --build build && ctest --test-dir build
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

#include "logger.h"

using namespace std::chrono_literals;

#define CHECK(cond) do { \
	if (!(cond)) { \
		std::fprintf(stderr, "CHECK FAILED at line %d: %s\n", __LINE__, #cond); \
		std::abort(); \
	} \
} while (0)

static std::string slurp(const char* path) {
	std::ifstream in(path);
	return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

static std::chrono::steady_clock::time_point in_ms(int ms) {
	return std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
}

static int count_occurrences(const std::string& hay, const std::string& needle) {
	int n = 0;
	for (size_t pos = 0; (pos = hay.find(needle, pos)) != std::string::npos; pos += needle.size()) {
		++n;
	}
	return n;
}

int main() {
	char path[] = "/tmp/logger_test_XXXXXX";
	int fd = ::mkstemp(path);
	CHECK(fd >= 0);
	::close(fd);

	Logging::config.colors = false;
	Logging::config.log_level = LOG_DEBUG;  // let INFO/DEBUG through for the test
	Logging::handlers.clear();
	Logging::handlers.emplace_back(std::make_unique<StreamLogger>(path));

	// --- Phase 1: immediate fan-out -------------------------------------
	Logging::log(LOG_WARNING, "IMMEDIATE_MARKER");

	// --- Phase 2: routing -----------------------------------------------
	{
		auto h = Logging::do_log(true, in_ms(200), true, LOG_WARNING, "CANCELLED_MARKER");
		h.clear();  // cancelled before it fires -> silent
	}
	auto kept = Logging::do_log(true, in_ms(100), true, LOG_WARNING, "FIRED_MARKER");  // outlives delay -> fires
	{
		auto h = Logging::do_log(false, std::chrono::steady_clock::now(), true, LOG_INFO, "ASYNC_MARKER");
	}

	// --- Phase 3: backpressure (early, so a later line reports the drops) -
	Logging::config.max_pending = 5;
	std::vector<Log> held;
	for (int i = 0; i < 5; ++i) {
		held.push_back(Logging::do_log(true, in_ms(60000), true, LOG_WARNING, "FILLER_MARKER"));
	}
	for (int i = 0; i < 10; ++i) {
		auto h = Logging::do_log(false, std::chrono::steady_clock::now(), true, LOG_INFO, "DROPME_MARKER");
	}
	CHECK(Logging::dropped_count() >= 1);  // routine lines dropped while saturated
	Logging::config.max_pending = 0;

	// --- Phase 2/3: cancel/swap, fast op stays silent -------------------
	{
		auto h = Logging::do_log(true, in_ms(60000), true, LOG_WARNING, "FAST_TIMEOUT_MARKER");
		h.unlog(LOG_NOTICE, "FAST_DONE_MARKER");
		// handle drops here, long before the timeout: both suppressed.
	}

	// --- Phase 2/3: cancel/swap, slow op fires timeout then swap --------
	{
		auto h = Logging::do_log(true, in_ms(30), true, LOG_WARNING, "SLOW_TIMEOUT_MARKER");
		h.unlog(LOG_NOTICE, "SLOW_DONE_MARKER");
		std::this_thread::sleep_for(120ms);  // let the timeout fire first
		// handle drops here, after the timeout fired: swap is emitted.
	}

	// --- Phase 3: exception decoration ----------------------------------
	std::exception_ptr eptr;
	try {
		throw std::runtime_error("EXC_DETAIL");
	} catch (...) {
		eptr = std::current_exception();
	}
	Logging::do_log(false, std::chrono::steady_clock::now(), false, true, 0, LOG_ERR,
		eptr, "func", "file.cc", 42, "EXC_MARKER");

	// --- Phase 3: `once` dedup ------------------------------------------
	for (int i = 0; i < 3; ++i) {
		Logging::do_log(false, std::chrono::steady_clock::now(), false, true, 0xABCDEFu, LOG_NOTICE,
			std::exception_ptr{}, nullptr, nullptr, 0, "ONCE_MARKER");
	}

	std::this_thread::sleep_for(300ms);  // let async/deferred lines fire
	Logging::finish();                   // drain + join the LOG thread

	auto out = slurp(path);
	CHECK(out.find("IMMEDIATE_MARKER") != std::string::npos);
	CHECK(out.find("CANCELLED_MARKER") == std::string::npos);    // never fired
	CHECK(out.find("FIRED_MARKER") != std::string::npos);
	CHECK(out.find("ASYNC_MARKER") != std::string::npos);
	CHECK(out.find("FAST_TIMEOUT_MARKER") == std::string::npos); // fast op: silent
	CHECK(out.find("FAST_DONE_MARKER") == std::string::npos);    // fast op: silent
	CHECK(out.find("SLOW_TIMEOUT_MARKER") != std::string::npos); // slow op: timeout fired
	CHECK(out.find("SLOW_DONE_MARKER") != std::string::npos);    // slow op: swap emitted
	CHECK(out.find("EXC_MARKER") != std::string::npos);
	CHECK(out.find("EXC_DETAIL") != std::string::npos);          // exception described
	CHECK(count_occurrences(out, "ONCE_MARKER") == 1);           // deduped
	CHECK(out.find("FILLER_MARKER") == std::string::npos);       // far-future, never fired
	CHECK(out.find("DROPME_MARKER") == std::string::npos);       // dropped under backpressure
	CHECK(out.find("backpressure") != std::string::npos);        // drop summary emitted
	CHECK(out.find('\033') == std::string::npos);                // colors off -> no ANSI

	std::remove(path);
	std::printf("logger Phase 1-3 smoke test: OK\n");
	return 0;
}
