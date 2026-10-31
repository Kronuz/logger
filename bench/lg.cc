// Producer-side cost of the logger, against a synchronous fmt + mutex sink.
//
//   burst:      ns per L_INFO with the LOG thread idle (batches of 1000, drained
//               between batches); this is the producer's own work.
//   steady:     ns per L_INFO while the LOG thread drains concurrently, wall / N.
//   end-to-end: ns per line from the first call until the LOG thread has written
//               the last line (timestamp formatting and handler dispatch included).
//
// Build: cmake -B build -DLOGGER_BUILD_BENCH=ON && cmake --build build --target logger_bench
// Time it under `quiet strict` and compare against a build of the previous commit.
#include "logger.h"

#include <fmt/format.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

static std::atomic<long> lines;
struct Count : Logger {
	void log(int, std::string_view s, bool, bool) override {
		lines.fetch_add(1, std::memory_order_relaxed);
		asm volatile("" ::"r"(s.data()) : "memory");
	}
};

static std::mutex sink_mutex;
static long sunk;
template <class... A>
static void sync_info(fmt::format_string<A...> f, A&&... a) {
	fmt::memory_buffer b;
	fmt::format_to(std::back_inserter(b), f, std::forward<A>(a)...);
	std::lock_guard l(sink_mutex);
	sunk += static_cast<long>(b.size());
	asm volatile("" ::"r"(b.data()) : "memory");
}

static double now_s() {
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static void drain() {
	while (Logging::pending_count() > 0) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}

static void report(const char* name, std::vector<double> s) {
	std::sort(s.begin(), s.end());
	std::printf("%-44s median %8.1f  min %8.1f  spread %.3f ns\n", name, s[s.size() / 2], s[0], s.back() / s[0]);
	std::fflush(stdout);
}

int main() {
	Logging::config.log_level = LOG_INFO;
	Logging::add_handler(std::make_unique<Count>());

	for (int T : {1, 4}) {
		std::printf("--- %d producer thread(s)\n", T);

		{
			std::vector<double> s;
			for (int r = 0; r < 5; ++r) {
				constexpr int kBatch = 1000, kBatches = 30;
				std::atomic<long long> ns{0};
				std::vector<std::thread> ts;
				for (int t = 0; t < T; ++t) {
					ts.emplace_back([&] {
						long long mine = 0;
						for (int b = 0; b < kBatches; ++b) {
							double t0 = now_s();
							for (int i = 0; i < kBatch; ++i) {
								L_INFO("value {} and {}", i, "x");
							}
							mine += static_cast<long long>((now_s() - t0) * 1e9);
							std::this_thread::sleep_for(std::chrono::milliseconds(5));
						}
						ns += mine;
					});
				}
				for (auto& t : ts) {
					t.join();
				}
				drain();
				s.push_back(static_cast<double>(ns) / (kBatch * kBatches * T));
			}
			report("L_INFO burst (LOG thread idle)", s);
		}

		auto steady = [&](const char* name, bool end_to_end) {
			std::vector<double> s;
			constexpr size_t N = 50000;
			for (int r = 0; r < 5; ++r) {
				double t0 = now_s();
				std::vector<std::thread> ts;
				for (int t = 0; t < T; ++t) {
					ts.emplace_back([&] {
						for (size_t k = 0; k < N; ++k) {
							L_INFO("value {} and {}", static_cast<int>(k), "x");
						}
					});
				}
				for (auto& t : ts) {
					t.join();
				}
				double produced = now_s() - t0;
				drain();
				s.push_back((end_to_end ? now_s() - t0 : produced) * 1e9 / (N * T));
			}
			report(name, s);
		};
		steady("L_INFO steady (wall / calls)", false);
		steady("L_INFO end-to-end (produce + drain)", true);

		std::vector<double> s;
		for (int r = 0; r < 5; ++r) {
			constexpr size_t N = 300000;
			double t0 = now_s();
			std::vector<std::thread> ts;
			for (int t = 0; t < T; ++t) {
				ts.emplace_back([&] {
					for (size_t k = 0; k < N; ++k) {
						sync_info("value {} and {}", static_cast<int>(k), "x");
					}
				});
			}
			for (auto& t : ts) {
				t.join();
			}
			s.push_back((now_s() - t0) * 1e9 / (N * T));
		}
		report("comparator: fmt + mutex + counting sink", s);
	}
	Logging::finish();
	Logging::join();
}
