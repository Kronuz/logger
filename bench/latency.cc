// Latency of one isolated line: nanoseconds from an L_INFO to its handler running, lines 5 ms apart,
// so the LOG thread is idle before each one. Prints the median and the 90th percentile.
//
// Usage: logger_latency [N]   (N lines, 300 by default; built with -DLOGGER_BUILD_BENCH=ON)
// Public API only, so it builds against older commits. Time it under `quiet strict`.
#include "logger.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

using clk = std::chrono::steady_clock;
static std::atomic<long long> done_ns{0};
struct Stamp : Logger {
	void log(int, std::string_view, bool, bool) override {
		done_ns.store(clk::now().time_since_epoch().count(), std::memory_order_release);
	}
};

int main(int argc, char** argv) {
	int n = argc > 1 ? std::atoi(argv[1]) : 300;
	Logging::config.log_level = LOG_INFO;
	Logging::add_handler(std::make_unique<Stamp>());
	std::vector<double> v;
	for (int i = 0; i < n; ++i) {
		done_ns.store(0);
		auto t0 = clk::now().time_since_epoch().count();
		L_INFO("value {}", i);
		while (!done_ns.load(std::memory_order_acquire)) {}
		v.push_back(static_cast<double>(done_ns.load() - t0));
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	std::sort(v.begin(), v.end());
	std::printf("latency median %.1f ns\n", v[v.size() / 2]);
	std::printf("latency p90 %.1f ns\n", v[v.size() * 9 / 10]);
	Logging::finish();
	return 0;
}
