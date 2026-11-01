// Ordering through the LOG thread, within the wheel's 1 ms tick and across ticks.
//
// Oracle (the contract, not the implementation): for two lines i < j (i logged first)
// whose due times satisfy due_i <= due_j, line i is written before line j. A line due
// later than one logged after it may go either way within the wheel's resolution, so the
// test never asserts on those pairs. Due time is the `wakeup` handed to do_log (now + delay);
// an immediate async line is due now.
//
// The coalescing shift (an async line due within 1 ms is queued 1 ms out) must keep this
// for every kind of line, including `clears` lines, which wait for their handle to be
// dropped to be cancelled.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
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
	std::atomic<int> stall_ms{0};   // the LOG thread sleeps this long in the next write (once)
	void log(int, std::string_view sv, bool, bool) override {
		{
			std::lock_guard l(m);
			lines.emplace_back(sv);
		}
		if (int ms = stall_ms.exchange(0); ms != 0) {
			std::this_thread::sleep_for(std::chrono::milliseconds(ms));
		}
	}
};

static int index_of(const std::string& line) {
	auto p = line.rfind('#');
	return p == std::string::npos ? -1 : std::atoi(line.c_str() + p + 1);
}

int main() {
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

	std::mt19937_64 rng(42);
	constexpr int kRounds = 40, kPerRound = 400;
	long pairs_checked = 0;
	for (int round = 0; round < kRounds; ++round) {
		// Every other round the LOG thread is busy (writing a slow line) while the lines are logged, so
		// it files them overdue, all at once: the order must hold then too.
		const bool stall = round % 2 == 1;
		const size_t extra = stall ? 1 : 0;
		if (stall) {
			c->stall_ms = 20;
			// Due 2 ms out, so it goes through the wheel: the LOG thread walks the wheel again as soon as
			// the write returns, and that walk moves the floor that overdue lines are filed at.
			Logging::do_log(false, std::chrono::steady_clock::now() + 2ms, true, LOG_INFO, "stall");
			for (int i = 0; i < 5000; ++i) {
				{
					std::lock_guard l(c->m);
					if (!c->lines.empty()) {
						break;
					}
				}
				std::this_thread::sleep_for(100us);
			}
			std::this_thread::sleep_for(2ms);   // the LOG thread is inside the write
		}
		std::vector<std::chrono::steady_clock::time_point> due(kPerRound);
		std::vector<Log> handles;   // `clears` lines are cancelled when their handle goes
		handles.reserve(kPerRound);
		for (int i = 0; i < kPerRound; ++i) {
			// Delays that straddle the 1 ms coalescing window and the wheel's tick.
			static const int delay_us[] = {0, 0, 0, 100, 300, 500, 950, 1000, 1050, 1500, 2000, 3500};
			auto delay = std::chrono::microseconds(delay_us[rng() % (sizeof(delay_us) / sizeof(delay_us[0]))]);
			bool clears = rng() % 3 == 0;
			auto wakeup = std::chrono::steady_clock::now() + delay;
			due[i] = wakeup;
			handles.push_back(Logging::do_log(clears, wakeup, true, LOG_INFO, "#" + std::to_string(i)));
			if (rng() % 16 == 0) {
				std::this_thread::sleep_for(std::chrono::microseconds(rng() % 400));
			}
		}
		// (Handles keep their lines counted as pending, so wait for the lines themselves.)
		for (int i = 0; i < 5000; ++i) {
			{
				std::lock_guard l(c->m);
				if (c->lines.size() >= static_cast<size_t>(kPerRound) + extra) {
					break;
				}
			}
			std::this_thread::sleep_for(1ms);
		}
		std::this_thread::sleep_for(20ms);   // a late duplicate would show
		handles.clear();

		std::lock_guard l(c->m);
		CHECK(c->lines.size() == static_cast<size_t>(kPerRound) + extra);
		std::vector<int> position(kPerRound, -1);
		for (size_t k = 0; k < c->lines.size(); ++k) {
			if (stall && k == 0) {
				CHECK(c->lines[0].find("stall") != std::string::npos);
				continue;
			}
			int i = index_of(c->lines[k]);
			CHECK(i >= 0 && i < kPerRound && position[i] == -1);
			position[i] = static_cast<int>(k);
		}
		for (int i = 0; i < kPerRound; ++i) {
			for (int j = i + 1; j < kPerRound; ++j) {
				if (due[i] <= due[j]) {
					++pairs_checked;
					if (position[i] > position[j]) {
						std::fprintf(stderr, "round %d: line %d (due %lld ns) written after line %d (due %lld ns)\n", round, i,
							static_cast<long long>((due[i].time_since_epoch()).count()), j, static_cast<long long>((due[j].time_since_epoch()).count()));
						std::abort();
					}
				}
			}
		}
		c->lines.clear();
	}
	std::printf("logger order OK: %ld ordered pairs held over %d rounds\n", pairs_checked, kRounds);

	// A line logged just before finish() is written: the 1 ms coalescing does not lose it
	// (finish() drains the wheel).
	{
		std::lock_guard l(c->m);
		c->lines.clear();
	}
	for (int i = 0; i < 200; ++i) {
		Logging::do_log(false, std::chrono::steady_clock::now(), true, LOG_INFO, "#" + std::to_string(i));
	}
	Logging::finish();
	{
		std::lock_guard l(c->m);
		CHECK(c->lines.size() == 200);
		for (int i = 0; i < 200; ++i) {
			CHECK(index_of(c->lines[i]) == i);
		}
	}
	std::printf("logger order OK: 200 lines logged immediately before finish() were all written, in order\n");
	return 0;
}
