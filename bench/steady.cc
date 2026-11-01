// A producer loop for profiling: logs 50,000 lines on one thread while the LOG thread drains, waits for
// the drain, and repeats for a number of seconds, printing the producer's cost per line. Run it, then
// `sample steady 3` on macOS or `perf record -p` on Linux. Builds against any version of the library
// (public API only).
#include "logger.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

static std::atomic<long> lines;
struct Count : Logger {
	void log(int, std::string_view s, bool, bool) override {
		lines.fetch_add(1, std::memory_order_relaxed);
		asm volatile("" ::"r"(s.data()) : "memory");
	}
};

int main(int argc, char** argv) {
	int seconds = argc > 1 ? std::atoi(argv[1]) : 10;
	Logging::config.log_level = LOG_INFO;
	Logging::add_handler(std::make_unique<Count>());
	constexpr long kBatch = 50000;
	auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
	double produced_ns = 0;
	long n = 0;
	auto report_at = std::chrono::steady_clock::now() + std::chrono::seconds(1);
	while (std::chrono::steady_clock::now() < end) {
		auto t0 = std::chrono::steady_clock::now();
		for (long k = 0; k < kBatch; ++k) {
			L_INFO("value {} and {}", static_cast<int>(k), "x");
		}
		auto t1 = std::chrono::steady_clock::now();
		produced_ns += std::chrono::duration<double, std::nano>(t1 - t0).count();
		n += kBatch;
		while (Logging::pending_count() > 0) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		if (std::chrono::steady_clock::now() >= report_at) {
			std::printf("%.1f ns per line produced (%ld lines)\n", produced_ns / static_cast<double>(n), n);
			std::fflush(stdout);
			report_at += std::chrono::seconds(1);
		}
	}
	Logging::finish();
	return 0;
}
