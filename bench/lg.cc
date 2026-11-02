// What a producer thread pays for one log call, against a synchronous fmt + mutex logger and, built
// with -DLOGGER_BENCH_QUILL=ON, the asynchronous logger quill (with its own background thread).
//
//   burst:        ns per call on each producer, in batches of 1,000 calls with 5 ms between them (an
//                 asynchronous logger's consumer writes a batch in the pause); a producer's own time
//                 per call. The producer spins through the pause: a sleeping one lets the core's clock
//                 drop, and on Apple silicon the next batch then times the clock's ramp, several
//                 times the logger's own cost, for fmt + mutex as much as for the asynchronous loggers.
//   back to back: calls back to back on each producer (while an asynchronous logger's consumer
//                 writes them), wall time divided by all calls (with four producers, their combined
//                 rate).
//   end-to-end:   the same, until the consumer has written the last line.
//
// fmt + mutex, the synchronous logger, runs the first two the same way.
//
// Every logger formats the same message, "value {} and {}" with an int and "x", into a sink that counts
// it and does no I/O. `--check` runs only the same-text check: each logger's sink must receive exactly
// fmt::format's text for 100,000 messages.
//
// Usage: logger_bench [--check] [logger|quill|fmt ...]   (default: every one built in)
// Build: cmake -B build -DCMAKE_BUILD_TYPE=Release -DLOGGER_BUILD_BENCH=ON [-DLOGGER_BENCH_QUILL=ON]
//        && cmake --build build --target logger_bench
// Time it under `quiet strict`. It uses the library's public API only, so it builds against older commits.
#if defined(LOGGER_BENCH_QUILL)
#define QUILL_DISABLE_NON_PREFIXED_MACROS
#include "quill/Backend.h"
#include "quill/Frontend.h"
#include "quill/LogMacros.h"
#include "quill/Logger.h"
#include "quill/sinks/Sink.h"
#endif

#include <fmt/format.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "logger.h"   // last: it defines LOG(), L() and syslog's LOG_* macros

static std::atomic<long> sunk_lines;          // lines a consumer's sink received
static std::vector<std::string>* captured;    // --check: the text each sink received, in order

static void receive(const char* data, std::size_t size) {
	if (captured != nullptr) {
		captured->emplace_back(data, size);
	}
	sunk_lines.fetch_add(1, std::memory_order_relaxed);
	asm volatile("" ::"r"(data) : "memory");
}

struct Count : Logger {
	void log(int, std::string_view s, bool, bool) override { receive(s.data(), s.size()); }
};

#if defined(LOGGER_BENCH_QUILL)
struct QuillCount : quill::Sink {
	void write_log(quill::MacroMetadata const*, uint64_t, std::string_view, std::string_view, std::string const&,
			std::string_view, quill::LogLevel, std::string_view, std::string_view,
			std::vector<std::pair<std::string, std::string>> const*, std::string_view message, std::string_view) override {
		receive(message.data(), message.size());
	}
	void flush_sink() override {}
};
static quill::Logger* quill_logger;
#endif


static std::mutex sink_mutex;
static long sunk_bytes;
static void sync_info(int i) {
	fmt::memory_buffer b;
	fmt::format_to(std::back_inserter(b), "value {} and {}", i, "x");
	std::lock_guard l(sink_mutex);
	sunk_bytes += static_cast<long>(b.size());
	if (captured != nullptr) {
		captured->emplace_back(b.data(), b.size());
	}
	asm volatile("" ::"r"(b.data()) : "memory");
}

