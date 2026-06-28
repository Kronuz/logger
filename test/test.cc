// Smoke test for the logger library — Phases 1 and 2.
//
// Phase 1 (immediate fan-out): install a file sink, emit on the calling thread.
// Phase 2 (scheduler routing): the three paths and the cancel/swap mechanism.
//   - deferred + cancelled before it fires  -> nothing is written (silent)
//   - deferred + outlives its delay          -> fires, written by the LOG thread
//   - async "now"                            -> offloaded, written by the LOG thread
//   - unlog swap on a slow op                -> the swapped-in message is written
//
// The scheduler is timing-based, so the test uses generous windows and never
// asserts tight races.
//
// Build: cmake -B build && cmake --build build && ctest --test-dir build
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>

#include "logger.h"

using namespace std::chrono_literals;

static std::string slurp(const char* path) {
	std::ifstream in(path);
	return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

static std::chrono::steady_clock::time_point in_ms(int ms) {
	return std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
}

int main() {
	char path[] = "/tmp/logger_test_XXXXXX";
	int fd = ::mkstemp(path);
	assert(fd >= 0);
	::close(fd);

	Logging::config.colors = false;
	Logging::config.log_level = LOG_DEBUG;  // let INFO/DEBUG through for the test
	Logging::handlers.clear();
	Logging::handlers.emplace_back(std::make_unique<StreamLogger>(path));

	// --- Phase 1: immediate fan-out -------------------------------------
	Logging::log(LOG_WARNING, "IMMEDIATE_MARKER");

	// --- Phase 2 ---------------------------------------------------------
	// deferred + cancelled before it fires -> silent.
	{
		auto h = Logging::do_log(true, in_ms(200), true, LOG_WARNING, "CANCELLED_MARKER");
		h.clear();
	}

	// deferred + kept alive past its delay -> fires.
	auto kept = Logging::do_log(true, in_ms(100), true, LOG_WARNING, "FIRED_MARKER");

	// async "now" -> offloaded to the LOG thread.
	{
		auto h = Logging::do_log(false, std::chrono::steady_clock::now(), true, LOG_INFO, "ASYNC_MARKER");
	}

	// unlog swap: a "slow op" line armed in the future, replaced on completion.
	{
		auto h = Logging::do_log(true, in_ms(5000), true, LOG_WARNING, "TIMEOUT_MARKER");
		h.unlog(LOG_NOTICE, "DONE_MARKER");
		// handle drops here -> timeout cancelled before firing, swap emitted.
	}

	std::this_thread::sleep_for(400ms);  // let the deferred line fire
	Logging::finish();                   // drain + join the LOG thread

	auto out = slurp(path);
	assert(out.find("IMMEDIATE_MARKER") != std::string::npos);
	assert(out.find("CANCELLED_MARKER") == std::string::npos);  // never fired
	assert(out.find("FIRED_MARKER") != std::string::npos);
	assert(out.find("ASYNC_MARKER") != std::string::npos);
	assert(out.find("TIMEOUT_MARKER") == std::string::npos);    // cancelled
	assert(out.find("DONE_MARKER") != std::string::npos);       // swapped in
	assert(out.find('\033') == std::string::npos);              // colors off -> no ANSI

	std::remove(path);
	std::printf("logger Phase 1+2 smoke test: OK\n");
	return 0;
}
