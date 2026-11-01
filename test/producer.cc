// The producer path under load: many threads logging at once while the LOG thread drains.
//
//   1. every line is written exactly once, and each thread's lines come out in the order it logged them;
//   2. an order established between threads (each logs after seeing the previous one log) is the
//      order the lines are written in, for lines of mixed delays too;
//   3. the counters stay exact: nothing pending once drained, drops plus written lines equal logged;
//   4. finish() racing the producers loses no line and writes none twice (run as separate processes,
//      because finish() is final for the LOG thread).
//
// Usage: logger_producer_test          all of the above
//        logger_producer_test race N   one finish() race, seeded by N
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <random>
#include <string>
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

struct Capture : Logger {
	std::mutex m;
	std::vector<std::string> lines;
	std::atomic<long> inline_calls{0};
	void log(int, std::string_view sv, bool, bool) override {
		std::lock_guard<std::mutex> l(m);
		lines.emplace_back(sv);
	}
};

static Capture* capture;

static void install() {
	auto cap = std::make_unique<Capture>();
	capture = cap.get();
	Logging::handlers.clear();
	Logging::handlers.push_back(std::move(cap));
	Logging::config = LogConfig{};
	Logging::config.color = LogColorMode::never;
	Logging::config.log_level = LOG_INFO;
	Logging::config.iterm2 = false;
	Logging::hooks.timestamp = [](std::chrono::system_clock::time_point) { return std::string(); };
}

static bool drain(std::chrono::milliseconds limit = 30s) {
	auto end = std::chrono::steady_clock::now() + limit;
	while (Logging::pending_count() > 0 && std::chrono::steady_clock::now() < end) {
		std::this_thread::sleep_for(1ms);
	}
	return Logging::pending_count() == 0;
}

// "t3:1500" -> thread 3, sequence 1500
static bool parse(const std::string& line, int* t, int* k) {
	auto p = line.rfind('t');
	if (p == std::string::npos) {
		return false;
	}
	return std::sscanf(line.c_str() + p, "t%d:%d", t, k) == 2;
}

static void test_exactly_once_and_thread_order() {
	install();
	constexpr int kThreads = 8, kPer = 40000;
	std::vector<std::thread> ts;
	for (int t = 0; t < kThreads; ++t) {
		ts.emplace_back([t] {
			for (int k = 0; k < kPer; ++k) {
				L_INFO("line t{}:{}", t, k);
			}
		});
	}
	for (auto& th : ts) {
		th.join();
	}
	CHECK(drain());
	std::lock_guard<std::mutex> l(capture->m);
	CHECK(capture->lines.size() == static_cast<size_t>(kThreads) * kPer);
	std::vector<int> next(kThreads, 0);
	for (auto& line : capture->lines) {
		int t, k;
		CHECK(parse(line, &t, &k));
		CHECK(t >= 0 && t < kThreads);
		CHECK(k == next[t]);   // this thread's lines, in the order it logged them, none missing or repeated
		++next[t];
	}
	std::printf("producer exactly-once OK: %d lines from %d threads, each thread's order kept\n", kThreads * kPer, kThreads);
	capture->lines.clear();
}

// Threads take turns; the order of the turns is the order of the lines, for a mix of delays that
// straddle the wheel's 1 ms tick.
static void test_order_across_threads() {
	install();
	constexpr int kThreads = 4, kPer = 600;
	constexpr int N = kThreads * kPer;
	std::atomic<int> turn{0};
	std::vector<std::chrono::steady_clock::time_point> due(N);
	std::vector<char> shown(N, 0);
	std::vector<std::thread> ts;
	for (int t = 0; t < kThreads; ++t) {
		ts.emplace_back([&, t] {
			std::mt19937_64 rng(1000 + t);
			static const int delay_us[] = {0, 0, 0, 100, 300, 500, 950, 1000, 1050, 1500, 2000, 3500};
			for (int k = 0; k < kPer; ++k) {
				int id = k * kThreads + t;
				while (turn.load(std::memory_order_acquire) != id) {
				}
				auto delay = std::chrono::microseconds(delay_us[rng() % (sizeof(delay_us) / sizeof(delay_us[0]))]);
				auto wakeup = std::chrono::steady_clock::now() + delay;
				due[id] = wakeup;
				shown[id] = 1;
				Logging::do_log(false, wakeup, true, LOG_INFO, "line t" + std::to_string(id) + ":" + std::to_string(id));
				turn.store(id + 1, std::memory_order_release);
			}
		});
	}
	for (auto& th : ts) {
		th.join();
	}
	CHECK(drain());
	std::lock_guard<std::mutex> l(capture->m);
	CHECK(capture->lines.size() == static_cast<size_t>(N));
	std::vector<int> position(N, -1);
	for (size_t i = 0; i < capture->lines.size(); ++i) {
		int id, k;
		CHECK(parse(capture->lines[i], &id, &k));
		CHECK(id >= 0 && id < N && position[id] == -1);
		position[id] = static_cast<int>(i);
	}
	long pairs = 0;
	for (int i = 0; i < N; ++i) {
		for (int j = i + 1; j < N; ++j) {
			if (due[i] <= due[j]) {
				++pairs;
				if (position[i] > position[j]) {
					std::fprintf(stderr, "line %d (due %lld) written after line %d (due %lld)\n", i,
						static_cast<long long>(due[i].time_since_epoch().count()), j, static_cast<long long>(due[j].time_since_epoch().count()));
					std::abort();
				}
			}
		}
	}
	std::printf("producer order OK: %ld ordered pairs held across %d threads\n", pairs, kThreads);
	capture->lines.clear();
}

