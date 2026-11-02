// The per-thread queues under load (ARCHITECTURE.md, "The per-thread queues").
//
//   1. exactly once, in each thread's order: 4, 8 and 16 producers mixing routine lines (formatted in
//      place), lines std::vformat formats (copied in), long lines (the queue wraps and moves to bigger
//      buffers) and full records (do_log), while the LOG thread drains;
//   2. order between threads: threads take turns, each logging after it sees the previous line's call
//      return, through the fast path and as full records; the lines come out in turn order;
//   3. the sleep handshake: the LOG thread falls asleep after 50 ms idle, and a line logged then is
//      written at once (a lost wakeup leaves it unwritten: the LOG thread has nothing else to wake for);
//   4. threads that log and end: what they queued is written, their queues are taken by new threads,
//      and a line logged from a thread-local destructor, before or after the thread's queue is
//      released, is written once;
//   5. exit without finish(): a child process logs from several threads and returns from main; every
//      line is in its file;
//   6. finish() racing 16 producers: every line is written once (each round is a process);
//   7. the wrap at the end of a buffer at every offset, each line whole; the order of routine lines and
//      full records due 0 to 3.5 ms out across turn-taking threads; watchdogs armed and cancelled from
//      16 threads; backpressure from 16 producers; swaps on routine lines' handles.
//
// Usage: logger_queue_test             all of the above
//        logger_queue_test exit FILE   (internal) the child of section 5
//        logger_queue_test race N      (internal) one round of section 6
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <unistd.h>

#include "logger.h"

#if defined(__SANITIZE_THREAD__)
#define QUEUE_TEST_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define QUEUE_TEST_TSAN 1
#endif
#endif
#if defined(QUEUE_TEST_TSAN)
static constexpr bool kThreadSanitizer = true;
#else
static constexpr bool kThreadSanitizer = false;
#endif

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
	void log(int, std::string_view sv, bool, bool) override {
		std::lock_guard<std::mutex> l(m);
		lines.emplace_back(sv);
	}
	size_t size() {
		std::lock_guard<std::mutex> l(m);
		return lines.size();
	}
};

static Capture* capture;
static const char* self;

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

static bool drain(std::chrono::milliseconds limit = 60s) {
	auto end = std::chrono::steady_clock::now() + limit;
	while (Logging::pending_count() > 0 && std::chrono::steady_clock::now() < end) {
		std::this_thread::sleep_for(1ms);
	}
	return Logging::pending_count() == 0;
}

// "... t3:1500 ..." -> thread 3, sequence 1500
static bool parse(const std::string& line, int* t, int* k) {
	auto p = line.find(" t");
	if (p == std::string::npos) {
		return false;
	}
	return std::sscanf(line.c_str() + p + 1, "t%d:%d", t, k) == 2;
}

static void test_exactly_once(int threads, int per) {
	install();
	std::vector<std::thread> ts;
	for (int t = 0; t < threads; ++t) {
		ts.emplace_back([t, per] {
			std::mt19937 rng(static_cast<unsigned>(t + 1));
			const std::string big(5000 + 13 * t, 'x');
			for (int k = 0; k < per; ++k) {
				switch (rng() % 16) {
					case 0:
						L_INFO("line t{}:{} {}", t, k, 1.5);   // std::vformat formats it, the queue copies it
						break;
					case 1:
						L_INFO("line t{}:{} {}", t, k, std::string_view(big.data(), rng() % big.size()));   // long
						break;
					case 2:
						Logging::do_log(false, std::chrono::steady_clock::now(), true, LOG_INFO,
							"line t" + std::to_string(t) + ":" + std::to_string(k));   // a full record
						break;
					case 3:
						L_INFO("line t{}:{}", t, k);
						if (rng() % 64 == 0) {
							std::this_thread::sleep_for(std::chrono::microseconds(rng() % 2000));   // let the LOG thread nap
						}
						break;
					default:
						L_INFO("line t{}:{}", t, k);
						break;
				}
			}
		});
	}
	for (auto& th : ts) {
		th.join();
	}
	CHECK(drain());
	std::lock_guard<std::mutex> l(capture->m);
	CHECK(capture->lines.size() == static_cast<size_t>(threads) * static_cast<size_t>(per));
	std::vector<int> next(static_cast<size_t>(threads), 0);
	for (auto& line : capture->lines) {
		int t, k;
		CHECK(parse(line, &t, &k));
		CHECK(t >= 0 && t < threads);
		CHECK(k == next[static_cast<size_t>(t)]);   // this thread's lines, in the order it logged them, none missing or repeated
		++next[static_cast<size_t>(t)];
	}
	std::printf("queue exactly-once OK: %d threads x %d lines, each thread's order kept\n", threads, per);
	capture->lines.clear();
}

