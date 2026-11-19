// Two children that return from main without finish(), each run three times; the parent expects
// status 0.
//
// `child`: the LOG thread is stalled in a handler, so the exit-time stop gives up on it and the
// statics (the handlers among them) are destroyed. The LOG thread must have stopped touching them by
// then. Its handler stalls 1 s on its third line, against the stop's 200 ms without a written line,
// and counts its calls; its destructor sets a flag, and a call after that exits with status 99.
//
// `child-slow`: the drain is slow but never stalled: 400 lines queued at exit, 1 ms each, take
// longer than one 200 ms join. Every line is written (status 96 otherwise): the stop waits while
// lines are written.
//
// `child-capped`: the drain is steady but too long: 200 lines at 50 ms each, 10 s against the stop's
// 5 s cap. The stop cuts it and reports on stderr at most how many lines it did not write; with one
// producer and no deferred lines the bound is exact, so the parent checks that count and the
// handler's own add up to 200, and that some were cut.
//
// `child-once`: `once` lines are still queued when main returns. The once filter is made by the
// first `once` line, after the exit-time stop is registered, so the statics' destruction reaches it
// first while the LOG thread still writes `once` lines, through the stop's last look. The handler
// counts what it is given; its destructor, which runs after the stop, checks every distinct line
// was written once (status 98 otherwise), and a crash or an abort fails the run.
//
// `child-late`: a static of this file, destroyed after logger.cc's statics (this file is linked
// first, so its statics are made first; the child exits 97 if the order differs), logs a routine
// and a severe line as it is destroyed, and an `L_EXC` line. All must reach stderr as they are, after
// the handler (which prefixes its lines with "sink: ") is gone; the exception line carries its
// description, made without the host's describe hook, which is destroyed with `Logging::hooks` (it
// is heap-held, flags its destruction, and exits 95 if called after it).
//
// `child-fork`: a fork child of a process whose LOG thread runs logs a line and returns from main.
// It has no LOG thread, so its exit must not wait for one; the parent kills it after 8 s.
//
// `child-logexit`: a handler calls exit() on the LOG thread. The exit-time stop must not wait for
// the thread it runs on.
//
// Usage: logger_exit_test          the parent: runs `child` three times
//        logger_exit_test once     the parent: runs `child-once` three times
//        logger_exit_test late     the parent: runs `child-late` three times, checking its stderr
//        logger_exit_test slow     the parent: runs `child-slow` three times
//        logger_exit_test capped   the parent: runs `child-capped` once, checking its stderr
//        logger_exit_test fork     the parent: runs `child-fork` once, within 10 s
//        logger_exit_test logexit  the parent: runs `child-logexit` once, within 10 s
//        logger_exit_test child | child-once | child-late | child-slow | child-capped | child-fork |
//                         child-logexit    (internal) a child
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <array>
#include <exception>
#include <memory>
#include <stdexcept>
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
		if (calls.fetch_add(1) == 2) {
			std::this_thread::sleep_for(std::chrono::seconds(1));   // stalled
		}
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

// child-slow: kSlowLines lines queued at exit, kSlowLines ms to write.
constexpr int kSlowLines = 400;