// Backpressure and the counters: drops plus written lines are the lines logged, and nothing is
// pending once drained.
static void test_counters() {
	install();
	CHECK(Logging::pending_count() == 0);
	Logging::config.max_pending = 200;
	constexpr int kThreads = 6, kPer = 30000;
	std::vector<std::thread> ts;
	for (int t = 0; t < kThreads; ++t) {
		ts.emplace_back([t] {
			for (int k = 0; k < kPer; ++k) {
				L_INFO("line t{}:{}", t, k);
			}
		});
	}
	for (auto& th : ts) {
		th.join();
	}
	CHECK(drain());
	Logging::config.max_pending = 0;
	CHECK(Logging::pending_count() == 0);
	std::this_thread::sleep_for(50ms);
	// Lines logged while the wheel held 200 are dropped; the rest are written. The next line to fire
	// reports the drops so far in a summary line, so the drops are those reported plus those still waiting.
	std::lock_guard<std::mutex> l(capture->m);
	long written = 0, summaries = 0, reported = 0;
	for (auto& line : capture->lines) {
		int t, k;
		if (parse(line, &t, &k)) {
			++written;
		} else if (line.find("dropped ") != std::string::npos) {
			++summaries;
			reported += std::atol(line.c_str() + line.find("dropped ") + 8);
		}
	}
	long dropped = reported + Logging::dropped_count();
	CHECK(written + dropped == static_cast<long>(kThreads) * kPer);
	CHECK(written > 0 && dropped > 0);
	std::printf("producer counters OK: %ld written + %ld dropped = %d logged, %ld summaries\n", written, dropped, kThreads * kPer, summaries);
	capture->lines.clear();
}

// One finish() racing producers. Every line is written once: by the LOG thread when it was accepted,
// inline by the caller when it was refused.
static void race_once(unsigned seed) {
	install();
	std::mt19937_64 rng(seed);
	constexpr int kThreads = 4, kPer = 20000;
	std::atomic<bool> go{false};
	std::atomic<long> logged{0};
	std::vector<std::thread> ts;
	for (int t = 0; t < kThreads; ++t) {
		ts.emplace_back([&, t] {
			while (!go.load()) {
			}
			for (int k = 0; k < kPer; ++k) {
				L_INFO("line t{}:{}", t, k);
				logged.fetch_add(1, std::memory_order_relaxed);
			}
		});
	}
	go.store(true);
	std::this_thread::sleep_for(std::chrono::microseconds(100 + rng() % 4000));
	Logging::finish();
	for (auto& th : ts) {
		th.join();
	}
	std::lock_guard<std::mutex> l(capture->m);
	CHECK(logged.load() == static_cast<long>(kThreads) * kPer);
	CHECK(capture->lines.size() == static_cast<size_t>(kThreads) * kPer);
	std::vector<std::vector<char>> seen(kThreads, std::vector<char>(kPer, 0));
	for (auto& line : capture->lines) {
		int t, k;
		CHECK(parse(line, &t, &k));
		CHECK(t >= 0 && t < kThreads && k >= 0 && k < kPer);
		CHECK(!seen[t][k]);
		seen[t][k] = 1;
	}
	CHECK(Logging::pending_count() == 0);
}

int main(int argc, char** argv) {
	if (argc == 3 && std::strcmp(argv[1], "race") == 0) {
		race_once(static_cast<unsigned>(std::atoi(argv[2])));
		return 0;
	}
	test_exactly_once_and_thread_order();
	test_order_across_threads();
	test_counters();
	int rounds = argc == 2 ? std::atoi(argv[1]) : 40;
	for (int i = 0; i < rounds; ++i) {
		std::string cmd = std::string("\"") + argv[0] + "\" race " + std::to_string(i + 1);
		CHECK(std::system(cmd.c_str()) == 0);
	}
	std::printf("producer finish race OK: %d rounds, no line lost or repeated\n", rounds);
	std::printf("all producer tests passed\n");
	return 0;
}