// Threads take turns: the order of the turns is the order of the lines. A thread logs once the previous
// line's call has returned (it reads `turn`), and the clock has moved on: two lines stamped in the same
// tick of the clock by two threads can come out in either order (README, "When to use it").
static void test_order_across_threads(int threads, int per) {
	install();
	const int n = threads * per;
	std::atomic<int> turn{0};
	std::vector<std::thread> ts;
	for (int t = 0; t < threads; ++t) {
		ts.emplace_back([&, t] {
			std::mt19937 rng(static_cast<unsigned>(77 + t));
			for (int k = 0; k < per; ++k) {
				const int id = k * threads + t;
				while (turn.load(std::memory_order_acquire) != id) {
					std::this_thread::yield();   // more threads than cores: let the one whose turn it is run
				}
				const auto seen = std::chrono::steady_clock::now();
				while (std::chrono::steady_clock::now() - seen < 200ns) {
				}
				if (rng() % 4 == 0) {
					Logging::do_log(false, std::chrono::steady_clock::now(), true, LOG_INFO,
						"line t" + std::to_string(id) + ":0");
				} else {
					L_INFO("line t{}:0", id);
				}
				turn.store(id + 1, std::memory_order_release);
			}
		});
	}
	for (auto& th : ts) {
		th.join();
	}
	CHECK(drain());
	std::lock_guard<std::mutex> l(capture->m);
	CHECK(capture->lines.size() == static_cast<size_t>(n));
	for (int i = 0; i < n; ++i) {
		int id, k;
		CHECK(parse(capture->lines[static_cast<size_t>(i)], &id, &k));
		if (id != i) {
			std::fprintf(stderr, "position %d holds line %d\n", i, id);
		}
		CHECK(id == i);
	}
	std::printf("queue order across threads OK: %d turns over %d threads\n", n, threads);
	capture->lines.clear();
}

// The LOG thread sleeps once nothing has come for 50 ms; the producer that queues the next line wakes it.
// With no deferred line pending the LOG thread has nothing else to wake for, so a lost wakeup leaves the
// line unwritten past the deadline.
static void test_sleep_handshake(int rounds) {
	install();
	std::mt19937 rng(5);
	std::chrono::nanoseconds worst{0};
	for (int i = 0; i < rounds; ++i) {
		std::this_thread::sleep_for(std::chrono::milliseconds(52 + rng() % 12));
		const auto t0 = std::chrono::steady_clock::now();
		if (i % 2 == 0) {
			L_INFO("wake t{}:0", i);
		} else {
			std::thread([i] { L_INFO("wake t{}:0", i); }).join();   // a new thread: a queue the LOG thread never looked at
		}
		while (capture->size() < static_cast<size_t>(i + 1) && std::chrono::steady_clock::now() - t0 < 5s) {
			std::this_thread::sleep_for(100us);
		}
		CHECK(capture->size() == static_cast<size_t>(i + 1));
		worst = std::max(worst, std::chrono::steady_clock::now() - t0);
	}
	std::printf("queue sleep handshake OK: %d wakeups, slowest %.2f ms\n", rounds,
		std::chrono::duration<double, std::milli>(worst).count());
	capture->lines.clear();
}

static std::atomic<int> tls_dtor_lines{0};
struct LogFromDestructor {
	int id = -1;
	~LogFromDestructor() {
		if (id >= 0) {
			L_INFO("dtor t{}:{}", id, 0);
			Logging::do_log(false, std::chrono::steady_clock::now(), true, LOG_INFO, "dtor t" + std::to_string(id) + ":1");
			tls_dtor_lines.fetch_add(2);
		}
	}
};

