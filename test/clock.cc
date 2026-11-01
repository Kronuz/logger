// The two clocks the producer path reads: steady_now() must agree with steady_clock::now(), and
// system_at(steady_now()) with system_clock::now(), whatever the host. The logger's tests hook the
// timestamp (or log through do_log, which reads steady_clock), so nothing else compares them.
//
//   steady_now() is between two steady_clock::now() readings taken around it;
//   system_at(steady_now()) is within kSlack of the system_clock::now() readings taken around it
//   (the offset between the clocks is measured again every 20 ms, so it is exact to the drift over that).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "logger.h"

using namespace std::chrono_literals;

#define CHECK(cond) do { \
	if (!(cond)) { \
		std::fprintf(stderr, "CHECK FAILED at line %d: %s\n", __LINE__, #cond); \
		std::abort(); \
	} \
} while (0)

int main() {
	constexpr auto kSlack = 100us;
	std::atomic<long> readings{0};
	std::atomic<long long> worst_ns{0};
	std::vector<std::thread> threads;
	for (int t = 0; t < 4; ++t) {
		threads.emplace_back([&] {
			auto end = std::chrono::steady_clock::now() + 1500ms;   // about 75 re-measurements of the offset
			long n = 0;
			long long worst = 0;
			while (std::chrono::steady_clock::now() < end) {
				for (int i = 0; i < 1000; ++i) {
					auto s0 = std::chrono::system_clock::now();
					auto a = std::chrono::steady_clock::now();
					auto b = logger_detail::steady_now();
					auto c = std::chrono::steady_clock::now();
					auto w = logger_detail::system_at(b);
					auto s1 = std::chrono::system_clock::now();
					CHECK(a <= b && b <= c);
					CHECK(w >= s0 - kSlack);
					CHECK(w <= s1 + kSlack);
					long long early = std::chrono::duration_cast<std::chrono::nanoseconds>(s0 - w).count();
					long long late = std::chrono::duration_cast<std::chrono::nanoseconds>(w - s1).count();
					worst = std::max({worst, early, late});
					++n;
				}
			}
			readings.fetch_add(n);
			long long seen = worst_ns.load();
			while (worst > seen && !worst_ns.compare_exchange_weak(seen, worst)) {
			}
		});
	}
	for (auto& th : threads) {
		th.join();
	}
	std::printf("logger clocks OK: %ld readings from 4 threads; steady_now() inside its steady_clock bracket, "
		"system_at() at most %lld ns outside its system_clock bracket\n", readings.load(), worst_ns.load());
	return 0;
}
