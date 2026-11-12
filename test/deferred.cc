// Deferred lines on the LOG thread's wheel: each is written once, in order with its peers, never
// before its due time, and within one tick (1 ms, the wheel's slot) plus the LOG thread's timed wake.
//
//   logger_deferred_test           one producer: exactly once, order, never early, lateness bound
//   logger_deferred_test stress    8 producers, cancelled and ring lines mixed in: exactly once,
//                                  none cancelled written, never early, each thread's order
//   logger_deferred_test ties      lines with one due time and one call time: written in filing order
//
// Order oracle (the contract, as test/order.cc): for lines i < j of one thread (i logged first) with
// due_i <= due_j, i is written before j. Due is the `wakeup` handed to do_log.
//
// The wake allowance is measured here, not assumed: the median and p99 of how late a timed
// condition-variable wait returns on this machine now. The lateness bounds are skipped under
// ThreadSanitizer, which slows every thread, and when DEFERRED_LOADED is set (test/deferred_load.sh
// competes with busy loops); the other checks hold there too.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <random>
#include <source_location>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "logger.h"

using namespace std::chrono_literals;
using clk = std::chrono::steady_clock;

#if defined(__SANITIZE_THREAD__)
#define DEFERRED_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define DEFERRED_TSAN 1
#endif
#endif
#ifndef DEFERRED_TSAN
#define DEFERRED_TSAN 0
#endif

#define CHECK(cond) do { \
	if (!(cond)) { \
		std::fprintf(stderr, "CHECK FAILED at line %d: %s\n", __LINE__, #cond); \
		std::abort(); \
	} \
} while (0)

constexpr std::int64_t kTickNs = 1'000'000;   // the wheel's slot

static std::int64_t ns(clk::time_point t) { return t.time_since_epoch().count(); }

// Every written line: its id ("t<thread>#<index>") and when the handler saw it.
struct Capture : Logger {
	std::mutex m;
	std::vector<std::pair<std::string, std::int64_t>> lines;
	void log(int, std::string_view sv, bool, bool) override {
		const auto now = ns(clk::now());
		std::lock_guard l(m);
		lines.emplace_back(std::string(sv), now);
	}
	std::size_t size() {
		std::lock_guard l(m);
		return lines.size();
	}
};

// A line's (thread, index) from its text, or (-1, -1).
static std::pair<int, int> parse(const std::string& s) {
	auto t = s.rfind('t');
	auto h = s.rfind('#');
	if (t == std::string::npos || h == std::string::npos || h < t) {
		return {-1, -1};
	}
	return {std::atoi(s.c_str() + t + 1), std::atoi(s.c_str() + h + 1)};
}

// A line as logged. `cancelled`: its handle's clear() won, so it is never written. `unsure`: clear() lost
// to the LOG thread, which may or may not have written it by then (a user clear() that lands before
// the LOG thread's last look at the line still drops it): written at most once, never early.
struct Line {
	std::int64_t due;
	bool cancelled = false;
	bool unsure = false;
	bool required() const { return !cancelled && !unsure; }   // written exactly once
};

// Cancels `h`, a line logged before its due time, and notes how it went.
static void cancel(Log& h, Line& line) {
	line.cancelled = h.clear();
	line.unsure = !line.cancelled;
}

// How late a timed wait returns: {median, p99} in ns over `n` waits of 1 to 4 ms.
static std::pair<std::int64_t, std::int64_t> timed_wake(int n) {
	std::mutex m;
	std::condition_variable cv;
	std::vector<std::int64_t> late;
	std::mt19937 rng(7);
	std::unique_lock l(m);
	for (int i = 0; i < n; ++i) {
		const auto until = clk::now() + std::chrono::microseconds(1000 + rng() % 3000);
		while (cv.wait_until(l, until) != std::cv_status::timeout) {
		}
		late.push_back(ns(clk::now()) - ns(until));
	}
	std::sort(late.begin(), late.end());
	return {late[late.size() / 2], late[late.size() * 99 / 100]};
}