static void test_short_lived_threads(int rounds, int threads, int per) {
	install();
	tls_dtor_lines = 0;
	long expected = 0;
	for (int round = 0; round < rounds; ++round) {
		std::vector<std::thread> ts;
		for (int t = 0; t < threads; ++t) {
			const int id = round * threads + t;
			ts.emplace_back([id, per] {
				static thread_local LogFromDestructor early;   // made before the queue: destroyed after its release
				if (id % 3 == 0) {
					early.id = id;
				}
				for (int k = 0; k < per; ++k) {
					L_INFO("line t{}:{}", id, k);
				}
				static thread_local LogFromDestructor late;    // made after the queue: destroyed before its release
				if (id % 3 == 1) {
					late.id = id;
				}
			});
			expected += per;
		}
		for (auto& th : ts) {
			th.join();
		}
		std::this_thread::sleep_for(2ms);   // the LOG thread drains the ended threads' queues and frees them
	}
	expected += tls_dtor_lines.load();
	CHECK(drain());
	std::lock_guard<std::mutex> l(capture->m);
	CHECK(static_cast<long>(capture->lines.size()) == expected);
	std::vector<std::vector<int>> seen(static_cast<size_t>(rounds * threads));
	long dtor = 0;
	for (auto& line : capture->lines) {
		int t, k;
		CHECK(parse(line, &t, &k));
		if (line.compare(0, 4, "dtor") == 0) {
			++dtor;
			continue;
		}
		auto& s = seen[static_cast<size_t>(t)];
		CHECK(static_cast<int>(s.size()) == k);   // in order, once
		s.push_back(k);
	}
	CHECK(dtor == tls_dtor_lines.load());
	// A queue whose thread ended is taken by a later thread once the LOG thread has drained it, so the
	// queues number about the threads alive at once, not the threads that ever logged.
	const auto queues = logger_detail::queue_count();
	std::printf("queue short-lived threads OK: %d threads logged %ld lines (%ld from thread-local destructors) through %zu queues\n",
		rounds * threads, expected, dtor, queues);
	CHECK(queues < static_cast<size_t>(rounds * threads) / 4);
	capture->lines.clear();
}

// The wrap at the end of a queue's buffer writes only a record head, whatever the room left: a whole
// line header there overran the buffer by up to its own size (the heap corruption of the queues' first
// version; ASan reports it at once, a plain build aborts later in malloc). One producer keeps at most
// 16 lines in flight, so its first 16 KiB buffer never grows and wraps every 40 lines or so, and draws
// its line sizes from every multiple of 8 up to 520 bytes, so over a few hundred wraps the wrap falls
// at every offset short of a header. Every line must come out whole.
static void test_wrap_every_offset(int per) {
	install();
	std::string filler(600, 'w');
	std::mt19937 rng(7);
	std::vector<std::size_t> size(static_cast<size_t>(per));
	for (int k = 0; k < per; ++k) {
		while (Logging::pending_count() >= 16) {
			std::this_thread::yield();
		}
		size[static_cast<size_t>(k)] = 8 * (rng() % 65);
		L_INFO("wrap t0:{} {}", k, std::string_view(filler.data(), size[static_cast<size_t>(k)]));
	}
	CHECK(drain());
	std::lock_guard<std::mutex> l(capture->m);
	CHECK(capture->lines.size() == static_cast<size_t>(per));
	for (int k = 0; k < per; ++k) {
		const auto& line = capture->lines[static_cast<size_t>(k)];
		CHECK(line == "wrap t0:" + std::to_string(k) + " " + filler.substr(0, size[static_cast<size_t>(k)]));
	}
	std::printf("queue wrap OK: %d lines of every size class, each whole\n", per);
	capture->lines.clear();
}

