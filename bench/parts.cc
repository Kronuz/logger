// Where a log call's producer time goes: each piece of it, alone, in a loop. Single file, no
// dependencies beyond the standard library; build it with the library's flags and time it under
// `quiet strict`:  c++ -std=c++20 -O2 bench/parts.cc -o parts && ./parts
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

template <class T>
static inline void sink(const T& v) {
	asm volatile("" ::"r"(&v) : "memory");
}

static double now_s() {
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

template <class F>
static void run(const char* name, F&& f, long n = 2000000) {
	double best = 1e30;
	for (int rep = 0; rep < 7; ++rep) {
		double t0 = now_s();
		for (long i = 0; i < n; ++i) {
			f(i);
		}
		best = std::min(best, (now_s() - t0) * 1e9 / static_cast<double>(n));
	}
	std::printf("%-46s %8.1f ns\n", name, best);
	std::fflush(stdout);
}

struct Record {
	std::string str;
	long a, b, c, d, e, f, g;
	std::atomic<long> x, y;
	std::string unlog;
	std::shared_ptr<int> hold;
	char pad[40];
};

static thread_local int tls_int;
static std::atomic<long> shared_counter;
static std::atomic<void*> head{nullptr};

#ifdef __APPLE__
#include <mach/mach_time.h>
#include <time.h>
#endif

int main() {
#ifdef __APPLE__
	run("mach_absolute_time", [](long) { auto t = mach_absolute_time(); sink(t); });
	run("mach_continuous_time", [](long) { auto t = mach_continuous_time(); sink(t); });
	run("clock_gettime_nsec_np(CLOCK_UPTIME_RAW)", [](long) { auto t = clock_gettime_nsec_np(CLOCK_UPTIME_RAW); sink(t); });
	run("clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW)", [](long) { auto t = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW); sink(t); });
	run("clock_gettime(CLOCK_MONOTONIC_RAW)", [](long) { timespec ts; clock_gettime(CLOCK_MONOTONIC_RAW, &ts); sink(ts); });
	run("clock_gettime(CLOCK_MONOTONIC)", [](long) { timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); sink(ts); });
	run("clock_gettime(CLOCK_REALTIME)", [](long) { timespec ts; clock_gettime(CLOCK_REALTIME, &ts); sink(ts); });
#if defined(__aarch64__)
	run("mrs cntvct_el0", [](long) { unsigned long v; asm volatile("mrs %0, cntvct_el0" : "=r"(v)); sink(v); });
	run("isb; mrs cntvct_el0", [](long) { unsigned long v; asm volatile("isb; mrs %0, cntvct_el0" : "=r"(v)); sink(v); });
#endif
#endif
	run("steady_clock::now", [](long) { auto t = std::chrono::steady_clock::now(); sink(t); });
	run("system_clock::now", [](long) { auto t = std::chrono::system_clock::now(); sink(t); });
	run("thread_local int ++", [](long) { ++tls_int; sink(tls_int); });
	run("atomic fetch_add relaxed (own line)", [](long) { shared_counter.fetch_add(1, std::memory_order_relaxed); });
	run("atomic CAS seq_cst (own line)", [](long i) {
		void* expected = head.load(std::memory_order_relaxed);
		head.compare_exchange_strong(expected, reinterpret_cast<void*>(static_cast<uintptr_t>(i)), std::memory_order_seq_cst);
	});
	run("seq_cst fence", [](long) { std::atomic_thread_fence(std::memory_order_seq_cst); });

	static std::string scratch;
	scratch.reserve(512);
	int v = 7;
	const char* s = "x";
	run("vformat_to 'value {} and {}' into reused string", [&](long i) {
		scratch.clear();
		int a = static_cast<int>(i);
		std::vformat_to(std::back_inserter(scratch), "value {} and {}", std::make_format_args(a, s));
		sink(scratch);
	});
	run("vformat into fresh string", [&](long i) {
		int a = static_cast<int>(i);
		auto r = std::vformat("value {} and {}", std::make_format_args(a, s));
		sink(r);
	});
	run("vformat_to, one int", [&](long i) {
		scratch.clear();
		int a = static_cast<int>(i);
		std::vformat_to(std::back_inserter(scratch), "value {}", std::make_format_args(a));
		sink(scratch);
	});
	run("vformat_to, no placeholders", [&](long) {
		scratch.clear();
		std::vformat_to(std::back_inserter(scratch), "a plain message with no arguments at all", std::make_format_args());
		sink(scratch);
	});
	run("string::assign 40 bytes into reused string", [&](long) {
		scratch.assign("value 1234567 and x, forty bytes of text..");
		sink(scratch);
	});

	run("malloc+free 240 B", [](long) { void* p = std::malloc(240); sink(p); std::free(p); });
	run("make_shared<Record> (zeroed ~200 B) + drop", [](long) { auto p = std::make_shared<Record>(); sink(p); });
	run("new string of 60 B + free", [](long) { auto* p = new std::string(60, 'x'); sink(p); delete p; });
	{
		auto p = std::make_shared<Record>();
		run("shared_ptr copy + drop", [&](long) { auto q = p; sink(q); });
	}
	run("shared_ptr<int> alloc+copy+drop (box)", [](long) {
		auto p = std::make_shared<int>(1);
		auto* box = new std::shared_ptr<int>(p);
		sink(box);
		delete box;
	});

	// The whole of a producer's path as the library builds it, minus the queue: clocks, a record, a message.
	run("clocks x2 + record + format (no queue)", [&](long i) {
		auto n = std::chrono::steady_clock::now();
		auto sy = std::chrono::system_clock::now();
		scratch.clear();
		int a = static_cast<int>(i);
		std::vformat_to(std::back_inserter(scratch), "value {} and {}", std::make_format_args(a, s));
		auto r = std::make_shared<Record>();
		r->str.assign(scratch);
		sink(n);
		sink(sy);
		sink(r);
	});
	(void)v;
	return 0;
}
