// The decorated-line timestamp and the producer path.
//
// 1. Differential: logger_detail::default_timestamp against a frozen copy of the
//    original snprintf implementation, over random and edge times, every style,
//    precision and gradient setting, with TZ switched between calls.
// 2. Line text and ordering through the LOG thread are unchanged, including a
//    later-queued earlier-due line waking a sleeping consumer.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "ansi_color.hh"
#include "logger.h"

using namespace std::chrono_literals;

#define CHECK(cond) do { \
	if (!(cond)) { \
		std::fprintf(stderr, "CHECK FAILED at line %d: %s\n", __LINE__, #cond); \
		std::abort(); \
	} \
} while (0)

static const std::string kGrey60 {std::string_view(rgb( 60,  60,  60))};
static const std::string kGrey94 {std::string_view(rgb( 94,  94,  94))};
static const std::string kGrey162{std::string_view(rgb(162, 162, 162))};
static const std::string kGrey230{std::string_view(rgb(230, 230, 230))};

// Frozen copy of the original implementation (localtime_r + snprintf per field).
static std::string
old_timestamp(std::chrono::system_clock::time_point tp)
{
	using namespace std::chrono;
	const auto& cfg = Logging::config;
	if (cfg.timestamp == LogTimestamp::none) {
		return {};
	}
	const bool grad = cfg.timestamp_gradient;
	auto g = [grad](const std::string& col) -> const std::string& {
		static const std::string none;
		return grad ? col : none;
	};
	auto t = system_clock::to_time_t(tp);
	long us = static_cast<long>(duration_cast<microseconds>(tp.time_since_epoch()).count() % 1000000);
	std::tm tm{};
	localtime_r(&t, &tm);
	char b[32];
	std::string s;
	auto frac = [&]() {
		if (cfg.precision == LogPrecision::microseconds) {
			std::snprintf(b, sizeof b, "%06ld", us);
			s += g(kGrey60); s += '.'; s += b;
		} else if (cfg.precision == LogPrecision::milliseconds) {
			std::snprintf(b, sizeof b, "%03ld", us / 1000);
			s += g(kGrey60); s += '.'; s += b;
		}
	};
	if (cfg.timestamp == LogTimestamp::epoch) {
		std::snprintf(b, sizeof b, "%010ld", static_cast<long>(t));
		s += g(kGrey94); s += b;
		frac();
		s += ' ';
		return s;
	}
	if (cfg.timestamp == LogTimestamp::iso8601) {
		std::snprintf(b, sizeof b, "%04d", tm.tm_year + 1900); s += g(kGrey94); s += b;
		s += g(kGrey60); s += '-';
		std::snprintf(b, sizeof b, "%02d", tm.tm_mon + 1);     s += g(kGrey94); s += b;
		s += g(kGrey60); s += '-';
		std::snprintf(b, sizeof b, "%02d", tm.tm_mday);        s += g(kGrey94); s += b;
		s += g(kGrey60); s += ' ';
		std::snprintf(b, sizeof b, "%02d", tm.tm_hour);        s += g(kGrey94); s += b;
		s += g(kGrey60); s += ':';
		std::snprintf(b, sizeof b, "%02d", tm.tm_min);         s += g(kGrey94); s += b;
		s += g(kGrey60); s += ':';
		std::snprintf(b, sizeof b, "%02d", tm.tm_sec);         s += g(kGrey94); s += b;
		frac();
		s += ' ';
		return s;
	}
	std::snprintf(b, sizeof b, "%04d", tm.tm_year + 1900); s += g(kGrey60);  s += b;
	std::snprintf(b, sizeof b, "%02d", tm.tm_mon + 1);     s += g(kGrey94);  s += b;
	std::snprintf(b, sizeof b, "%02d", tm.tm_mday);        s += g(kGrey162); s += b;
	std::snprintf(b, sizeof b, "%02d", tm.tm_hour);        s += g(kGrey230); s += b;
	std::snprintf(b, sizeof b, "%02d", tm.tm_min);         s += g(kGrey162); s += b;
	std::snprintf(b, sizeof b, "%02d", tm.tm_sec);         s += g(kGrey94);  s += b;
	frac();
	s += ' ';
	return s;
}

