// A process that returns from main without finish() while the LOG thread is stalled in a handler:
// the exit-time join times out, and the statics (the handlers among them) are destroyed. The LOG
// thread must have stopped touching them by then.
//
// The child's handler is slow (20 ms a line) and counts its calls; its destructor sets a flag, and a
// call after that exits with status 99. The test counts calls, never times them: the
// child is forced into the timeout by 1200 lines at 20 ms each against the 200 ms join.
//
// Usage: logger_exit_test          the parent: runs the child, expects status 0
//        logger_exit_test child    (internal) the child
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>

#include "logger.h"

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)

static std::atomic<bool> destroyed{false};
static std::atomic<int> calls{0};

class SlowLogger : public Logger {
public:
	void log(int, std::string_view, bool, bool) override {
		if (destroyed.load()) {
			_exit(99);   // a line written through a destroyed handler
		}
		calls.fetch_add(1);
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	~SlowLogger() noexcept override {
		destroyed.store(true);
		std::this_thread::sleep_for(std::chrono::milliseconds(100));   // time for a LOG thread still running to use it
	}
};

static int child() {
	Logging::handlers.clear();
	Logging::add_handler(std::make_unique<SlowLogger>());
	Logging::config.color = LogColorMode::never;
	Logging::config.log_level = LOG_INFO;
	Logging::config.iterm2 = false;
	for (int k = 0; k < 1200; ++k) {
		L_INFO("exit line {}", k);
	}
	return 0;
}

int main(int argc, char** argv) {
	if (argc == 2 && std::strcmp(argv[1], "child") == 0) {
		return child();
	}
	for (int round = 0; round < 3; ++round) {
		std::string cmd = std::string("\"") + argv[0] + "\" child";
		int st = std::system(cmd.c_str());
		CHECK(st == 0);
	}
	std::printf("exit with a stalled LOG thread OK: the child exits 0, no handler used after it is destroyed\n");
	return 0;
}