// Waits until every required line was written (or 10 s), then a little more for any line written twice.
static void await(Capture* c, const std::vector<std::vector<Line>>& logged) {
	const auto end = clk::now() + 10s;
	for (;;) {
		{
			std::lock_guard l(c->m);
			std::vector<std::vector<char>> seen(logged.size());
			for (std::size_t t = 0; t < logged.size(); ++t) {
				seen[t].assign(logged[t].size(), 0);
			}
			for (const auto& line : c->lines) {
				auto [t, i] = parse(line.first);
				if (t >= 0 && static_cast<std::size_t>(t) < logged.size() && i >= 0 && static_cast<std::size_t>(i) < logged[t].size()) {
					seen[t][i] = 1;
				}
			}
			bool all = true;
			for (std::size_t t = 0; t < logged.size() && all; ++t) {
				for (std::size_t i = 0; i < logged[t].size(); ++i) {
					if (logged[t][i].required() && !seen[t][i]) {
						all = false;
						break;
					}
				}
			}
			if (all) {
				break;
			}
		}
		if (clk::now() >= end) {
			break;
		}
		std::this_thread::sleep_for(1ms);
	}
	std::this_thread::sleep_for(30ms);
}

// Checks every thread's lines: once each unless cancelled, never early, in each thread's order.
// Returns each written line's lateness in ns.
static std::vector<std::int64_t> verify(Capture* c, const std::vector<std::vector<Line>>& logged) {
	std::lock_guard l(c->m);
	std::vector<std::vector<int>> seen(logged.size());
	std::vector<std::vector<long>> pos(logged.size());
	for (std::size_t t = 0; t < logged.size(); ++t) {
		seen[t].assign(logged[t].size(), 0);
		pos[t].assign(logged[t].size(), -1);
	}
	std::vector<std::int64_t> late;
	for (std::size_t k = 0; k < c->lines.size(); ++k) {
		auto [t, i] = parse(c->lines[k].first);
		CHECK(t >= 0 && static_cast<std::size_t>(t) < logged.size());
		CHECK(i >= 0 && static_cast<std::size_t>(i) < logged[t].size());
		const Line& line = logged[t][i];
		CHECK(!line.cancelled);                          // a line whose clear() won is never written
		CHECK(++seen[t][i] == 1);                        // nor any line twice
		CHECK(c->lines[k].second >= line.due);           // nor before its due time
		late.push_back(c->lines[k].second - line.due);
		pos[t][i] = static_cast<long>(k);
	}
	long pairs = 0;
	for (std::size_t t = 0; t < logged.size(); ++t) {
		const auto& ls = logged[t];
		for (std::size_t i = 0; i < ls.size(); ++i) {
			CHECK(!ls[i].required() || seen[t][i] == 1);   // every required line once
			if (!ls[i].required()) {
				continue;
			}
			for (std::size_t j = i + 1; j < ls.size(); ++j) {
				if (ls[j].required() && ls[i].due <= ls[j].due) {
					CHECK(pos[t][i] < pos[t][j]);
					++pairs;
				}
			}
		}
	}
	std::printf("checked %zu lines, %ld ordered pairs\n", late.size(), pairs);
	return late;
}