static void set_tz(const char* tz) {
	if (tz) {
		::setenv("TZ", tz, 1);
	} else {
		::unsetenv("TZ");
	}
	::tzset();
}

static long checked = 0;

static void compare(std::chrono::system_clock::time_point tp) {
	auto a = logger_detail::default_timestamp(tp);
	auto b = old_timestamp(tp);
	if (a != b) {
		std::fprintf(stderr, "MISMATCH tp=%lld us\n new: %s\n old: %s\n",
			static_cast<long long>(tp.time_since_epoch().count()), a.c_str(), b.c_str());
		std::abort();
	}
	++checked;
}

static void differential() {
	const char* zones[] = {nullptr, "UTC", "", "America/New_York", "Europe/London", "Australia/Lord_Howe",
		"Asia/Kolkata", "Pacific/Chatham", "America/Sao_Paulo", "EST5EDT", "Pacific/Kiritimati"};
	std::mt19937_64 rng(12345);
	auto us_pick = [&] { return std::chrono::microseconds(rng() % 1000000); };

	// 2024 DST transitions (UTC seconds) in several hemispheres, and the year edges.
	const long edges[] = {1710054000L, 1730613600L, 1711846800L, 1729990800L, 1727542800L,
		1712455200L, 1728140400L, 1704067199L, 1704067200L, 951782400L, 0L, 2147483647L, 4102444799L};

	for (auto ts : {LogTimestamp::none, LogTimestamp::datetime, LogTimestamp::iso8601, LogTimestamp::epoch}) {
		for (auto pr : {LogPrecision::seconds, LogPrecision::milliseconds, LogPrecision::microseconds}) {
			for (bool grad : {false, true}) {
				Logging::config.timestamp = ts;
				Logging::config.precision = pr;
				Logging::config.timestamp_gradient = grad;
				for (const char* z : zones) {
					set_tz(z);
					for (long e : edges) {
						for (long d = -7200; d <= 7200; d += 1800) {
							auto tp = std::chrono::system_clock::time_point(std::chrono::seconds(e + d) + us_pick());
							compare(tp);
							compare(tp);  // same second again: cache hit
						}
					}
				}
				// Random times, TZ switched between calls (same second under two zones).
				for (int i = 0; i < 1500; ++i) {
					long sec = static_cast<long>(rng() % 4000000000ULL);
					auto tp = std::chrono::system_clock::time_point(std::chrono::seconds(sec) + us_pick());
					set_tz(zones[rng() % std::size(zones)]);
					compare(tp);
					set_tz(zones[rng() % std::size(zones)]);
					compare(tp);
					compare(tp + 1s);
				}
			}
		}
	}

	// Outside the integer path's range: pre-epoch (negative fractions), far future.
	Logging::config.timestamp = LogTimestamp::iso8601;
	Logging::config.precision = LogPrecision::microseconds;
	Logging::config.timestamp_gradient = false;
	set_tz("UTC");
	// Years past 9999 need a system_clock coarser than nanoseconds (libc++).
	std::vector<long long> far_secs{-1LL, -86400LL * 365, -2000000000LL};
	if (std::chrono::system_clock::period::den <= 1000000) {
		far_secs.insert(far_secs.end(), {253402300800LL, 400000000000LL});
	}
	for (long long sec : far_secs) {
		for (long long us : {0LL, 1LL, 999999LL, 123456LL}) {
			compare(std::chrono::system_clock::time_point(std::chrono::microseconds(sec * 1000000 + us)));
			compare(std::chrono::system_clock::time_point(std::chrono::microseconds(sec * 1000000 - us)));
		}
	}
	Logging::config.timestamp = LogTimestamp::epoch;
	std::vector<long long> epoch_secs{-1LL, 9000000000LL};
	if (std::chrono::system_clock::period::den <= 1000000) {
		epoch_secs.insert(epoch_secs.end(), {9999999999LL, 10000000000LL});
	}
	for (long long sec : epoch_secs) {
		compare(std::chrono::system_clock::time_point(std::chrono::seconds(sec)));
	}
	set_tz(nullptr);
	std::printf("differential: %ld comparisons identical\n", checked);
}

