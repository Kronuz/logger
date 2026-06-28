// Smoke test for the logger library — Phase 1.
//
// Exercises the immediate emit path: install a file sink, log a couple of lines
// on the calling thread, read the file back, and confirm the messages are
// present and (with color off) free of escape sequences.
//
// Build: cmake -B build && cmake --build build && ctest --test-dir build
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <unistd.h>

#include "logger.h"


int main() {
	char path[] = "/tmp/logger_test_XXXXXX";
	int fd = ::mkstemp(path);
	assert(fd >= 0);
	::close(fd);

	Logging::config.colors = false;
	Logging::handlers.clear();
	Logging::handlers.emplace_back(std::make_unique<StreamLogger>(path));

	Logging::log(LOG_WARNING, "hello world");
	Logging::log(LOG_ERR, "second line");

	// Drop the sink so the file descriptor is closed before we read it back.
	Logging::handlers.clear();

	std::ifstream in(path);
	std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

	assert(content.find("hello world") != std::string::npos);
	assert(content.find("second line") != std::string::npos);
	assert(content.find('\033') == std::string::npos);  // colors off -> no ANSI

	std::remove(path);
	std::printf("logger Phase 1 smoke test: OK\n");
	return 0;
}
