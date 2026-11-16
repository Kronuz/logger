// How soon a routine line is written. With nothing pending, an isolated L_INFO is written at once: the
// LOG thread is woken for it. While a line is pending (here a held watchdog), a routine line joins the
// 1 ms batch: it is due 1 ms after the call and needs no signal, so a burst of such lines wakes the
// LOG thread about once a tick (test/wake_mutant.sh: a burst that wakes it once a line).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <mach/mach.h>       // task_info(TASK_EVENTS_INFO): the task's context switches
#else
#include <sys/resource.h>    // getrusage
#endif

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

// The process's context switches so far (macOS leaves getrusage's counts at zero).
static long switches() {
#if defined(__APPLE__)
	task_events_info_data_t info{};
	mach_msg_type_number_t count = TASK_EVENTS_INFO_COUNT;
	CHECK(task_info(mach_task_self(), TASK_EVENTS_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS);
	return info.csw;
#else
	rusage u{};
	getrusage(RUSAGE_SELF, &u);
	return u.ru_nvcsw;
#endif
}

// The context switches over `n` L_INFO calls `gap` apart (the caller spins between them): each wake
// of the LOG thread costs at least one.
static long burst_switches(int n, std::chrono::nanoseconds gap) {
	std::this_thread::sleep_for(5ms);   // the LOG thread settles into its sleep
	const long before = switches();
	for (int i = 0; i < n; ++i) {
		L_INFO("burst {}", i);
		for (const auto t = clk::now(); clk::now() - t < gap;) {
		}
	}
	return switches() - before;
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

		// A burst of batched lines wakes the LOG thread about once a tick, not once a line.
		const auto woken = burst_switches(400, 10us);
		std::printf("400 batched lines 10 us apart: %ld context switches\n", woken);
		CHECK(woken < 100);
	}   // the watchdog is cancelled: never written

	Logging::finish();
	std::printf("all wake tests passed\n");
	return 0;
}