// Records lines with the color resets removed (the raw Logger sees them).
struct Capture : Logger {
	std::mutex m;
	std::vector<std::string> lines;
	void log(int, std::string_view sv, bool, bool) override {
		std::string s(sv);
		for (size_t p; (p = s.find("\033[0m")) != std::string::npos;) {
			s.erase(p, 4);
		}
		std::lock_guard l(m);
		lines.emplace_back(std::move(s));
	}
};

static void wait_drained() {
	for (int i = 0; i < 5000 && Logging::pending_count() > 0; ++i) {
		std::this_thread::sleep_for(1ms);
	}
	CHECK(Logging::pending_count() == 0);
	std::this_thread::sleep_for(50ms);
}

static void text_and_order() {
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
	Logging::hooks.timestamp = [](std::chrono::system_clock::time_point) { return std::string("T "); };

	// One producer: every line present, in order, byte-exact.
	for (int i = 0; i < 2000; ++i) {
		L_INFO("line {} of {}", i, 2000);
	}
	wait_drained();
	{
		std::lock_guard l(c->m);
		CHECK(c->lines.size() == 2000);
		for (int i = 0; i < 2000; ++i) {
			auto want = "T " + std::string("line ") + std::to_string(i) + " of 2000";
			if (c->lines[i] != want) {
				std::fprintf(stderr, "line %d: got [%s] want [%s]\n", i, c->lines[i].c_str(), want.c_str());
			}
			CHECK(c->lines[i] == want);
		}
		c->lines.clear();
	}

	// Several producers: nothing lost or duplicated, per-producer order kept.
	constexpr int kThreads = 6, kEach = 1000;
	std::vector<std::thread> ts;
	for (int t = 0; t < kThreads; ++t) {
		ts.emplace_back([t] {
			for (int i = 0; i < kEach; ++i) {
				L_INFO("p{} n{}", t, i);
			}
		});
	}
	for (auto& t : ts) {
		t.join();
	}
	wait_drained();
	{
		std::lock_guard l(c->m);
		CHECK(c->lines.size() == static_cast<size_t>(kThreads * kEach));
		std::vector<int> last(kThreads, -1), seen(kThreads, 0);
		for (auto& ln : c->lines) {
			int t = 0, n = 0;
			CHECK(std::sscanf(ln.c_str(), "T p%d n%d", &t, &n) == 2);
			CHECK(t >= 0 && t < kThreads);
			++seen[t];
			last[t] = std::max(last[t], n);
		}
		for (int t = 0; t < kThreads; ++t) {
			CHECK(seen[t] == kEach && last[t] == kEach - 1);
		}
		c->lines.clear();
	}

	// Deferred lines fire in wake-time order, and an earlier-due line queued
	// after a later one still wakes the sleeping consumer on time.
	{
		auto now = std::chrono::steady_clock::now();
		Logging::do_log(false, now + 3s, true, LOG_INFO, "late");
		std::this_thread::sleep_for(50ms);
		Logging::do_log(false, now + 1500ms, true, LOG_INFO, "mid");
		Logging::do_log(false, std::chrono::steady_clock::now() + 40ms, true, LOG_INFO, "soon");
		for (int i = 0; i < 100; ++i) {
			std::this_thread::sleep_for(10ms);
			std::lock_guard l(c->m);
			if (!c->lines.empty()) {
				break;
			}
		}
		{
			std::lock_guard l(c->m);
			CHECK(c->lines.size() == 1 && c->lines[0] == "T soon");
		}
		for (int i = 0; i < 1000 && Logging::pending_count() > 0; ++i) {
			std::this_thread::sleep_for(10ms);
		}
		std::this_thread::sleep_for(100ms);
		std::lock_guard l(c->m);
		CHECK((c->lines == std::vector<std::string>{"T soon", "T mid", "T late"}));
	}
	Logging::hooks.timestamp = nullptr;
	std::printf("text and order: ok\n");
}

int main(int argc, char** argv) {
	if (argc < 2) {
		differential();
	}
	text_and_order();
	Logging::finish();
	Logging::join();
	return 0;
}