// A wrap needs room for the padding and the record: a buffer too full for both moves to a bigger one
// rather than write the record over lines the LOG thread has not read. One producer keeps its buffer
// within about a line of full (lines of about 1 KiB, as many in flight as the buffer holds less one),
// so many wraps find room for the record but not for the padding too. The `{:d}` field sends each
// line through std::vformat, which asks for the record's exact room (the fast formatter asks for
// twice what it had). Every line must come out whole.
static void test_wrap_full(int per) {
	install();
	std::string filler(1200, 'f');
	std::mt19937 rng(11);
	std::vector<std::size_t> size(static_cast<size_t>(per));
	for (int k = 0; k < per; ++k) {
		const auto* r = logger_detail::thread_state.ring;
		const long cap = r != nullptr ? static_cast<long>(r->mask + 1) : 16384;
		const long in_flight = std::max(1L, cap / 1040 - 1);
		while (Logging::pending_count() >= in_flight) {
			std::this_thread::yield();
		}
		size[static_cast<size_t>(k)] = 900 + 8 * (rng() % 32);
		L_INFO("full t0:{:d} {}", k, std::string_view(filler.data(), size[static_cast<size_t>(k)]));
	}
	CHECK(drain());
	std::lock_guard<std::mutex> l(capture->m);
	CHECK(capture->lines.size() == static_cast<size_t>(per));
	for (int k = 0; k < per; ++k) {
		CHECK(capture->lines[static_cast<size_t>(k)] == "full t0:" + std::to_string(k) + " " + filler.substr(0, size[static_cast<size_t>(k)]));
	}
	const auto* r = logger_detail::thread_state.ring;
	std::printf("queue full wrap OK: %d lines of about 1 KiB with the buffer kept full, each whole (buffer now %lu KiB)\n", per,
		r != nullptr ? static_cast<unsigned long>((r->mask + 1) / 1024) : 0UL);
	capture->lines.clear();
}

// Threads take turns, as above, and mix routine lines with full records due 0 to 3.5 ms out: of two
// lines logged in that order whose due times are in that order, the first is written first.
static void test_due_order_mixed(int threads, int per) {
	install();
	const int n = threads * per;
	std::atomic<int> turn{0};
	std::vector<std::chrono::steady_clock::time_point> due(static_cast<size_t>(n));
	std::vector<std::thread> ts;
	for (int t = 0; t < threads; ++t) {
		ts.emplace_back([&, t] {
			std::mt19937 rng(static_cast<unsigned>(500 + t));
			static const int delay_us[] = {0, 0, 0, 0, 100, 300, 950, 1000, 1050, 1500, 2000, 3500};
			for (int k = 0; k < per; ++k) {
				const int id = k * threads + t;
				while (turn.load(std::memory_order_acquire) != id) {
					std::this_thread::yield();   // more threads than cores: let the one whose turn it is run
				}
				const auto seen = std::chrono::steady_clock::now();
				while (std::chrono::steady_clock::now() - seen < 200ns) {
				}
				const int d = delay_us[rng() % (sizeof(delay_us) / sizeof(delay_us[0]))];
				const auto now = std::chrono::steady_clock::now();
				due[static_cast<size_t>(id)] = now + std::chrono::microseconds(d);
				if (d == 0 && rng() % 2 == 0) {
					L_INFO("line t{}:0", id);
				} else {
					Logging::do_log(rng() % 4 == 0, due[static_cast<size_t>(id)], true, LOG_INFO, "line t" + std::to_string(id) + ":0");
				}
				turn.store(id + 1, std::memory_order_release);
			}
		});
	}
	for (auto& th : ts) {
		th.join();
	}
	// `clears` lines were cancelled when their handles dropped at once, unless they were already due:
	// wait for the rest, then check every pair that was written.
	CHECK(drain());
	std::this_thread::sleep_for(10ms);
	std::lock_guard<std::mutex> l(capture->m);
	std::vector<int> position(static_cast<size_t>(n), -1);
	for (size_t i = 0; i < capture->lines.size(); ++i) {
		int id, k;
		CHECK(parse(capture->lines[i], &id, &k));
		CHECK(id >= 0 && id < n && position[static_cast<size_t>(id)] == -1);
		position[static_cast<size_t>(id)] = static_cast<int>(i);
	}
	long pairs = 0;
	for (int i = 0; i < n; ++i) {
		if (position[static_cast<size_t>(i)] < 0) {
			continue;
		}
		for (int j = i + 1; j < n; ++j) {
			if (position[static_cast<size_t>(j)] >= 0 && due[static_cast<size_t>(i)] <= due[static_cast<size_t>(j)]) {
				++pairs;
				if (position[static_cast<size_t>(i)] > position[static_cast<size_t>(j)]) {
					std::fprintf(stderr, "line %d written after line %d, due before it\n", i, j);
					std::abort();
				}
			}
		}
	}
	std::printf("queue due order OK: %ld ordered pairs over %d threads, routine lines and records due 0 to 3.5 ms out\n", pairs, threads);
	capture->lines.clear();
}

