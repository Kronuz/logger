// Two children that return from main without finish(), each run three times; the parent expects
// status 0.
//
// `child`: the LOG thread is stalled in a handler, so the exit-time join times out and the statics
// (the handlers among them) are destroyed. The LOG thread must have stopped touching them by then.
// Its handler is slow (20 ms a line) and counts its calls; its destructor sets a flag, and a call
// after that exits with status 99. The test counts calls, never times them: the child is forced
// into the timeout by 1200 lines at 20 ms each against the 200 ms join.
//
// `child-once`: `once` lines are still queued when main returns. The once filter is made by the
// first `once` line, after the exit-time stop is registered, so the statics' destruction reaches it
// first while the LOG thread still writes `once` lines, through the stop's last look. The handler
// counts what it is given; its destructor, which runs after the stop, checks every distinct line
// was written once (status 98 otherwise), and a crash or an abort fails the run.
//
// `child-late`: a static of this file, destroyed after logger.cc's statics (this file is linked
// first, so its statics are made first; the child exits 97 if the order differs), logs a routine
// and a severe line as it is destroyed. Both must reach stderr as they are, after the handler
// (which prefixes its lines with "sink: ") is gone.
//
// Usage: logger_exit_test          the parent: runs `child` three times
//        logger_exit_test once     the parent: runs `child-once` three times
//        logger_exit_test late     the parent: runs `child-late` three times, checking its stderr
//        logger_exit_test child | child-once | child-late    (internal) a child
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

// child-once: 2 lines, then kOnceLines distinct `once` lines, each logged twice.
constexpr int kOnceLines = 60;

class CountingLogger : public Logger {
public:
	void log(int, std::string_view, bool, bool) override {
		if (destroyed.load()) {
			_exit(99);
		}
		calls.fetch_add(1);
		std::this_thread::sleep_for(std::chrono::microseconds(50));   // keeps lines queued at exit
	}
	~CountingLogger() noexcept override {
		destroyed.store(true);
		if (calls.load() != 2 + kOnceLines) {
			std::fprintf(stderr, "child-once: %d lines written, expected %d\n", calls.load(), 2 + kOnceLines);
			_exit(98);
		}
	}
};

static int child_once() {
	Logging::handlers.clear();
	Logging::add_handler(std::make_unique<CountingLogger>());
	Logging::config.color = LogColorMode::never;
	Logging::config.log_level = LOG_INFO;
	Logging::config.iterm2 = false;
	L_INFO("first line");          // starts the LOG thread and registers its exit-time stop
	L_INFO_ONCE("first once line");   // makes the once filter, after the stop
	for (int k = 0; k < 2000 && calls.load() < 2; ++k) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	CHECK(calls.load() == 2);
	for (int k = 0; k < kOnceLines; ++k) {
		L_INFO_ONCE("exit once line {}", k);
		L_INFO_ONCE("exit once line {}", k);   // the filter drops it
	}
	return 0;
}

// child-late: a handler that marks its lines and its destruction, and a static that logs after it.
class MarkingLogger : public Logger {
public:
	void log(int, std::string_view str, bool, bool) override {
		if (destroyed.load()) {
			_exit(99);
		}
		std::string s = "sink: ";
		s += str;
		s += '\n';
		CHECK(::write(STDERR_FILENO, s.data(), s.size()) == static_cast<ssize_t>(s.size()));
		calls.fetch_add(1);
	}
	~MarkingLogger() noexcept override {
		destroyed.store(true);
	}
};

struct LateLogger {
	bool armed = false;
	~LateLogger() {
		if (!armed) {
			return;
		}
		if (!destroyed.load()) {
			std::fprintf(stderr, "child-late: this static is destroyed before the handlers; nothing tested\n");
			_exit(97);
		}
		L_WARNING("late line from a static destructor");
		L_CRIT("late severe line from a static destructor");
	}
};
static LateLogger late;

static int child_late() {
	Logging::handlers.clear();
	Logging::add_handler(std::make_unique<MarkingLogger>());
	Logging::config.color = LogColorMode::never;
	Logging::config.log_level = LOG_INFO;
	Logging::config.iterm2 = false;
	L_INFO("first line");   // starts the LOG thread and registers its exit-time stop
	for (int k = 0; k < 2000 && calls.load() < 1; ++k) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	CHECK(calls.load() == 1);
	late.armed = true;
	return 0;
}

static std::string read_file(const char* path) {
	std::string out;
	if (std::FILE* f = std::fopen(path, "rb")) {
		char b[4096];
		for (std::size_t n; (n = std::fread(b, 1, sizeof b, f)) > 0;) {
			out.append(b, n);
		}
		std::fclose(f);
	}
	return out;
}

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
	if (argc == 2 && std::strcmp(argv[1], "child-once") == 0) {
		return child_once();
	}
	if (argc == 2 && std::strcmp(argv[1], "child-late") == 0) {
		return child_late();
	}
	if (argc == 2 && std::strcmp(argv[1], "late") == 0) {
		const char* err = "logger_exit_late.err";
		for (int round = 0; round < 3; ++round) {
			std::string cmd = std::string("\"") + argv[0] + "\" child-late 2>" + err;
			int st = std::system(cmd.c_str());
			const std::string text = read_file(err);
			if (st != 0) {
				std::fprintf(stderr, "round %d: the child's status is %d; its stderr:\n%s", round, st, text.c_str());
			}
			CHECK(st == 0);
			CHECK(text.find("sink: ") != std::string::npos && text.find("first line\n") != std::string::npos);
			const bool routine = text.find("\nlate line from a static destructor\n") != std::string::npos;
			const bool severe = text.find("\nlate severe line from a static destructor\n") != std::string::npos;
			if (!routine || !severe) {
				std::fprintf(stderr, "round %d: a late line is missing from the child's stderr:\n%s", round, text.c_str());
			}
			CHECK(routine && severe);
		}
		std::remove(err);
		std::printf("exit with a static logging after the teardown OK: its lines reach stderr as they are\n");
		return 0;
	}
	const bool once = argc == 2 && std::strcmp(argv[1], "once") == 0;
	// Freed memory is filled, so a destroyed static the LOG thread reads is garbage, not its old bytes.
	setenv("MallocScribble", "1", 1);
	setenv("MALLOC_PERTURB_", "165", 1);
	for (int round = 0; round < 3; ++round) {
		std::string cmd = std::string("\"") + argv[0] + (once ? "\" child-once" : "\" child");
		int st = std::system(cmd.c_str());
		if (st != 0) {
			std::fprintf(stderr, "round %d: the child's status is %d\n", round, st);
		}
		CHECK(st == 0);
	}
	if (once) {
		std::printf("exit with once lines queued OK: every line written once, no static used after it is destroyed\n");
	} else {
		std::printf("exit with a stalled LOG thread OK: the child exits 0, no handler used after it is destroyed\n");
	}
	return 0;
}