// One producer: lines due 2 to 60 ms out, runs of three sharing one due time, a fifth cancelled
// (a clear() that loses to the LOG thread leaves the line written at most once).
static void single(Capture* c) {
	const auto [wake_med, wake_p99] = timed_wake(200);
	std::printf("timed wake: median %.3f ms, p99 %.3f ms\n", wake_med / 1e6, wake_p99 / 1e6);

	std::mt19937_64 rng(42);
	constexpr int kLines = 600;
	std::vector<std::vector<Line>> logged(1);
	std::vector<Log> kept;
	clk::time_point wakeup;
	for (int i = 0; i < kLines; ++i) {
		if (i % 3 == 0) {   // a new due time; the next two lines share it
			wakeup = clk::now() + std::chrono::microseconds(2000 + rng() % 58000);
		}
		const bool want_cancel = rng() % 5 == 0;
		auto h = Logging::do_log(want_cancel, wakeup, true, LOG_INFO, "t0#" + std::to_string(i));
		Line line{ns(wakeup)};
		if (want_cancel) {
			cancel(h, line);
		} else {
			kept.push_back(std::move(h));
		}
		logged[0].push_back(line);
		if (rng() % 4 == 0) {
			std::this_thread::sleep_for(std::chrono::microseconds(rng() % 1500));
		}
	}
	await(c, logged);
	auto late = verify(c, logged);
	std::size_t expect = 0;
	for (const auto& line : logged[0]) {
		expect += line.required();
	}
	CHECK(late.size() >= expect);
	std::sort(late.begin(), late.end());
	const auto med = late[late.size() / 2], p99 = late[late.size() * 99 / 100], max = late.back();
	std::printf("lateness: min %.3f ms, median %.3f ms, p99 %.3f ms, max %.3f ms\n", late.front() / 1e6,
		med / 1e6, p99 / 1e6, max / 1e6);
	if (!DEFERRED_TSAN && std::getenv("DEFERRED_LOADED") == nullptr) {
		// A line waits for the end of its 1 ms slot, then for the LOG thread's timed wake.
		CHECK(med <= kTickNs + wake_med + 500'000);
		CHECK(p99 <= kTickNs + wake_p99 + 2'000'000);
	}
}

// Eight producers at once, each with deferred, cancelled and ring (due within a tick) lines.
static void stress(Capture* c) {
	constexpr int kThreads = 8, kLines = 3000;
	std::vector<std::vector<Line>> logged(kThreads);
	std::atomic<int> logging{kThreads};
	std::atomic<bool> release{false};   // handles keep their lines pending: kept until all are written
	std::vector<std::thread> threads;
	for (int t = 0; t < kThreads; ++t) {
		threads.emplace_back([&, t] {
			std::mt19937_64 rng(100 + t);
			static const int delay_us[] = {0, 0, 300, 950, 1000, 1500, 2000, 3500, 5000, 8000, 13000, 40000};
			std::vector<Log> kept;
			auto& mine = logged[t];
			mine.reserve(kLines);
			for (int i = 0; i < kLines; ++i) {
				const auto delay = std::chrono::microseconds(delay_us[rng() % std::size(delay_us)] + rng() % 200);
				const bool want_cancel = delay >= 2ms && rng() % 4 == 0;
				const auto wakeup = clk::now() + delay;
				auto h = Logging::do_log(want_cancel, wakeup, true, LOG_INFO,
					"t" + std::to_string(t) + "#" + std::to_string(i));
				Line line{ns(wakeup)};
				if (want_cancel) {
					cancel(h, line);
				} else {
					kept.push_back(std::move(h));
				}
				mine.push_back(line);
				if (rng() % 64 == 0) {
					std::this_thread::sleep_for(std::chrono::microseconds(rng() % 2000));
				}
			}
			logging.fetch_sub(1);
			while (!release.load()) {
				std::this_thread::sleep_for(1ms);
			}
		});
	}
	while (logging.load() != 0) {
		std::this_thread::sleep_for(1ms);
	}
	await(c, logged);
	release = true;
	for (auto& th : threads) {
		th.join();
	}
	verify(c, logged);
}

// `ties` logs lines with a call time of its own choosing: the macros and do_log take it from the clock,
// and no two calls read the same instant.
struct logger_detail::TestAccess {
	template <typename... A>
	static Log add(A&&... a) { return Logging::add(std::forward<A>(a)...); }
};

// Lines that tie on due time and call time are written in the order they were logged: 20 rounds of 200
// lines, each round with one due time and one call time (so the batch is sorted on its last tie-break).
static void ties(Capture* c) {
	constexpr int kRounds = 20, kLines = 200;
	std::vector<std::vector<Line>> logged(1);
	std::vector<Log> kept;
	for (int r = 0; r < kRounds; ++r) {
		const auto now = clk::now();
		const auto wakeup = now + 15ms + std::chrono::milliseconds(r);
		for (int i = 0; i < kLines; ++i) {
			kept.push_back(logger_detail::TestAccess::add(wakeup, "t0#" + std::to_string(logged[0].size()), false, true, true, 0, LOG_INFO,
				nullptr, std::source_location{}, now, std::chrono::system_clock::now(), now));
			logged[0].push_back({ns(wakeup)});
		}
		std::this_thread::sleep_for(1ms);
	}
	await(c, logged);
	verify(c, logged);
}

int main(int argc, char** argv) {
	const std::string mode = argc > 1 ? argv[1] : "";
	const bool stressed = mode == "stress";
	auto cap = std::make_unique<Capture>();
	Capture* c = cap.get();
	Logging::handlers.clear();
	Logging::handlers.push_back(std::move(cap));
	Logging::config.color = LogColorMode::never;
	Logging::config.log_level = LOG_INFO;
	Logging::config.with_threads = false;
	Logging::config.with_location = false;
	Logging::config.tint = false;
	Logging::config.max_pending = 0;
	Logging::hooks.timestamp = [](std::chrono::system_clock::time_point) { return std::string(); };

	if (stressed) {
		stress(c);
	} else if (mode == "ties") {
		ties(c);
	} else {
		single(c);
	}
	Logging::finish();
	std::printf("ok\n");
	return 0;
}