// Watchdogs armed from many threads at once: one dropped before it is due stays silent, swap included;
// one held until it is written is written once, after its due time, and its swap follows when it drops.
// (One dropped late, as a slow build may, can go either way, but never half.)
static void test_delayed_cancel(int threads, int per) {
	install();
	enum : char { kSilent, kFires, kEither };
	std::vector<std::vector<char>> kind(static_cast<size_t>(threads), std::vector<char>(static_cast<size_t>(per)));
	std::vector<std::thread> ts;
	for (int t = 0; t < threads; ++t) {
		ts.emplace_back([&, t] {
			std::mt19937 rng(static_cast<unsigned>(900 + t));
			std::vector<std::pair<int, Log>> slow;
			for (int k = 0; k < per; ++k) {
				const auto id = "t" + std::to_string(t) + ":" + std::to_string(k);
				const auto due = std::chrono::steady_clock::now() + std::chrono::milliseconds(3 + rng() % 5);
				auto h = Logging::do_log(true, due, true, LOG_WARNING, "watchdog " + id);
				h.unlog(LOG_NOTICE, "done " + id);
				if (rng() % 3 == 0) {
					slow.emplace_back(k, std::move(h));   // the work is slow: the watchdog fires, then the swap
					kind[static_cast<size_t>(t)][static_cast<size_t>(k)] = kFires;
				} else {
					{
						Log gone = std::move(h);   // the work is done: the handle drops
					}
					kind[static_cast<size_t>(t)][static_cast<size_t>(k)] = std::chrono::steady_clock::now() < due ? kSilent : kEither;
				}
			}
			for (auto& [k, h] : slow) {
				// A record's age is set when the LOG thread fires it: until then, the work is still slow.
				for (int i = 0; i < 60000 && h.age() == 0ns; ++i) {
					std::this_thread::sleep_for(1ms);
				}
				Log gone = std::move(h);
			}
		});
	}
	for (auto& th : ts) {
		th.join();
	}
	CHECK(drain());
	std::lock_guard<std::mutex> l(capture->m);
	std::set<std::string> seen;
	long watchdogs = 0, swaps = 0, slow = 0, late = 0;
	for (auto& written : capture->lines) {
		const auto line = written.substr(0, written.find(" ~"));   // a line fired 100 ms late says by how much
		CHECK(seen.insert(line).second);   // once
		int t, k;
		CHECK(parse(line, &t, &k));
		const char c = kind[static_cast<size_t>(t)][static_cast<size_t>(k)];
		if (line.rfind("watchdog", 0) == 0) {
			CHECK(c != kSilent);
			++watchdogs;
			slow += c == kFires;
			late += c == kEither;
		} else {
			CHECK(line.rfind("done", 0) == 0);
			CHECK(seen.count("watchdog t" + std::to_string(t) + ":" + std::to_string(k)) == 1);   // after its watchdog
			++swaps;
		}
	}
	long expected_slow = 0;
	for (auto& v : kind) {
		expected_slow += std::count(v.begin(), v.end(), kFires);
	}
	CHECK(slow == expected_slow && swaps == watchdogs);
	std::printf("queue delayed/cancel OK: %d threads armed %d watchdogs, %ld held ones fired with their swaps (and %ld dropped late), "
		"those dropped in time silent\n", threads, threads * per, slow, late);
	capture->lines.clear();
}

// Backpressure from 16 producers: drops plus written lines are the lines logged, the drops are reported.
static void test_backpressure(int threads, int per) {
	install();
	Logging::config.max_pending = 100;
	std::vector<std::thread> ts;
	for (int t = 0; t < threads; ++t) {
		ts.emplace_back([t, per] {
			for (int k = 0; k < per; ++k) {
				L_INFO("line t{}:{}", t, k);
			}
		});
	}
	for (auto& th : ts) {
		th.join();
	}
	CHECK(drain());
	Logging::config.max_pending = 0;
	L_INFO("after t0:0");   // reports the drops still waiting
	CHECK(drain());
	std::this_thread::sleep_for(5ms);
	std::lock_guard<std::mutex> l(capture->m);
	long written = 0, reported = 0;
	for (auto& line : capture->lines) {
		if (line.find("dropped ") != std::string::npos) {
			reported += std::atol(line.c_str() + line.find("dropped ") + 8);
		} else if (line.rfind("line", 0) == 0) {
			++written;
		}
	}
	CHECK(written + reported == static_cast<long>(threads) * per);
	CHECK(written > 0 && reported > 0 && Logging::dropped_count() == 0);
	std::printf("queue backpressure OK: %ld written + %ld dropped and reported = %d logged\n", written, reported, threads * per);
	capture->lines.clear();
}

