// An exception logged with L_EXC on one thread and written by the LOG thread while the throwing
// thread still holds it; the throwing thread then leaves its catch and destroys the exception. The
// line must carry the exception's description, made when it was logged: the LOG thread never
// touches the exception object. Under TSan a LOG thread that did (a virtual what() on it) races with
// its destructor, since the sanitizer cannot see the C++ runtime's reference count on the exception.
//
// Usage: logger_exception_test
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "logger.h"

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)

namespace {

struct Boom : std::runtime_error {
	using std::runtime_error::runtime_error;
	~Boom() override = default;
};

// Keeps what it is given behind a mutex the throwing threads never take, so it orders nothing
// between them and the LOG thread.
class Capture : public Logger {
public:
	std::mutex mtx;
	std::vector<std::string> lines;
	void log(int, std::string_view str, bool, bool) override {
		std::lock_guard<std::mutex> lk(mtx);
		lines.emplace_back(str);
	}
};

}  // namespace

int main() {
	auto capture = std::make_unique<Capture>();
	Capture* cap = capture.get();
	Logging::handlers.clear();
	Logging::add_handler(std::move(capture));
	Logging::config.color = LogColorMode::never;
	Logging::config.with_timestamp = false;
	Logging::config.log_level = LOG_INFO;
	Logging::config.iterm2 = false;
	L_INFO("start");   // the LOG thread and the clocks, before the threads below

	constexpr int kThreads = 4;
	for (int t = 0; t < kThreads; ++t) {
		std::thread th([t] {
			try {
				throw Boom("boom " + std::to_string(t));
			} catch (...) {
				L_EXC("caught {}", t);
				// The LOG thread writes the line meanwhile (it is due at once); the exception is
				// destroyed here, as the catch ends, after the LOG thread has let go of the line.
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
			}
		});
		th.join();
	}
	CHECK(Logging::finish());

	std::lock_guard<std::mutex> lk(cap->mtx);
	for (int t = 0; t < kThreads; ++t) {
		const std::string want = "caught " + std::to_string(t) + ": boom " + std::to_string(t);
		bool found = false;
		for (const auto& l : cap->lines) {
			found |= l.find(want) != std::string::npos;
		}
		if (!found) {
			std::fprintf(stderr, "missing line: %s\n", want.c_str());
		}
		CHECK(found);
	}
	std::printf("exception lines OK: %d lines from other threads carry their description\n", kThreads);
	return 0;
}
