// logger's three scheduling patterns on (A) logger as it is and (B) a prototype on
// wheel::timer_wheel, one backend per process. docs/perf/wheel-ab.md has the method and the rule.
//
// Built as logger_wheel_ab_c (WAB_ARM_C, against bench/wheel_ab/c's copy of logger whose LOG thread
// files deferred lines in a wheel), its one backend is C, which runs (A)'s calls.
//
// Usage: logger_wheel_ab --backend A|B|C --pattern 1|2|3 --producers P [--formatted] [--check]
//                        [--rounds N] [--bursts N] [--pause-ms N]
// Prints the effective configuration, then one JSON line with the run's metrics. --check also
// verifies every line was written exactly once, none early, and no cleared line; it exits 1 if not.
#include "wheel.hh"

#include <mach/mach.h>
#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "logger.h"   // last: it defines LOG(), L() and syslog's LOG_* macros

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define WAB_TSAN 1
#endif
#endif

using namespace std::chrono_literals;

static inline std::int64_t now_ns() noexcept {
	return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// --- operator new, counted per thread (one uncontended counter each) -----------------------------

namespace alloc_count {
struct alignas(128) Slot {
	std::atomic<std::uint64_t> bytes{0}, count{0};
};
constexpr unsigned kSlots = 256;
inline Slot slots[kSlots];
inline Slot shared;   // past kSlots threads
inline std::atomic<unsigned> used{0};
inline thread_local Slot* mine = nullptr;

inline void note(std::size_t n) noexcept {
	Slot* s = mine;
	if (s == nullptr) {
		const unsigned i = used.fetch_add(1, std::memory_order_relaxed);
		s = mine = i < kSlots ? &slots[i] : &shared;
	}
	if (s == &shared) {
		s->bytes.fetch_add(n, std::memory_order_relaxed);
		s->count.fetch_add(1, std::memory_order_relaxed);
	} else {
		s->bytes.store(s->bytes.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
		s->count.store(s->count.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
	}
}

struct Totals {
	std::uint64_t bytes = 0, count = 0;
};
inline Totals totals() noexcept {
	Totals t;
	const unsigned n = std::min(used.load(std::memory_order_relaxed), kSlots);
	for (unsigned i = 0; i < n; ++i) {
		t.bytes += slots[i].bytes.load(std::memory_order_relaxed);
		t.count += slots[i].count.load(std::memory_order_relaxed);
	}
	t.bytes += shared.bytes.load(std::memory_order_relaxed);
	t.count += shared.count.load(std::memory_order_relaxed);
	return t;
}
}  // namespace alloc_count

#if !defined(WAB_TSAN)
static void* counted_alloc(std::size_t n, std::size_t align) noexcept {
	alloc_count::note(n);
	if (n == 0) {
		n = 1;
	}
	if (align <= alignof(std::max_align_t)) {
		return std::malloc(n);
	}
	void* p = nullptr;
	return posix_memalign(&p, align, n) == 0 ? p : nullptr;
}
static void* counted_or_throw(std::size_t n, std::size_t align) {
	if (void* p = counted_alloc(n, align)) {
		return p;
	}
	throw std::bad_alloc();
}
void* operator new(std::size_t n) { return counted_or_throw(n, 0); }
void* operator new[](std::size_t n) { return counted_or_throw(n, 0); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return counted_alloc(n, 0); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return counted_alloc(n, 0); }
void* operator new(std::size_t n, std::align_val_t a) { return counted_or_throw(n, static_cast<std::size_t>(a)); }
void* operator new[](std::size_t n, std::align_val_t a) { return counted_or_throw(n, static_cast<std::size_t>(a)); }
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { return counted_alloc(n, static_cast<std::size_t>(a)); }
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { return counted_alloc(n, static_cast<std::size_t>(a)); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { std::free(p); }
#endif

// --- configuration and the workload's ids ---------------------------------------------------------

struct Cfg {
	char backend = 'A';
	int pattern = 1;
	unsigned producers = 1;
	bool formatted = false;
	bool check = false;
	int rounds = 8;   // measured rounds, after one warm-up round
	int bursts = 10;
	int burst = 1024;
	int pause_ms = 5;
};
static Cfg cfg;
constexpr int kBlock = 64;                 // calls per timed block
constexpr std::int64_t kSlowDelay = 200'000'000;   // pattern 3: 200 ms
constexpr std::int64_t kTick = 1'000'000;          // the batching tick both backends use

static std::uint64_t mix(std::uint64_t x) noexcept {   // splitmix64
	x += 0x9E3779B97F4A7C15ULL;
	x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
	x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
	return x ^ (x >> 31);
}
static std::uint64_t make_id(unsigned t, std::uint64_t seq) noexcept { return (std::uint64_t{t} << 40) | seq; }
static unsigned id_thread(std::uint64_t id) noexcept { return static_cast<unsigned>(id >> 40); }
static std::uint64_t id_seq(std::uint64_t id) noexcept { return id & ((std::uint64_t{1} << 40) - 1); }
// Pattern 2: uniform in [100 ms, 1 s) by the id; pattern 3: 200 ms.
static std::int64_t delay_of(std::uint64_t id) noexcept {
	switch (cfg.pattern) {
	case 2:
		return 100'000'000 + static_cast<std::int64_t>(mix(id) % 900'000'000);
	case 3:
		return kSlowDelay;
	default:
		return 0;
	}
}
// Pattern 3: the 1% of operations that are slow (their handle is kept until the line is written).
static bool kept(std::uint64_t id) noexcept { return mix(id ^ 0x5BD1E995u) % 100 == 0; }

// --- (B): records, pools and the prototype's LOG thread on the wheel ------------------------------

namespace wab {
struct Pool;
enum : std::uint8_t { kLine = 0, kDeferred = 1, kWatch = 2 };
enum : std::uint8_t { kFiring = 1, kDone = 2, kDropped = 4 };
// One line: the wheel's entry first (so the entry's address is the record's), then the message.
struct alignas(64) Rec {
	wheel::timer_entry e;
	Pool* pool = nullptr;
	Rec* free_next = nullptr;
	std::int64_t created = 0;
	std::atomic<std::uint8_t> state{0};
	std::uint8_t kind = kLine;
	std::int16_t priority = 0;
	std::uint32_t len = 0;
	char msg[60];
};
static_assert(sizeof(Rec) == 128);

// A thread's records: its own free list, and a stack the LOG thread returns records to.
struct alignas(128) Pool {
	Rec* local = nullptr;
	std::int64_t last_soon = std::numeric_limits<std::int64_t>::min() / 2;
	std::vector<std::unique_ptr<Rec[]>> chunks;
	alignas(128) std::atomic<Rec*> returned{nullptr};
	alignas(128) std::atomic<long> posted{0};   // only the owner writes it

	Rec* get() {
		if (local == nullptr) {
			local = returned.exchange(nullptr, std::memory_order_acquire);
			if (local == nullptr) {
				grow();
			}
		}
		Rec* r = local;
		local = r->free_next;
		return r;
	}
	void put_local(Rec* r) noexcept {
		r->free_next = local;
		local = r;
	}
	void put_remote(Rec* r) noexcept {
		Rec* h = returned.load(std::memory_order_relaxed);
		do {
			r->free_next = h;
		} while (!returned.compare_exchange_weak(h, r, std::memory_order_release, std::memory_order_relaxed));
	}
	void grow() {
		constexpr std::size_t n = 256;
		auto c = std::make_unique<Rec[]>(n);
		for (std::size_t i = 0; i < n; ++i) {
			c[i].pool = this;
			c[i].e.fire = [](wheel::timer_entry&, bool) noexcept {};
			c[i].free_next = i + 1 < n ? &c[i + 1] : local;
		}
		local = &c[0];
		chunks.push_back(std::move(c));
	}
};

inline constexpr unsigned kSleeping = 1, kStop = 2;

class WheelLog {
public:
	wheel::timer_wheel* w;
	alignas(128) std::atomic<unsigned> state{0};
	alignas(128) std::atomic<std::int64_t> wake_at{0};
	alignas(128) std::atomic<long> completed{0};

	WheelLog() : w(new wheel::timer_wheel(static_cast<std::uint64_t>(now_ns()))) {}

	void start() { th = std::thread([this] { loop(); }); }
	void stop() {
		state.fetch_or(kStop, std::memory_order_seq_cst);
		{
			std::lock_guard<std::mutex> lk(mtx);
			cv.notify_one();
		}
		th.join();
	}
	// Wakes the LOG thread if it sleeps past `due`.
	void wake_for(std::int64_t due) noexcept {
		if (due < wake_at.load(std::memory_order_relaxed)
				&& (state.fetch_and(~kSleeping, std::memory_order_seq_cst) & kSleeping) != 0) {
			std::lock_guard<std::mutex> lk(mtx);
			cv.notify_one();
		}
	}

private:
	std::thread th;
	std::mutex mtx;
	std::condition_variable cv;

	static void fire(Rec& r) {
		if (r.kind == kWatch) {
			if ((r.state.fetch_or(kFiring, std::memory_order_acq_rel) & kDropped) != 0) {
				r.pool->put_remote(&r);   // cleared: discarded
				return;
			}
			write(r);
			if ((r.state.fetch_or(kDone, std::memory_order_acq_rel) & kDropped) != 0) {
				r.pool->put_remote(&r);
			}
			return;
		}
		write(r);
		r.pool->put_remote(&r);
	}
	// What logger's emit does for a line with no decoration: the message, and a record's age when
	// it is past 100 ms, then the fan-out to the sinks.
	static void write(const Rec& r) {
		std::string m(r.msg, r.len);
		if (r.kind != kLine) {
			if (const std::int64_t age = now_ns() - r.created; age > 100'000'000) {
				m += " ~";
				m += std::to_string(age / 1'000'000);
				m += "ms";
			}
		}
		Logging::log(r.priority, std::move(m));
	}

	void loop() {
		for (;;) {
			long fired = static_cast<long>(w->expire(static_cast<std::uint64_t>(now_ns()),
				[](wheel::timer_entry& e) { fire(*reinterpret_cast<Rec*>(&e)); }));
			if (fired != 0) {
				completed.fetch_add(fired, std::memory_order_release);
				continue;
			}
			const std::uint64_t nx = w->next();
			if (nx == 0) {
				continue;   // other threads' arms wait to be filed
			}
			if ((state.load(std::memory_order_relaxed) & kStop) != 0 && w->size() == 0) {
				break;
			}
			// Nothing due: sleep until next(). A producer that arms one due earlier sees kSleeping
			// (or this second look sees its arm) and wakes it.
			std::unique_lock<std::mutex> lk(mtx);
			const std::int64_t until = nx == wheel::timer_wheel::never ? std::numeric_limits<std::int64_t>::max()
				: static_cast<std::int64_t>(nx);
			wake_at.store(until, std::memory_order_relaxed);
			state.fetch_or(kSleeping, std::memory_order_seq_cst);
			std::atomic_thread_fence(std::memory_order_seq_cst);
			if (w->next() >= nx) {
				auto awake = [&] { return (state.load(std::memory_order_seq_cst) & (kSleeping | kStop)) != kSleeping; };
				if (until == std::numeric_limits<std::int64_t>::max()) {
					cv.wait(lk, awake);
				} else {
					cv.wait_until(lk, std::chrono::steady_clock::time_point(std::chrono::nanoseconds(until)), awake);
				}
			}
			state.fetch_and(~kSleeping, std::memory_order_seq_cst);
		}
		delete w;
		w = nullptr;
	}
};
}  // namespace wab

// --- producers and the sink -----------------------------------------------------------------------

struct alignas(128) Producer {
	unsigned index = 0;
	std::uint64_t seq = 0;
	std::unique_ptr<std::atomic<std::int64_t>[]> block_start;   // per block of 64 calls
	std::vector<double> samples;                                // ns per call, per block, measured rounds
	std::vector<Log> keep_a;                                    // pattern 3, the slow 1%
	std::vector<wab::Rec*> keep_b;
	std::atomic<long> kept_lines{0};   // atomic: TSan does not see libc++'s barrier on macOS as a sync
	wab::Pool pool;
	char msg[16];
};

static std::vector<std::unique_ptr<Producer>> producers;
static std::uint64_t per_producer = 0;   // ids each producer logs in the run
static wab::WheelLog* wlog = nullptr;

// Check mode: per id, the times it was written and the clock before its call.
static std::unique_ptr<std::atomic<std::uint8_t>[]> hits;
// Relaxed atomics: `post` is stored after the call has published the line; the sink skips a 0.
static std::unique_ptr<std::atomic<std::int64_t>[]> pre;
static std::unique_ptr<std::atomic<std::int64_t>[]> post;   // the clock after its call: its due lies in [pre, post] + delay

struct SinkState {
	alignas(128) std::atomic<long> sunk{0};
	alignas(128) std::atomic<bool> measuring{false};
	std::atomic<mach_port_t> port{0};
	pthread_t tid{};
	std::vector<std::int64_t> lags;   // ns, written lines of measured rounds
	long early = 0, inversions = 0, due_inversions = 0, bad_id = 0, other_thread = 0;
	std::vector<std::int64_t> last_seq;
	std::int64_t max_due_lo = std::numeric_limits<std::int64_t>::min();   // the latest due lower bound written
};
static SinkState sink;

static void on_line(std::string_view s) {
	const std::int64_t t = now_ns();
	if (sink.port.load(std::memory_order_relaxed) == 0) {
		sink.tid = pthread_self();
		sink.port.store(mach_thread_self(), std::memory_order_release);
	} else if (pthread_equal(sink.tid, pthread_self()) == 0) {
		++sink.other_thread;
	}
	if (!cfg.formatted) {
		std::uint64_t id = ~std::uint64_t{0};
		if (s.size() >= 8) {
			std::memcpy(&id, s.data(), 8);
		}
		const unsigned th = id_thread(id);
		const std::uint64_t seq = id_seq(id);
		if (th < cfg.producers && seq < per_producer) {
			const std::int64_t deadline = producers[th]->block_start[seq / kBlock].load(std::memory_order_relaxed) + delay_of(id);
			if (sink.measuring.load(std::memory_order_relaxed) && sink.lags.size() < sink.lags.capacity()) {
				sink.lags.push_back(t - deadline);
			}
			if (cfg.check) {
				const std::uint64_t i = th * per_producer + seq;
				hits[i].store(static_cast<std::uint8_t>(hits[i].load(std::memory_order_relaxed) + 1), std::memory_order_relaxed);
				const std::int64_t lo = pre[i].load(std::memory_order_relaxed);
				if (t < lo + delay_of(id)) {
					++sink.early;
				}
				if (cfg.pattern == 1) {
					if (static_cast<std::int64_t>(seq) < sink.last_seq[th]) {
						++sink.inversions;
					} else {
						sink.last_seq[th] = static_cast<std::int64_t>(seq);
					}
				} else {
					// Due order: a line whose due is surely earlier than one written before it.
					const std::int64_t hi = post[i].load(std::memory_order_relaxed);
					if (hi != 0 && hi + delay_of(id) < sink.max_due_lo) {
						++sink.due_inversions;
					}
					sink.max_due_lo = std::max(sink.max_due_lo, lo + delay_of(id));
				}
			}
		} else {
			++sink.bad_id;
		}
	}
	sink.sunk.store(sink.sunk.load(std::memory_order_relaxed) + 1, std::memory_order_release);
}

struct Sink : Logger {
	void log(int, std::string_view s, bool, bool) override { on_line(s); }
};

// --- the calls --------------------------------------------------------------------------------------

template <int Pattern, bool Formatted>
static inline void call_a(Producer& p, std::uint64_t id) {
	const std::string_view msg(p.msg, sizeof(p.msg));
	if constexpr (Pattern == 1) {
		if constexpr (Formatted) {
			L_INFO("value {} and {}", static_cast<int>(id_seq(id)), "x");
		} else {
			L_INFO(msg);
		}
	} else if constexpr (Pattern == 2) {
		L_DELAYED(false, std::chrono::nanoseconds(delay_of(id)), LOG_INFO, msg);
	} else {
		Log h = L_DELAYED(true, std::chrono::nanoseconds(kSlowDelay), LOG_WARNING, msg);
		if (kept(id)) {
			p.keep_a.push_back(std::move(h));
			p.kept_lines.store(p.kept_lines.load(std::memory_order_relaxed) + 1, std::memory_order_release);
		}
	}
}

static long pending_b() noexcept {
	const long done = wlog->completed.load(std::memory_order_acquire);
	long n = 0;
	for (auto& p : producers) {
		n += p->pool.posted.load(std::memory_order_relaxed);
	}
	return n - done;
}

static inline void drop_b(Producer& p, wab::Rec* r) noexcept {
	if ((r->state.fetch_or(wab::kDropped, std::memory_order_acq_rel) & wab::kDone) != 0) {
		p.pool.put_local(r);   // the line was written: this was the last hold
	}
}

template <int Pattern, bool Formatted>
static inline void call_b(Producer& p, std::uint64_t id) {
	wab::Rec* r = p.pool.get();
	const std::int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(logger_detail::steady_now().time_since_epoch()).count();
	std::int64_t due;
	if constexpr (Pattern == 1) {
		if constexpr (Formatted) {
			logger_detail::FastOut o{r->msg, r->msg + sizeof(r->msg)};
			(void)logger_detail::fast_format(o, "value {} and {}", static_cast<int>(id_seq(id)), "x");
			r->len = static_cast<std::uint32_t>(o.p - r->msg);
		} else {
			std::memcpy(r->msg, p.msg, sizeof(p.msg));
			r->len = sizeof(p.msg);
		}
		r->kind = wab::kLine;
		r->priority = LOG_INFO;
		// logger's rule: due at the call when it starts a burst and nothing is pending, else a tick later.
		due = now - p.pool.last_soon >= kTick && pending_b() == 0 ? now : now + kTick;
		p.pool.last_soon = now;
	} else {
		std::memcpy(r->msg, p.msg, sizeof(p.msg));
		r->len = sizeof(p.msg);
		r->kind = Pattern == 2 ? wab::kDeferred : wab::kWatch;
		r->priority = Pattern == 2 ? LOG_INFO : LOG_WARNING;
		r->state.store(0, std::memory_order_relaxed);
		due = now + delay_of(id);
	}
	r->created = now;
	p.pool.posted.store(p.pool.posted.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
	wlog->w->arm_remote(r->e, static_cast<std::uint64_t>(due));
	// Pairs with the LOG thread's kSleeping and second look: either it sees the arm or this sees it sleeping.
	std::atomic_thread_fence(std::memory_order_seq_cst);
	if (wlog->state.load(std::memory_order_seq_cst) != 0) [[unlikely]] {
		wlog->wake_for(due);
	}
	if constexpr (Pattern == 3) {
		if (kept(id)) {
			p.keep_b.push_back(r);
			p.kept_lines.store(p.kept_lines.load(std::memory_order_relaxed) + 1, std::memory_order_release);
		} else {
			drop_b(p, r);
		}
	}
}

// --- the run ----------------------------------------------------------------------------------------

static std::barrier<>* bar = nullptr;

template <char Backend, int Pattern, bool Formatted>
static void produce(Producer& p) {
	std::memcpy(p.msg + 8, "slow-op!", 8);
	for (int round = 0; round <= cfg.rounds; ++round) {
		bar->arrive_and_wait();   // start
		const bool measured = round > 0;
		for (int b = 0; b < cfg.bursts; ++b) {
			if (b != 0) {
				for (const std::int64_t t = now_ns(); now_ns() - t < cfg.pause_ms * 1'000'000LL;) {
				}
			}
			for (int k = 0; k < cfg.burst; k += kBlock) {
				const std::int64_t t0 = now_ns();
				p.block_start[p.seq / kBlock].store(t0, std::memory_order_relaxed);
				for (int j = 0; j < kBlock; ++j) {
					const std::uint64_t id = make_id(p.index, p.seq);
					std::memcpy(p.msg, &id, 8);
					if (cfg.check) {
						pre[p.index * per_producer + p.seq].store(now_ns(), std::memory_order_relaxed);
					}
					if constexpr (Backend == 'A') {
						call_a<Pattern, Formatted>(p, id);
					} else {
						call_b<Pattern, Formatted>(p, id);
					}
					if (cfg.check) {
						post[p.index * per_producer + p.seq].store(now_ns(), std::memory_order_relaxed);
					}
					++p.seq;
				}
				const std::int64_t t1 = now_ns();
				if (measured) {
					p.samples.push_back(static_cast<double>(t1 - t0) / kBlock);
				}
			}
		}
		bar->arrive_and_wait();   // produced
		bar->arrive_and_wait();   // drained
		p.keep_a.clear();
		for (wab::Rec* r : p.keep_b) {
			drop_b(p, r);
		}
		p.keep_b.clear();
		bar->arrive_and_wait();   // dropped
	}
}

template <char Backend>
static void start_producers(std::vector<std::thread>& ts) {
	for (auto& up : producers) {
		Producer* p = up.get();
		auto body = [p] {
			if (cfg.pattern == 1 && cfg.formatted) {
				produce<Backend, 1, true>(*p);
			} else if (cfg.pattern == 1) {
				produce<Backend, 1, false>(*p);
			} else if (cfg.pattern == 2) {
				produce<Backend, 2, false>(*p);
			} else {
				produce<Backend, 3, false>(*p);
			}
		};
		ts.emplace_back(body);
	}
}

static double thread_cpu_ns(mach_port_t port) {
	thread_basic_info_data_t info;
	mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
	if (port == 0 || thread_info(port, THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&info), &n) != KERN_SUCCESS) {
		return -1;
	}
	return (static_cast<double>(info.user_time.seconds) + static_cast<double>(info.system_time.seconds)) * 1e9
		+ (static_cast<double>(info.user_time.microseconds) + static_cast<double>(info.system_time.microseconds)) * 1e3;
}

static double pct(std::vector<double>& v, double q) {
	if (v.empty()) {
		return 0;
	}
	const auto k = static_cast<std::size_t>(q * static_cast<double>(v.size() - 1));
	std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
	return v[k];
}

static void usage() {
	std::fprintf(stderr, "usage: logger_wheel_ab --backend A|B --pattern 1|2|3 --producers P [--formatted] [--check]"
		" [--rounds N] [--bursts N] [--pause-ms N]\n");
	std::exit(2);
}

int main(int argc, char** argv) {
	bool rounds_set = false;
	for (int i = 1; i < argc; ++i) {
		const std::string_view a = argv[i];
		auto value = [&]() -> const char* {
			if (i + 1 >= argc) {
				usage();
			}
			return argv[++i];
		};
		if (a == "--backend") {
			cfg.backend = value()[0];
		} else if (a == "--pattern") {
			cfg.pattern = std::atoi(value());
		} else if (a == "--producers") {
			cfg.producers = static_cast<unsigned>(std::atoi(value()));
		} else if (a == "--formatted") {
			cfg.formatted = true;
		} else if (a == "--check") {
			cfg.check = true;
		} else if (a == "--rounds") {
			cfg.rounds = std::atoi(value());
			rounds_set = true;
		} else if (a == "--bursts") {
			cfg.bursts = std::atoi(value());
		} else if (a == "--pause-ms") {
			cfg.pause_ms = std::atoi(value());
		} else {
			usage();
		}
	}
#if defined(WAB_ARM_C)
	const bool backend_ok = cfg.backend == 'C';
#else
	const bool backend_ok = cfg.backend == 'A' || cfg.backend == 'B';
#endif
	if (!backend_ok || cfg.pattern < 1 || cfg.pattern > 3 || cfg.producers < 1
			|| cfg.producers > 64 || (cfg.formatted && (cfg.pattern != 1 || cfg.check)) || cfg.burst % kBlock != 0) {
		usage();
	}
	if (!rounds_set) {
		cfg.rounds = cfg.check ? 2 : cfg.pattern == 2 ? 3 : 8;
	}
	const std::int64_t t_start = now_ns();

	Logging::config.log_level = LOG_INFO;
	Logging::config.with_timestamp = false;
	Logging::add_handler(std::make_unique<Sink>());

	const unsigned P = cfg.producers;
	const std::uint64_t per_round = static_cast<std::uint64_t>(cfg.bursts) * static_cast<std::uint64_t>(cfg.burst);
	per_producer = per_round * static_cast<std::uint64_t>(cfg.rounds + 1);
	for (unsigned t = 0; t < P; ++t) {
		auto p = std::make_unique<Producer>();
		p->index = t;
		p->block_start = std::make_unique<std::atomic<std::int64_t>[]>(per_producer / kBlock);
		p->samples.reserve(per_round / kBlock * static_cast<std::uint64_t>(cfg.rounds));
		p->keep_a.reserve(per_round / 20);
		p->keep_b.reserve(per_round / 20);
		producers.push_back(std::move(p));
	}
	const std::uint64_t measured_calls = per_round * static_cast<std::uint64_t>(cfg.rounds) * P;
	sink.lags.reserve(measured_calls);
	sink.last_seq.assign(P, -1);
	if (cfg.check) {
		hits = std::make_unique<std::atomic<std::uint8_t>[]>(per_producer * P);
		pre = std::make_unique<std::atomic<std::int64_t>[]>(per_producer * P);
		post = std::make_unique<std::atomic<std::int64_t>[]>(per_producer * P);
	}
	std::printf("config backend=%c pattern=%d producers=%u formatted=%d check=%d rounds=%d bursts=%d burst=%d pause_ms=%d"
		" block=%d log_level=%d with_timestamp=%d wheel=50665c8 sizeof_rec=%zu sizeof_logging=%zu\n",
		cfg.backend, cfg.pattern, P, cfg.formatted, cfg.check, cfg.rounds, cfg.bursts, cfg.burst, cfg.pause_ms, kBlock,
		Logging::config.log_level, Logging::config.with_timestamp, sizeof(wab::Rec), sizeof(Logging));
	std::fflush(stdout);

	if (cfg.backend == 'B') {
		wlog = new wab::WheelLog;
		wlog->start();
	}
	std::barrier<> b(static_cast<std::ptrdiff_t>(P + 1));
	bar = &b;
	std::vector<std::thread> ts;
	if (cfg.backend == 'B') {
		start_producers<'B'>(ts);
	} else {
		start_producers<'A'>(ts);   // (C) runs (A)'s calls against its copy of logger
	}
	auto pending = [] { return cfg.backend == 'B' ? pending_b() : Logging::pending_count(); };
	// Lines the scheduler holds over the measured rounds (filed, cancelled or not), every 1 ms: the
	// peak, and the integral (line-milliseconds) per call, i.e. how long a call's line stays filed.
	std::atomic<bool> sampling{true};
	double held_ms = 0;
	long held_peak = 0;
	std::thread sampler([&] {
		std::int64_t last = now_ns();
		while (sampling.load(std::memory_order_relaxed)) {
			std::this_thread::sleep_for(1ms);
			const std::int64_t t = now_ns();
			if (sink.measuring.load(std::memory_order_relaxed)) {
				const long n = pending();
				held_ms += static_cast<double>(n) * static_cast<double>(t - last) / 1e6;
				held_peak = std::max(held_peak, n);
			}
			last = t;
		}
	});

	long expected = 0;
	alloc_count::Totals heap0;
	double cpu0 = 0;
	std::int64_t t_timed = 0;
	bool ok = true;
	for (int round = 0; round <= cfg.rounds; ++round) {
		if (round == 1) {
			heap0 = alloc_count::totals();
			cpu0 = thread_cpu_ns(sink.port.load(std::memory_order_acquire));
			t_timed = now_ns();
			sink.measuring.store(true, std::memory_order_relaxed);
		}
		b.arrive_and_wait();   // start
		b.arrive_and_wait();   // produced
		// (A) counts a record as done when it is destroyed, so the slow 1% whose handles are still held
		// stay pending until the drop below.
		long held = 0;
		if (cfg.pattern == 3) {
			for (auto& p : producers) {
				held += p->kept_lines.exchange(0, std::memory_order_acquire);
			}
			expected += held;
			held = cfg.backend == 'B' ? 0 : held;
		} else {
			expected += static_cast<long>(per_round * P);
		}
		const std::int64_t deadline = now_ns() + 30'000'000'000LL;
		while (pending() > held || sink.sunk.load(std::memory_order_acquire) < expected) {
			if (now_ns() > deadline) {
				std::printf("FAIL round %d did not drain: pending %ld, written %ld of %ld\n", round, pending(),
					sink.sunk.load(), expected);
				std::fflush(stdout);
				std::_Exit(1);
			}
			std::this_thread::sleep_for(1ms);
		}
		b.arrive_and_wait();   // drained
		b.arrive_and_wait();   // dropped
	}
	const std::int64_t t_end = now_ns();
	const alloc_count::Totals heap1 = alloc_count::totals();
	const double cpu1 = thread_cpu_ns(sink.port.load(std::memory_order_acquire));
	sink.measuring.store(false, std::memory_order_relaxed);
	sampling.store(false, std::memory_order_relaxed);
	sampler.join();
	for (auto& t : ts) {
		t.join();
	}
	if (sink.sunk.load() != expected) {
		std::printf("FAIL written %ld, expected %ld\n", sink.sunk.load(), expected);
		ok = false;
	}

	if (cfg.check) {
		long missing = 0, twice = 0, cleared_written = 0;
		for (unsigned t = 0; t < P; ++t) {
			for (std::uint64_t s = 0; s < per_producer; ++s) {
				const auto n = hits[t * per_producer + s].load(std::memory_order_relaxed);
				const bool want = cfg.pattern != 3 || kept(make_id(t, s));
				if (want && n == 0) {
					++missing;
				} else if (n > 1) {
					++twice;
				} else if (!want && n != 0) {
					++cleared_written;
				}
			}
		}
		std::printf("check backend=%c pattern=%d producers=%u lines=%llu written=%ld missing=%ld twice=%ld cleared_written=%ld"
			" early=%ld bad_id=%ld other_thread=%ld order_inversions=%ld due_inversions=%ld\n",
			cfg.backend, cfg.pattern, P, static_cast<unsigned long long>(per_producer * P), sink.sunk.load(), missing, twice,
			cleared_written, sink.early, sink.bad_id, sink.other_thread, sink.inversions, sink.due_inversions);
		ok = ok && missing == 0 && twice == 0 && cleared_written == 0 && sink.early == 0 && sink.bad_id == 0 && sink.other_thread == 0;
		// (A) keeps per-thread order and due order; (C) must keep them too. (B) is reported, not gated.
		if (cfg.backend != 'B') {
			ok = ok && sink.inversions == 0 && sink.due_inversions == 0;
		}
		std::printf("check %s\n", ok ? "ok" : "FAILED");
	}

	std::vector<double> all;
	for (auto& p : producers) {
		all.insert(all.end(), p->samples.begin(), p->samples.end());
	}
	std::vector<double> lags(sink.lags.begin(), sink.lags.end());
	const double med = pct(all, 0.5), p99 = pct(all, 0.99);
	const double lag_p99 = lags.empty() ? -1 : pct(lags, 0.99) / 1000.0;
	const double lag_med = lags.empty() ? -1 : pct(lags, 0.5) / 1000.0;
	const double calls = static_cast<double>(measured_calls);

	const std::int64_t t_teardown = now_ns();
	if (cfg.backend == 'B') {
		wlog->stop();
	} else {
		Logging::finish();
		Logging::join();
	}
	const std::int64_t t_done = now_ns();
	std::printf("{\"backend\":\"%c\",\"pattern\":%d,\"producers\":%u,\"formatted\":%s,\"check\":%s,\"rounds\":%d,"
		"\"calls\":%.0f,\"median_ns\":%.3f,\"p99_ns\":%.3f,\"heap_bytes_per_call\":%.3f,\"allocs_per_call\":%.4f,"
		"\"lag_p99_us\":%.1f,\"lag_median_us\":%.1f,\"lag_lines\":%zu,\"log_cpu_ns_per_call\":%.2f,"
		"\"held_peak\":%ld,\"held_ms_per_call\":%.3f,"
		"\"time_s\":{\"warmup\":%.3f,\"timed\":%.3f,\"teardown\":%.3f,\"total\":%.3f}}\n",
		cfg.backend, cfg.pattern, P, cfg.formatted ? "true" : "false", cfg.check ? "true" : "false", cfg.rounds, calls, med, p99,
		static_cast<double>(heap1.bytes - heap0.bytes) / calls, static_cast<double>(heap1.count - heap0.count) / calls,
		lag_p99, lag_med, lags.size(), cpu0 < 0 || cpu1 < 0 ? -1.0 : (cpu1 - cpu0) / calls, held_peak, held_ms / calls,
		static_cast<double>(t_timed - t_start) / 1e9, static_cast<double>(t_end - t_timed) / 1e9,
		static_cast<double>(t_done - t_teardown) / 1e9, static_cast<double>(t_done - t_start) / 1e9);
	std::fflush(stdout);
	return ok ? 0 : 1;
}