// A routine line's handle swaps in a message, written after the line when the handle drops, from
// many threads at once; clear() on it drops the swap and says the line is on its way.
static void test_light_swap(int threads, int per) {
	install();
	std::vector<std::thread> ts;
	for (int t = 0; t < threads; ++t) {
		ts.emplace_back([t, per] {
			for (int k = 0; k < per; ++k) {
				auto h = L_INFO("first t{}:{}", t, k);
				if (k % 3 == 0) {
					h.L_DELAYED_UNLOG(LOG_NOTICE, "second t{}:{}", t, k);
				} else if (k % 3 == 1) {
					h.L_DELAYED_UNLOG(LOG_NOTICE, "never t{}:{}", t, k);
					CHECK(!h.clear());
				}
			}
		});
	}
	for (auto& th : ts) {
		th.join();
	}
	CHECK(drain());
	std::lock_guard<std::mutex> l(capture->m);
	std::vector<std::vector<int>> first(static_cast<size_t>(threads), std::vector<int>(static_cast<size_t>(per), -1));
	long seconds = 0;
	for (size_t i = 0; i < capture->lines.size(); ++i) {
		const auto& line = capture->lines[i];
		int t, k;
		CHECK(parse(line, &t, &k));
		CHECK(line.rfind("never", 0) != 0);
		if (line.rfind("first", 0) == 0) {
			first[static_cast<size_t>(t)][static_cast<size_t>(k)] = static_cast<int>(i);
		} else {
			CHECK(line.rfind("second", 0) == 0 && k % 3 == 0);
			CHECK(first[static_cast<size_t>(t)][static_cast<size_t>(k)] >= 0);   // after its line
			++seconds;
		}
	}
	CHECK(seconds == static_cast<long>(threads) * ((per + 2) / 3));
	std::printf("queue swaps OK: %d threads, %ld swaps written after their lines, cleared ones silent\n", threads, seconds);
	capture->lines.clear();
}

// The child of the exit test: lines from several threads and from main, then return without finish().
static int exit_child(const char* path) {
	Logging::handlers.clear();
	Logging::add_handler(std::make_unique<StreamLogger>(path));
	Logging::config.color = LogColorMode::never;
	Logging::config.log_level = LOG_INFO;
	Logging::config.iterm2 = false;
	std::vector<std::thread> ts;
	for (int t = 0; t < 4; ++t) {
		ts.emplace_back([t] {
			for (int k = 0; k < 800; ++k) {
				if (k % 10 == 0) {
					Logging::do_log(false, std::chrono::steady_clock::now(), true, LOG_INFO,
						"exit t" + std::to_string(t) + ":" + std::to_string(k));
				} else {
					L_INFO("exit t{}:{}", t, k);
				}
			}
		});
	}
	for (auto& th : ts) {
		th.join();
	}
	for (int k = 0; k < 400; ++k) {
		L_INFO("exit t{}:{}", 4, k);
	}
	return 0;   // no finish(): the LOG thread is stopped at exit and writes what is queued
}

static void test_exit_flush() {
	char path[] = "logger_queue_exit.XXXXXX";   // in the working directory (a shared /tmp can be full)
	int fd = mkstemp(path);
	CHECK(fd >= 0);
	close(fd);
	for (int round = 0; round < 3; ++round) {
		std::remove(path);
		std::string cmd = std::string("\"") + self + "\" exit " + path;
		CHECK(std::system(cmd.c_str()) == 0);
		std::ifstream in(path);
		std::string line;
		std::vector<int> next(5, 0);
		long n = 0;
		while (std::getline(in, line)) {
			int t, k;
			if (!parse(line, &t, &k)) {
				continue;
			}
			CHECK(t >= 0 && t < 5);
			CHECK(k == next[static_cast<size_t>(t)]);
			++next[static_cast<size_t>(t)];
			++n;
		}
		CHECK(n == 4 * 800 + 400);
	}
	std::remove(path);
	std::printf("queue exit flush OK: every line of a process that returned from main without finish()\n");
}

