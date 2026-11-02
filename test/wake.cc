// How soon a routine line is written. With nothing pending, an isolated L_INFO is written at once: the
// LOG thread is woken for it. While a line is pending (here a held watchdog), a routine line joins the
// 1 ms batch: it is due 1 ms after the call and needs no signal.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

#include "logger.h"

using namespace std::chrono_literals;
using clk = std::chrono::steady_clock;

#define CHECK(cond) do { \
	if (!(cond)) { \
		std::fprintf(stderr, "CHECK FAILED at line %d: %s\n", __LINE__, #cond); \
		std::abort(); \
	} \
} while (0)

static std::atomic<long long> done_ns{0};
struct Stamp : Logger {
	void log(int, std::string_view, bool, bool) override {
		done_ns.store(clk::now().time_since_epoch().count(), std::memory_order_release);
	}
};

// The median time from an isolated L_INFO to its write, over `n` lines 5 ms apart.
static std::chrono::nanoseconds median_latency(int n) {
	std::vector<long long> v;
	for (int i = 0; i < n; ++i) {
		std::this_thread::sleep_for(5ms);
		done_ns.store(0);
		const auto t0 = clk::now();
		L_INFO("isolated {}", i);
		while (done_ns.load(std::memory_order_acquire) == 0 && clk::now() - t0 < 5s) {
			std::this_thread::yield();
		}
		CHECK(done_ns.load() != 0);
		v.push_back(done_ns.load() - t0.time_since_epoch().count());
	}
	std::sort(v.begin(), v.end());
	return std::chrono::nanoseconds(v[v.size() / 2]);
}

int main() {
	Logging::handlers.clear();
	Logging::add_handler(std::make_unique<Stamp>());
	Logging::config.log_level = LOG_INFO;
	Logging::config.iterm2 = false;

	// Nothing pending: written at once, well inside the 1 ms batch.
	const auto idle = median_latency(15);
	std::printf("isolated line, nothing pending: median %.1f us\n", static_cast<double>(idle.count()) / 1e3);
	CHECK(idle < 900us);

	// A line pending (a watchdog held for a minute): the next routine line waits for the 1 ms batch.
	{
		auto held = L_DELAYED(true, 60s, LOG_WARNING, "held");
		const auto batched = median_latency(15);
		std::printf("isolated line, a line pending: median %.1f us\n", static_cast<double>(batched.count()) / 1e3);
		CHECK(batched >= 900us);
	}   // the watchdog is cancelled: never written

	Logging::finish();
	std::printf("all wake tests passed\n");
	return 0;
}