static double now_s() {
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static void report(const char* who, const char* what, int producers, std::vector<double> s) {
	std::sort(s.begin(), s.end());
	char name[96];
	std::snprintf(name, sizeof(name), "%s %s, %d producer%s", who, what, producers, producers == 1 ? "" : "s");
	std::printf("%-54s median %8.1f  min %8.1f  spread %.3f ns\n", name, s[s.size() / 2], s[0], s.back() / s[0]);
	std::fflush(stdout);
}

// Batches of 1,000 calls per producer with 5 ms between them (spinning); the consumer drains between batches.
template <class Call, class Drain>
static void burst(const char* who, int producers, Call call, Drain drain) {
	std::vector<double> s;
	for (int r = 0; r < 5; ++r) {
		constexpr int kBatch = 1000, kBatches = 30;
		std::atomic<long long> ns{0};
		std::vector<std::thread> ts;
		for (int t = 0; t < producers; ++t) {
			ts.emplace_back([&] {
				long long mine = 0;
				for (int b = 0; b < kBatches; ++b) {
					double t0 = now_s();
					for (int i = 0; i < kBatch; ++i) {
						call(i);
					}
					mine += static_cast<long long>((now_s() - t0) * 1e9);
					for (double t1 = now_s(); now_s() - t1 < 0.005;) {
					}
				}
				ns += mine;
			});
		}
		for (auto& t : ts) {
			t.join();
		}
		drain(static_cast<long>(kBatch) * kBatches * producers);
		s.push_back(static_cast<double>(ns) / (kBatch * kBatches * producers));
	}
	report(who, "burst", producers, s);
}

// Calls back to back on every producer while the consumer drains: wall time / calls.
template <class Call, class Drain>
static void steady(const char* who, int producers, Call call, Drain drain, bool end_to_end) {
	std::vector<double> s;
	constexpr long N = 50000;
	for (int r = 0; r < 5; ++r) {
		double t0 = now_s();
		std::vector<std::thread> ts;
		for (int t = 0; t < producers; ++t) {
			ts.emplace_back([&] {
				for (long k = 0; k < N; ++k) {
					call(static_cast<int>(k));
				}
			});
		}
		for (auto& t : ts) {
			t.join();
		}
		double produced = now_s() - t0;
		drain(N * producers);
		s.push_back((end_to_end ? now_s() - t0 : produced) * 1e9 / static_cast<double>(N * producers));
	}
	report(who, end_to_end ? "end-to-end" : "back to back", producers, s);
}

// Waits until the sink has received `n` more lines than it had when the round began.
struct SinkDrain {
	long base = 0;
	void begin() { base = sunk_lines.load(); }
	void operator()(long n) {
		while (sunk_lines.load() - base < n) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		base += n;
	}
};

template <class Call, class Drain>
static void time_async(const char* who, Call call, Drain drain) {
	for (int producers : {1, 4}) {
		drain.begin();
		burst(who, producers, call, std::ref(drain));
		steady(who, producers, call, std::ref(drain), false);
		steady(who, producers, call, std::ref(drain), true);
	}
}

// The same-text check: one producer, 100,000 messages, each sink's text against fmt::format's.
template <class Call, class Drain>
static bool check(const char* who, Call call, Drain drain) {
	constexpr int N = 100000;
	std::vector<std::string> got;
	got.reserve(N);
	captured = &got;
	drain.begin();
	for (int i = 0; i < N; ++i) {
		call(i);
	}
	drain(N);
	captured = nullptr;
	long bad = static_cast<long>(got.size() != N);
	for (int i = 0; i < N && i < static_cast<int>(got.size()); ++i) {
		if (got[static_cast<size_t>(i)] != fmt::format("value {} and {}", i, "x")) {
			if (bad++ < 3) {
				std::printf("  %s line %d: \"%s\"\n", who, i, got[static_cast<size_t>(i)].c_str());
			}
		}
	}
	std::printf("same text: %-8s %d messages, %ld different\n", who, N, bad);
	return bad == 0;
}

int main(int argc, char** argv) {
	bool only_check = false;
	std::vector<std::string> which;
	for (int i = 1; i < argc; ++i) {
		if (std::strcmp(argv[i], "--check") == 0) {
			only_check = true;
		} else {
			which.emplace_back(argv[i]);
		}
	}
	auto wanted = [&](const char* w) { return which.empty() || std::find(which.begin(), which.end(), w) != which.end(); };
	bool ok = true;

	if (wanted("logger")) {
		Logging::config.log_level = LOG_INFO;
		Logging::config.with_timestamp = false;   // the sink sees the message alone for the check
		Logging::add_handler(std::make_unique<Count>());
		auto call = [](int i) { L_INFO("value {} and {}", i, "x"); };
		SinkDrain drain;
		ok &= check("logger", call, drain);
		if (!only_check) {
			Logging::config.with_timestamp = true;   // and a timestamped line, as a log has, for the timing
			time_async("L_INFO", call, drain);
		}
		Logging::finish();
		Logging::join();
	}

#if defined(LOGGER_BENCH_QUILL)
	if (wanted("quill")) {
		quill::Backend::start();
		quill_logger = quill::Frontend::create_or_get_logger("bench", quill::Frontend::create_or_get_sink<QuillCount>("count"));
		auto call = [](int i) { QUILL_LOG_INFO(quill_logger, "value {} and {}", i, "x"); };
		SinkDrain drain;
		ok &= check("quill", call, drain);
		if (!only_check) {
			time_async("quill", call, drain);
		}
		quill::Backend::stop();
	}
#endif

	if (wanted("fmt")) {
		std::vector<std::string> got;
		captured = &got;
		for (int i = 0; i < 100000; ++i) {
			sync_info(i);
		}
		captured = nullptr;
		long bad = 0;
		for (int i = 0; i < 100000; ++i) {
			bad += got[static_cast<size_t>(i)] != fmt::format("value {} and {}", i, "x");
		}
		std::printf("same text: %-8s %d messages, %ld different\n", "fmt", 100000, bad);
		ok &= bad == 0;
		if (!only_check) {
			struct NoDrain {
				void operator()(long) {}
			};
			for (int producers : {1, 4}) {
				burst("fmt + mutex", producers, sync_info, NoDrain{});
				steady("fmt + mutex", producers, sync_info, NoDrain{}, false);
			}
		}
	}
	return ok ? 0 : 1;
}
