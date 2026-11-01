// How fast the LOG thread drains: log N lines from one thread, then print how many are still pending
// every millisecond until none are. Builds against any version of the library (public API only).
#include "logger.h"

#include <chrono>
#include <cstdio>
#include <thread>

static std::atomic<long> lines;
struct Count : Logger {
	void log(int, std::string_view s, bool, bool) override {
		lines.fetch_add(1, std::memory_order_relaxed);
		asm volatile("" ::"r"(s.data()) : "memory");
	}
};

int main(int argc, char** argv) {
	long n = argc > 1 ? std::atol(argv[1]) : 50000;
	Logging::config.log_level = LOG_INFO;
	Logging::add_handler(std::make_unique<Count>());
	auto t0 = std::chrono::steady_clock::now();
	for (long k = 0; k < n; ++k) {
		L_INFO("value {} and {}", static_cast<int>(k), "x");
	}
	auto t1 = std::chrono::steady_clock::now();
	std::printf("produced %ld lines in %.2f ms (%.1f ns each)\n", n, std::chrono::duration<double, std::milli>(t1 - t0).count(),
		std::chrono::duration<double, std::nano>(t1 - t0).count() / static_cast<double>(n));
	long last = -1;
	int printed = 0;
	while (Logging::pending_count() > 0) {
		auto now = std::chrono::steady_clock::now();
		long p = Logging::pending_count();
		if (p != last && printed < 40) {
			std::printf("  +%.2f ms: pending %ld, written %ld\n", std::chrono::duration<double, std::milli>(now - t0).count(), p, lines.load());
			++printed;
		}
		last = p;
		std::this_thread::sleep_for(std::chrono::microseconds(500));
	}
	auto t2 = std::chrono::steady_clock::now();
	std::printf("drained at +%.2f ms: %.1f ns per line end to end\n", std::chrono::duration<double, std::milli>(t2 - t0).count(),
		std::chrono::duration<double, std::nano>(t2 - t0).count() / static_cast<double>(n));
	Logging::finish();
	return 0;
}