class PacedLogger : public Logger {
public:
	void log(int, std::string_view, bool, bool) override {
		if (destroyed.load()) {
			_exit(99);
		}
		calls.fetch_add(1);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	~PacedLogger() noexcept override {
		destroyed.store(true);
		if (calls.load() != kSlowLines) {
			std::fprintf(stderr, "child-slow: %d lines written, expected %d\n", calls.load(), kSlowLines);
			_exit(96);
		}
	}
};

static int child_slow() {
	Logging::handlers.clear();
	Logging::add_handler(std::make_unique<PacedLogger>());
	Logging::config.color = LogColorMode::never;
	Logging::config.log_level = LOG_INFO;
	Logging::config.iterm2 = false;
	for (int k = 0; k < kSlowLines; ++k) {
		L_INFO("slow line {}", k);
	}
	return 0;
}

// child-capped: kCappedLines lines queued at exit, 50 ms each, twice the stop's cap.
constexpr int kCappedLines = 200;

class SteadyLogger : public Logger {
public:
	void log(int, std::string_view, bool, bool) override {
		if (destroyed.load()) {
			_exit(99);
		}
		calls.fetch_add(1);
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	~SteadyLogger() noexcept override {
		destroyed.store(true);
		std::fprintf(stderr, "child-capped: %d lines written\n", calls.load());
	}
};

static int child_capped() {
	Logging::handlers.clear();
	Logging::add_handler(std::make_unique<SteadyLogger>());
	Logging::config.color = LogColorMode::never;
	Logging::config.log_level = LOG_INFO;
	Logging::config.iterm2 = false;
	for (int k = 0; k < kCappedLines; ++k) {
		L_INFO("capped line {}", k);
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

static std::atomic<bool> hook_destroyed{false};

// A describe hook too big for std::function's buffer, so it is freed with Logging::hooks.
struct DescribeHook {
	bool armed = false;
	std::array<char, 256> pad{};
	std::string operator()(std::exception_ptr) const {
		if (hook_destroyed.load()) {
			_exit(95);   // the host's hook called after Logging::hooks was destroyed
		}
		return "described by the hook" + std::string(pad[0] != 0 ? "!" : "");
	}
	~DescribeHook() {
		if (armed) {
			hook_destroyed.store(true);
		}
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
		if (!hook_destroyed.load()) {
			std::fprintf(stderr, "child-late: this static is destroyed before the hooks; nothing tested\n");
			_exit(97);
		}
		try {
			throw std::runtime_error("late boom");
		} catch (...) {
			L_EXC("late exception line");
		}
	}
};
static LateLogger late;

static int child_late() {
	Logging::handlers.clear();
	Logging::add_handler(std::make_unique<MarkingLogger>());
	Logging::hooks.describe_exception = DescribeHook{};
	Logging::hooks.describe_exception.target<DescribeHook>()->armed = true;
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

// child-fork: the grandchild returns from main with the logger's exit-time stop inherited.
static int child_fork() {
	Logging::handlers.clear();
	Logging::add_handler(std::make_unique<MarkingLogger>());
	Logging::config.color = LogColorMode::never;
	Logging::config.log_level = LOG_INFO;
	Logging::config.iterm2 = false;
	L_INFO("before fork");   // starts the LOG thread and registers its exit-time stop
	for (int k = 0; k < 2000 && calls.load() < 1; ++k) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	CHECK(calls.load() == 1);
	const pid_t pid = fork();
	CHECK(pid >= 0);
	if (pid == 0) {
		L_INFO("in the fork child");
		return 0;
	}
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
	int st = 0;
	while (waitpid(pid, &st, WNOHANG) == 0) {
		if (std::chrono::steady_clock::now() > deadline) {
			kill(pid, SIGKILL);
			waitpid(pid, &st, 0);
			std::fprintf(stderr, "child-fork: the fork child hung at exit\n");
			_exit(1);
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
	return 0;
}

// child-logexit: a handler that calls exit() on the LOG thread.
class ExitingLogger : public Logger {
public:
	void log(int, std::string_view str, bool, bool) override {
		if (str.find("bye") != std::string_view::npos) {
			std::exit(0);
		}
	}
};

static int child_logexit() {
	Logging::handlers.clear();
	Logging::add_handler(std::make_unique<ExitingLogger>());
	Logging::config.color = LogColorMode::never;
	Logging::config.log_level = LOG_INFO;
	Logging::config.iterm2 = false;
	L_INFO("bye");
	std::this_thread::sleep_for(std::chrono::seconds(30));
	return 3;   // the handler's exit() never came
}

// Runs `self mode`; kills it after `seconds`. Its wait status, or -1 if it was killed.
static int run_bounded(const char* self, const char* mode, int seconds) {
	const pid_t pid = fork();
	CHECK(pid >= 0);
	if (pid == 0) {
		execl(self, self, mode, static_cast<char*>(nullptr));
		_exit(127);
	}
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
	int st = 0;
	while (waitpid(pid, &st, WNOHANG) == 0) {
		if (std::chrono::steady_clock::now() > deadline) {
			kill(pid, SIGKILL);
			waitpid(pid, &st, 0);
			return -1;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	return st;
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
	if (argc == 2 && std::strcmp(argv[1], "child-slow") == 0) {
		return child_slow();
	}
	if (argc == 2 && std::strcmp(argv[1], "child-capped") == 0) {
		return child_capped();
	}
	if (argc == 2 && std::strcmp(argv[1], "child-fork") == 0) {
		return child_fork();
	}
	if (argc == 2 && std::strcmp(argv[1], "child-logexit") == 0) {
		return child_logexit();
	}
	if (argc == 2 && (std::strcmp(argv[1], "fork") == 0 || std::strcmp(argv[1], "logexit") == 0)) {
		const bool fork_mode = std::strcmp(argv[1], "fork") == 0;
		const int st = run_bounded(argv[0], fork_mode ? "child-fork" : "child-logexit", 10);
		if (st != 0) {
			std::fprintf(stderr, "the child's status is %d%s\n", st, st == -1 ? " (hung at exit, killed after 10 s)" : "");
		}
		CHECK(st == 0);
		std::printf(fork_mode ? "exit in a fork child OK: it does not wait for a LOG thread it does not have\n"
			: "exit on the LOG thread OK: the stop does not wait for its own thread\n");
		return 0;
	}
	if (argc == 2 && std::strcmp(argv[1], "capped") == 0) {
		const char* err = "logger_exit_capped.err";
		std::string cmd = std::string("\"") + argv[0] + "\" child-capped 2>" + err;
		int st = std::system(cmd.c_str());
		const std::string text = read_file(err);
		long cut = -1, written = -1;
		if (auto at = text.find("(5 s cap): at most "); at != std::string::npos) {
			cut = std::strtol(text.c_str() + at + 19, nullptr, 10);
		}
		if (auto at = text.find("child-capped: "); at != std::string::npos) {
			written = std::strtol(text.c_str() + at + 14, nullptr, 10);
		}
		if (st != 0 || cut <= 0 || written < 0 || cut + written != kCappedLines) {
			std::fprintf(stderr, "the child's status is %d, %ld lines cut, %ld written; its stderr:\n%s", st, cut, written, text.c_str());
		}
		CHECK(st == 0);
		CHECK(cut > 0 && written >= 0 && cut + written == kCappedLines);
		std::remove(err);
		std::printf("exit with a drain past the cap OK: %ld lines written, %ld cut and reported\n", written, cut);
		return 0;
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
			const bool exc = text.find("\nlate exception line: late boom\n") != std::string::npos;
			if (!routine || !severe || !exc) {
				std::fprintf(stderr, "round %d: a late line is missing from the child's stderr:\n%s", round, text.c_str());
			}
			CHECK(routine && severe && exc);
		}
		std::remove(err);
		std::printf("exit with a static logging after the teardown OK: its lines reach stderr as they are\n");
		return 0;
	}
	const bool once = argc == 2 && std::strcmp(argv[1], "once") == 0;
	const bool slow = argc == 2 && std::strcmp(argv[1], "slow") == 0;
	// Freed memory is filled, so a destroyed static the LOG thread reads is garbage, not its old bytes.
	setenv("MallocScribble", "1", 1);
	setenv("MALLOC_PERTURB_", "165", 1);
	for (int round = 0; round < 3; ++round) {
		std::string cmd = std::string("\"") + argv[0] + (once ? "\" child-once" : slow ? "\" child-slow" : "\" child");
		int st = std::system(cmd.c_str());
		if (st != 0) {
			std::fprintf(stderr, "round %d: the child's status is %d\n", round, st);
		}
		CHECK(st == 0);
	}
	if (once) {
		std::printf("exit with once lines queued OK: every line written once, no static used after it is destroyed\n");
	} else if (slow) {
		std::printf("exit with a slow drain OK: every queued line written, past one 200 ms join\n");
	} else {
		std::printf("exit with a stalled LOG thread OK: the child exits 0, no handler used after it is destroyed\n");
	}
	return 0;
}