static int race_child(unsigned seed) {
	install();
	std::mt19937_64 rng(seed);
	constexpr int kThreads = 16, kPer = 4000;
	std::atomic<int> ready{0};
	std::atomic<bool> go{false};
	std::vector<std::thread> ts;
	for (int t = 0; t < kThreads; ++t) {
		ts.emplace_back([&, t] {
			ready.fetch_add(1);
			while (!go.load()) {
			}
			for (int k = 0; k < kPer; ++k) {
				if (k % 7 == 0) {
					Logging::do_log(false, std::chrono::steady_clock::now(), true, LOG_INFO,
						"race t" + std::to_string(t) + ":" + std::to_string(k));
				} else {
					L_INFO("race t{}:{}", t, k);
				}
			}
		});
	}
	while (ready.load() != kThreads) {
	}
	go.store(true);
	std::this_thread::sleep_for(std::chrono::microseconds(50 + rng() % 3000));
	Logging::finish();
	for (auto& th : ts) {
		th.join();
	}
	std::lock_guard<std::mutex> l(capture->m);
	CHECK(capture->lines.size() == static_cast<size_t>(kThreads) * kPer);
	std::vector<std::vector<char>> seen(kThreads, std::vector<char>(kPer, 0));
	for (auto& line : capture->lines) {
		int t, k;
		CHECK(parse(line, &t, &k));
		CHECK(t >= 0 && t < kThreads && k >= 0 && k < kPer);
		CHECK(!seen[static_cast<size_t>(t)][static_cast<size_t>(k)]);
		seen[static_cast<size_t>(t)][static_cast<size_t>(k)] = 1;
	}
	CHECK(Logging::pending_count() == 0);
	return 0;
}

int main(int argc, char** argv) {
	std::setvbuf(stdout, nullptr, _IOLBF, 0);   // each test's line as it passes, also into a log file
	self = argv[0];
	if (argc == 3 && std::strcmp(argv[1], "exit") == 0) {
		return exit_child(argv[2]);
	}
	if (argc == 3 && std::strcmp(argv[1], "race") == 0) {
		return race_child(static_cast<unsigned>(std::atoi(argv[2])));
	}
	if (argc == 2 && std::strcmp(argv[1], "wrap") == 0) {
		test_wrap_every_offset(40000);   // alone: the revert checks of the wrap
		test_wrap_full(4000);
		return 0;
	}
	// Under ThreadSanitizer (ten times slower, and more with 16 threads) the quick sizes keep the run
	// within the test's timeout; they still wrap every queue and race every round.
	const bool quick = kThreadSanitizer || (argc == 2 && std::strcmp(argv[1], "quick") == 0);
	test_wrap_every_offset(quick ? 20000 : 40000);
	test_wrap_full(quick ? 2000 : 4000);
	for (int threads : {1, 2, 4, 8, 16}) {
		test_exactly_once(threads, quick ? 2000 : 20000);
	}
	test_order_across_threads(4, quick ? 200 : 1000);
	test_order_across_threads(16, quick ? 50 : 200);
	test_due_order_mixed(4, quick ? 150 : 500);
	test_due_order_mixed(16, quick ? 40 : 120);
	test_delayed_cancel(16, quick ? 100 : 400);
	test_backpressure(16, quick ? 3000 : 20000);
	test_light_swap(8, quick ? 300 : 3000);
	test_sleep_handshake(quick ? 4 : 12);
	test_short_lived_threads(quick ? 10 : 40, 8, 50);
	test_exit_flush();
	const int rounds = quick ? 3 : 12;
	for (int i = 0; i < rounds; ++i) {
		std::string cmd = std::string("\"") + self + "\" race " + std::to_string(i + 1);
		CHECK(std::system(cmd.c_str()) == 0);
	}
	std::printf("queue finish race OK: %d rounds of 16 producers, no line lost or repeated\n", rounds);
	std::printf("all queue tests passed\n");
	return 0;
}
