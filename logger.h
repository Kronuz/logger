/*
 * Copyright © 2026 Germán Méndez Bravo (Kronuz)
 * Copyright (c) 2015-2019 Dubalu LLC
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#pragma once

#include <array>         // for std::array (the marker palette)
#include <atomic>        // for std::atomic
#include <charconv>      // for std::to_chars
#include <chrono>        // for steady_clock, system_clock, time_point
#include <cstring>       // for std::memchr
#include <cstdint>       // for uint64_t
#include <exception>     // for std::exception_ptr
#include <format>        // for std::vformat, std::make_format_args
#include <functional>    // for std::function
#include <iterator>      // for std::back_inserter
#include <memory>        // for std::unique_ptr, std::shared_ptr
#include <source_location>  // for std::source_location
#include <string>        // for std::string
#include <string_view>   // for std::string_view
#include <thread>        // for std::thread::id
#include <type_traits>  // for std::remove_cvref_t, std::is_same_v
#include <utility>       // for std::forward
#include <vector>        // for std::vector
#include <syslog.h>      // for LOG_WARNING, LOG_ERR, LOG_DEBUG, ...

#include "scheduler.h"   // for Scheduler, ScheduledTask, ThreadPolicyType


#define DEFAULT_LOG_LEVEL LOG_WARNING       // emitted when priority <= this
#define MAX_LOG_PRIORITY  (LOG_DEBUG + 1)   // highest formatted level (+1 = verbose)


// When to colorize, independent of the depth below. Mirrors the conventional
// `--color=<mode>` flag (git / ls / grep).
enum class LogColorMode {
	automatic,  // color only when the sink is a tty and NO_COLOR is unset/empty
	always,     // force color, overriding the tty check and NO_COLOR
	never,      // never color
};

// The color tier term-color's depth-portable output is collapsed to. term-color
// emits three stacked escapes per color (16 / 256 / truecolor); a capable terminal
// already ends on the best tier it supports, but collapsing to one tier yields
// clean single-escape output and is robust on terminals that render rather than
// ignore the escapes they do not understand. `automatic` detects the tier from the
// terminal (COLORTERM / TERM).
enum class LogColorDepth {
	automatic,  // detect the tier from the terminal (COLORTERM / TERM)
	ansi16,     // 16-color  (\033[..;30-37/90-97m)
	ansi256,    // 256-color (\033[..;38;5;Nm)
	truecolor,  // 24-bit    (\033[..;38;2;R;G;Bm)
	stacked,    // do not collapse: emit all three stacked tiers and let the terminal
	            // apply the best it understands (no post-processing, larger output)
};

// How the decorated-line timestamp is formatted.
enum class LogTimestamp {
	none,
	datetime,   // YYYYMMDDHHMMSS (compact; pairs nicely with `timestamp_gradient`)
	iso8601,    // YYYY-MM-DD HH:MM:SS
	epoch,      // seconds since the epoch
};
enum class LogPrecision { seconds, milliseconds, microseconds };


// Runtime configuration. Replaces Xapiand's global `opts`.
struct LogConfig {
	int log_level = DEFAULT_LOG_LEVEL;
	LogColorMode color = LogColorMode::automatic;          // when to colorize (--color)
	LogColorDepth color_depth = LogColorDepth::automatic;  // tier to collapse color to
	bool with_timestamp = true;   // prepend a timestamp in the decorated path
	LogTimestamp timestamp = LogTimestamp::iso8601;        // its format
	LogPrecision precision = LogPrecision::milliseconds;   // its sub-second precision
	bool timestamp_gradient = false;  // grey-gradient the timestamp digits
	bool with_threads = false;    // prepend "(thread-name) "
	bool with_location = false;   // prepend "file:line at function: "
	bool iterm2 = true;           // emit iTerm2 escape codes (marks, tab tint, badge,
	                              // notifications) when stderr is an iTerm2 terminal
	                              // (auto-detected). false disables them entirely.

	// The per-priority severity marker (the leading bar). An empty entry falls back
	// to the built-in palette; a host can install richer (e.g. truecolor) markers.
	std::array<std::string, MAX_LOG_PRIORITY + 1> markers{};

	// Color each message in its severity's palette color (the markers' colors), as
	// Xapiand's lines read. Colors inside a message still apply after it.
	bool tint = false;

	// Backpressure. 0 = unbounded. Otherwise, once this many lines are pending on
	// the wheel, routine async lines (NOTICE and below severity, non-deferred) are
	// dropped instead of queued, and a coalesced "dropped N" summary is emitted.
	// Severe and deferred lines are never dropped.
	size_t max_pending = 0;
};


// Pluggable formatting, all optional. A host (e.g. Xapiand) injects richer
// versions of these; left empty, the core uses std-only defaults.
struct LogHooks {
	std::function<std::string(std::exception_ptr)> describe_exception;            // default: std::exception::what()
	std::function<std::string()> backtrace;                                       // default: empty
	std::function<std::string(std::thread::id)> thread_name;                      // default: the thread id
	std::function<std::string(std::chrono::system_clock::time_point)> timestamp;  // default: "YYYY-MM-DD HH:MM:SS.mmm"
};


// A sink. Every emitted line fans out to all installed handlers.
class Logger {
public:
	virtual void log(int priority, std::string_view str, bool with_priority, bool with_endl) = 0;

	virtual ~Logger() noexcept = default;
};


// Appends to a file (opened O_APPEND).
class StreamLogger : public Logger {
	int fdout;

public:
	explicit StreamLogger(const char* filename);
	~StreamLogger() noexcept override;

	void log(int priority, std::string_view str, bool with_priority, bool with_endl) override;
};


// Writes to stderr; colorizes when stderr is a tty (or colors are forced).
class StderrLogger : public Logger {
public:
	void log(int priority, std::string_view str, bool with_priority, bool with_endl) override;
};


// Writes to syslog(3).
class SysLog : public Logger {
public:
	explicit SysLog(const char* ident = "logger", int option = LOG_PID | LOG_CONS, int facility = LOG_USER);
	~SysLog() noexcept override;

	void log(int priority, std::string_view str, bool with_priority, bool with_endl) override;
};


namespace logger_detail {
// The built-in timestamp formatter, exposed for tests; not part of the API.
std::string default_timestamp(std::chrono::system_clock::time_point tp);

// The calling thread's reusable message buffers, and whether a message is being formatted into
// them: a raw buffer the fast formatter writes into, and a string std::vformat appends to. The
// slot is constant-initialized and trivially destructible, so reaching it is a plain thread-local
// access; the buffers are allocated on first use and freed (through a second thread-local) when
// the thread ends, after which the slot is `dead` and a message formats into its own string.
struct ScratchSlot {
	char* buf = nullptr;
	std::size_t cap = 0;
	std::string* text = nullptr;
	bool leased = false;
	bool dead = false;
};
inline thread_local ScratchSlot scratch_slot;
bool scratch_init() noexcept;   // allocates the buffers on first use; false once the slot is dead
char* scratch_grow() noexcept;  // doubles the raw buffer (to 1 MiB at most); null if it cannot

// What the logger keeps per thread: the thread's id and stacked-indent depth (captured into each line
// when it is made), and which of the lines-posted counters the thread adds to. One thread-local block, so
// a line's creation costs one thread-local access instead of three, and no call to pthread_self.
inline constexpr unsigned kStripes = 8;
struct ThreadState {
	std::thread::id id;
	int indent = 0;
	unsigned stripe = 0;
};
ThreadState& thread_state() noexcept;

// Passed where a line has no exception to describe, so the common path builds no std::exception_ptr
// (its destructor is a call into the runtime, even for a null one).
struct NoException {};

// std::chrono::steady_clock::now(), a little faster where the platform allows: on macOS, libc++'s
// steady_clock calls clock_gettime, which calls clock_gettime_nsec_np, which reads mach_continuous_time
// and the timebase; this reads mach_continuous_time itself (10 ns less per call on Apple silicon). It
// is used only after a check at first use that it agrees with steady_clock::now() (to a millisecond); if it
// does not, or on any other platform, this is steady_clock::now().
std::chrono::steady_clock::time_point steady_now() noexcept;

// The wall-clock time of a steady-clock reading taken now: the reading plus the offset between the
// two clocks, which is measured again when it is 20 ms old (every producer reads the steady clock
// anyway; a second clock read per line is what this saves). The wall clock's own steps and slews
// show up at the next measurement, so a stamp is exact to the clocks' drift over 20 ms (at most
// 10 us for a 500 ppm slew), not to the nanosecond.
std::chrono::system_clock::time_point system_at(std::chrono::steady_clock::time_point now);

// A fast path for the commonest messages: a format string whose fields are all `{}` (and the
// `{{` `}}` escapes) over integers, bool, char and strings. It writes exactly the bytes
// std::vformat would, straight into a raw buffer, and gives up at the first thing it is not sure
// of: a field with an index or a spec, a malformed format, too few arguments, a null `const char*`,
// or a char array with a NUL before its end (`declined`), or a message that does not fit (`full`).
// The caller then formats with std::vformat, which also decides what an error looks like. It is
// several times faster than std::vformat on these messages.
template <typename T>
inline constexpr bool fast_format_arg_v = [] {
	using U = std::remove_cvref_t<T>;
	if constexpr (std::is_same_v<U, bool> || std::is_same_v<U, char>) {
		return true;
	} else if constexpr (std::is_integral_v<U>) {
		return sizeof(U) <= 8 && !std::is_same_v<U, wchar_t> && !std::is_same_v<U, char8_t>
			&& !std::is_same_v<U, char16_t> && !std::is_same_v<U, char32_t>;
	} else if constexpr (std::is_array_v<U>) {
		return std::is_same_v<std::remove_cv_t<std::remove_extent_t<U>>, char>;
	} else if constexpr (std::is_pointer_v<U>) {
		return std::is_same_v<std::remove_cv_t<std::remove_pointer_t<U>>, char>;
	} else {
		return std::is_same_v<U, std::string> || std::is_same_v<U, std::string_view>;
	}
}();

enum class FastResult : unsigned char { ok, declined, full };

// A write position in the raw buffer.
struct FastOut {
	char* p;
	char* end;
};

// Copies n bytes without a call for the short runs that make up most of a message: two overlapping
// loads and stores of the widest size that fits (they read only inside [s, s + n)).
inline void fast_copy_bytes(char* d, const char* s, std::size_t n) noexcept {
	if (n >= 16) {
		std::memcpy(d, s, n);
	} else if (n >= 8) {
		std::uint64_t a, b;
		std::memcpy(&a, s, 8);
		std::memcpy(&b, s + n - 8, 8);
		std::memcpy(d, &a, 8);
		std::memcpy(d + n - 8, &b, 8);
	} else if (n >= 4) {
		std::uint32_t a, b;
		std::memcpy(&a, s, 4);
		std::memcpy(&b, s + n - 4, 4);
		std::memcpy(d, &a, 4);
		std::memcpy(d + n - 4, &b, 4);
	} else if (n >= 2) {
		std::uint16_t a, b;
		std::memcpy(&a, s, 2);
		std::memcpy(&b, s + n - 2, 2);
		std::memcpy(d, &a, 2);
		std::memcpy(d + n - 2, &b, 2);
	} else if (n == 1) {
		*d = *s;
	}
}

inline FastResult fast_put_bytes(FastOut& o, const char* s, std::size_t n) noexcept {
	if (static_cast<std::size_t>(o.end - o.p) < n) {
		return FastResult::full;
	}
	fast_copy_bytes(o.p, s, n);
	o.p += n;
	return FastResult::ok;
}

template <typename T>
inline FastResult fast_format_put(FastOut& o, const void* p) {
	using U = std::remove_cvref_t<T>;
	if constexpr (std::is_same_v<U, bool>) {
		return *static_cast<const bool*>(p) ? fast_put_bytes(o, "true", 4) : fast_put_bytes(o, "false", 5);
	} else if constexpr (std::is_same_v<U, char>) {
		return fast_put_bytes(o, static_cast<const char*>(p), 1);
	} else if constexpr (std::is_integral_v<U>) {
		if (o.end - o.p < 24) {
			return FastResult::full;
		}
		o.p = std::to_chars(o.p, o.p + 24, *static_cast<const U*>(p)).ptr;
		return FastResult::ok;
	} else if constexpr (std::is_array_v<U>) {
		constexpr std::size_t n = std::extent_v<U>;
		auto s = static_cast<const char*>(p);
		if (n == 0) {
			return FastResult::declined;
		}
		for (std::size_t i = 0; i + 1 < n; ++i) {
			if (s[i] == '\0') {
				return FastResult::declined;   // a NUL before the end: std::vformat decides what that prints
			}
		}
		if (s[n - 1] != '\0') {
			return FastResult::declined;
		}
		return fast_put_bytes(o, s, n - 1);
	} else if constexpr (std::is_pointer_v<U>) {
		auto s = *static_cast<const U*>(p);
		if (s == nullptr) {
			return FastResult::declined;
		}
		return fast_put_bytes(o, s, std::strlen(s));
	} else {
		const auto& v = *static_cast<const U*>(p);
		return fast_put_bytes(o, v.data(), v.size());
	}
}

template <typename... Args>
inline FastResult fast_format(FastOut& o, std::string_view fmt, const Args&... args) {
	constexpr std::size_t N = sizeof...(Args);
	using Put = FastResult (*)(FastOut&, const void*);
	static constexpr Put puts[N] = {&fast_format_put<Args>...};
	const void* const ptrs[N] = {static_cast<const void*>(std::addressof(args))...};
	std::size_t next = 0;
	const char* p = fmt.data();
	const char* const end = p + fmt.size();
	while (p < end) {
		const char* q = p;
		while (q < end && *q != '{' && *q != '}') {
			++q;
		}
		if (auto r = fast_put_bytes(o, p, static_cast<std::size_t>(q - p)); r != FastResult::ok) {
			return r;
		}
		if (q == end) {
			break;
		}
		const bool paired = q + 1 < end && q[1] == q[0];   // `{{` or `}}`
		if (paired) {
			if (auto r = fast_put_bytes(o, q, 1); r != FastResult::ok) {
				return r;
			}
			p = q + 2;
		} else if (*q == '{' && q + 1 < end && q[1] == '}') {
			if (next >= N) {
				return FastResult::declined;
			}
			if (auto r = puts[next](o, ptrs[next]); r != FastResult::ok) {
				return r;
			}
			++next;
			p = q + 2;
		} else {
			return FastResult::declined;   // an index, a spec, a lone brace
		}
	}
	return FastResult::ok;
}

// The fast path into a string, for tests: whether it took the message, and what it wrote.
template <typename... Args>
inline bool fast_format_to(std::string& out, std::string_view fmt, const Args&... args) {
	static thread_local std::string buf(1 << 16, '\0');
	FastOut o{buf.data(), buf.data() + buf.size()};
	if (fast_format(o, fmt, args...) != FastResult::ok) {
		return false;
	}
	out.assign(buf.data(), o.p);
	return true;
}

// A formatted message, valid until the end of the full expression that made it. It lives in the
// thread's reusable buffers, so a thread that logs steadily formats without allocating; when those
// are already in use (a formatter that itself logs) or gone (the thread is exiting), it takes a
// string of its own.
class FormattedMsg {
	ScratchSlot* slot_ = nullptr;
	bool big_ = false;   // std::vformat grew the thread's string past kKeepText: give it back at the end
	std::string local_;
	std::string_view view_;

public:
	// Renders like format_msg: no arguments gives the format string as it is, and a formatting
	// error gives it too.
	template <typename... Args>
	explicit FormattedMsg(std::string_view fmt, Args&&... args) {
		auto& slot = scratch_slot;
		if (!slot.leased && !slot.dead && (slot.buf != nullptr || scratch_init())) {
			slot.leased = true;
			slot_ = &slot;
		}
		try {
			render(fmt, args...);
		} catch (...) {   // out of memory: the destructor will not run, so give the buffers back here
			release();
			throw;
		}
	}
	~FormattedMsg() { release(); }
	FormattedMsg(const FormattedMsg&) = delete;
	FormattedMsg& operator=(const FormattedMsg&) = delete;

	std::string_view view() const noexcept { return view_; }

private:
	// The most capacity the thread's string keeps once a message is done with it.
	static constexpr std::size_t kKeepText = std::size_t{1} << 20;

	void release() noexcept {
		if (slot_) {
			if (big_) {
				std::string().swap(*slot_->text);
			}
			slot_->leased = false;
			slot_ = nullptr;
		}
	}

	// The message through std::vformat into the thread's string, or this object's when that is busy.
	template <typename... Args>
	void render_std(std::string_view fmt, Args&... args) {
		std::string* text = slot_ != nullptr ? slot_->text : &local_;
		text->clear();
		if constexpr (sizeof...(Args) == 0) {
			text->assign(fmt);
		} else {
			try {
				std::vformat_to(std::back_inserter(*text), fmt, std::make_format_args(args...));
			} catch (...) {
				text->assign(fmt);
			}
		}
		view_ = *text;
		big_ = slot_ != nullptr && text->capacity() > kKeepText;
	}

	template <typename... Args>
	void render(std::string_view fmt, Args&... args) {
		if constexpr (sizeof...(Args) != 0 && (fast_format_arg_v<Args> && ...)) {
			if (slot_ != nullptr) {
				for (;;) {
					FastOut o{slot_->buf, slot_->buf + slot_->cap};
					auto r = fast_format(o, fmt, args...);
					if (r == FastResult::ok) {
						view_ = std::string_view(slot_->buf, static_cast<std::size_t>(o.p - slot_->buf));
						return;
					}
					if (r == FastResult::declined || scratch_grow() == nullptr) {
						break;
					}
				}
			}
		}
		render_std(fmt, args...);
	}
};
}

class Log;
class Logging;
using LogType = std::shared_ptr<Logging>;


// One log entry, and also a ScheduledTask so the async and deferred paths can
// ride the scheduler's lock-free wheel. The scheduler's single "LOG" consumer
// thread fires operator() and writes the line. See ARCHITECTURE.md.
class Logging : public ScheduledTask<Scheduler<Logging, ThreadPolicyType::logging>, Logging, ThreadPolicyType::logging> {
	friend class Log;

	using Base = ScheduledTask<Scheduler<Logging, ThreadPolicyType::logging>, Logging, ThreadPolicyType::logging>;

	static Scheduler<Logging, ThreadPolicyType::logging>& scheduler();

	// Lines on the wheel and lines dropped, each counter on its own cache line, because
	// producers write them on every line while the LOG thread writes others: a line per
	// producer stripe counts lines put on the wheel, one the LOG thread owns counts those
	// that left it, and the lines pending are the difference.
	struct alignas(64) Counter {
		std::atomic<long> n{0};
	};
	static Counter posted_[logger_detail::kStripes];   // lines put on the wheel, by the putting thread's stripe
	static Counter completed_;          // lines that left it (fired, cancelled, refused or dropped)
	static Counter dropped_;            // lines dropped under backpressure, not yet reported
	static Counter& stripe() noexcept;  // this thread's posted_ entry

	std::string str;
	int priority;
	bool clears;        // cancel the scheduled line when the handle is cleaned
	bool async;         // offloaded to the LOG thread (vs. inline on the caller)
	bool info;          // decorate with timestamp / thread / location
	uint64_t once;      // dedup token; 0 = no dedup
	std::exception_ptr eptr;
	std::thread::id thread_id;
	std::source_location loc;   // call site (file / line / function), C++20
	int indent;         // stacked-indent depth, captured at creation (thread-local)
	bool scheduled;     // was put on the wheel (so ~Logging counts it as completed)
	// Set when the scheduler refused a future `clears` line: it stays armed off the
	// wheel, and its cancel point (clean() or clear()) writes it only if this
	// deadline has passed, i.e. the work was slow.
	std::chrono::steady_clock::time_point refused_deadline{};
	std::chrono::system_clock::time_point timestamp;
	std::atomic<std::chrono::steady_clock::time_point> atom_cleaned_at;

	// The message that replaces this one when the handle is cleaned (the "swap").
	std::string unlog_str;
	int unlog_priority;

	Logging(const Logging&) = delete;
	Logging& operator=(const Logging&) = delete;

	static Log add(std::chrono::steady_clock::time_point wakeup, std::string&& str, bool clears, bool async, bool info,
		uint64_t once, int priority, std::exception_ptr* eptr, std::source_location loc,
		std::chrono::steady_clock::time_point created_at,
		std::chrono::system_clock::time_point timestamp,
		std::chrono::steady_clock::time_point now);

	// The same for a message still in the caller's buffer: the record copies it.
	static Log add(std::chrono::steady_clock::time_point wakeup, std::string_view str, bool clears, bool async, bool info,
		uint64_t once, int priority, std::exception_ptr* eptr, std::source_location loc,
		std::chrono::steady_clock::time_point created_at,
		std::chrono::system_clock::time_point timestamp,
		std::chrono::steady_clock::time_point now);

	template <typename Msg>
	static Log add_line(std::chrono::steady_clock::time_point wakeup, Msg&& str, bool clears, bool async, bool info,
		uint64_t once, int priority, std::exception_ptr* eptr, std::source_location loc,
		std::chrono::steady_clock::time_point created_at,
		std::chrono::system_clock::time_point timestamp,
		std::chrono::steady_clock::time_point now);

public:
	static LogConfig config;
	static LogHooks hooks;
	static std::vector<std::unique_ptr<Logger>> handlers;

	Logging(std::string&& str, bool clears, bool async, bool info, uint64_t once, int priority,
		std::exception_ptr eptr, std::source_location loc,
		std::chrono::steady_clock::time_point created_at = std::chrono::steady_clock::now(),
		std::chrono::system_clock::time_point timestamp = std::chrono::system_clock::now());
	// A line with no exception (the usual one).
	Logging(std::string&& str, bool clears, bool async, bool info, uint64_t once, int priority,
		std::source_location loc,
		std::chrono::steady_clock::time_point created_at,
		std::chrono::system_clock::time_point timestamp);
	~Logging() noexcept;

	// Install a sink. Call before logging starts; handlers are read unsynchronized.
	static void add_handler(std::unique_ptr<Logger> handler) { handlers.push_back(std::move(handler)); }

	// Low-level fan-out: write `str` to every handler now, on the calling thread.
	static void log(int priority, std::string str, bool with_priority = true, bool with_endl = true);

	// Full entry point used by the macros. Renders nothing if priority is filtered.
	static Log do_log(bool clears, std::chrono::steady_clock::time_point wakeup, bool async, bool info,
		uint64_t once, int priority, std::exception_ptr eptr, std::source_location loc,
		std::string&& str);

	// Convenience entry point: a plain decorated line, no exception or location.
	static Log do_log(bool clears, std::chrono::steady_clock::time_point wakeup, bool async, int priority, std::string&& str);

	// The entry point of the macros: the line is due `delay` from now, and its message is
	// formatted in the thread's reusable buffer. Renders nothing if priority is filtered.
	static Log do_log_in(bool clears, std::chrono::steady_clock::duration delay, bool async, bool info,
		uint64_t once, int priority, logger_detail::NoException, std::source_location loc,
		const logger_detail::FormattedMsg& msg);
	static Log do_log_in(bool clears, std::chrono::steady_clock::duration delay, bool async, bool info,
		uint64_t once, int priority, std::exception_ptr eptr, std::source_location loc,
		const logger_detail::FormattedMsg& msg);

	// Drain and stop the LOG thread. Call at shutdown before the statics tear down.
	static bool finish(int wait = 10);
	static bool join(int timeout = 60000);

	// Lines dropped under backpressure so far (observability / tests).
	static long dropped_count() { return dropped_.n.load(std::memory_order_relaxed); }

	// Lines on the wheel now (observability / tests).
	static long pending_count() {
		// `completed_` first: a line's increment of its stripe comes before its push, which the LOG
		// thread acquired before it completed the line (a release add), so every line counted in
		// `done` has its increment visible to the stripe reads below and the sum cannot fall short.
		long done = completed_.n.load(std::memory_order_acquire);
		long n = 0;
		for (const auto& c : posted_) {
			n += c.n.load(std::memory_order_relaxed);
		}
		n -= done;
		return n > 0 ? n : 0;
	}

	// iTerm2 terminal integration. Each emits an iTerm2 proprietary escape code,
	// and is a safe no-op unless stderr is an iTerm2 terminal (auto-detected via
	// TERM_PROGRAM / LC_TERMINAL) and config.iterm2 is set. Callable from anywhere.
	static bool iterm2_available();                      // true if iTerm2 + a tty + enabled
	static void set_mark();                              // a navigable mark in the scrollback
	static void tab_rgb(int red, int green, int blue);   // tint the tab / title bar
	static void tab_title(std::string_view title);       // set the tab / window title
	static void badge(std::string_view text);            // set the iTerm2 badge
	static void growl(std::string_view text);            // post a notification
	static void reset_iterm2();                          // clear badge + tab tint

	void vunlog(int priority, std::string&& str);
	void clean();
	bool clear(bool internal = false);
	bool refused_late() const;   // refused by the scheduler and past its deadline
	std::chrono::nanoseconds age();

	void operator()();
};


// Owns a scheduled log and lets the caller cancel it or swap its message before
// it fires. Dropping the handle runs clean(): cancel the line if it `clears`,
// otherwise emit the swapped-in message.
class Log {
	LogType entry;

	Log(const Log&) = delete;
	Log& operator=(const Log&) = delete;

public:
	Log() = default;
	Log(Log&& o) noexcept;
	Log& operator=(Log&& o) noexcept;
	explicit Log(LogType entry);
	~Log() noexcept;

	void unlog(int priority, std::string str);
	bool clear();
	std::chrono::nanoseconds age();
	LogType release();
};


// Remove ANSI SGR escape sequences from `str` (used when color is off).
std::string strip_colors(std::string_view str);


// --- formatting helpers (used by the macros) -----------------------------

// Render a std::format string with its arguments, falling back to the raw
// format string if formatting throws. With no arguments the string is returned
// as-is (so a literal '{' in a plain message is not a format error).
template <typename... Args>
inline std::string format_msg(std::string_view fmt, Args&&... args) {
	if constexpr (sizeof...(Args) == 0) {
		return std::string(fmt);
	} else {
		try {
			return std::vformat(fmt, std::make_format_args(args...));
		} catch (...) {
			return std::string(fmt);
		}
	}
}

// Immediate, undecorated output (no priority marker), on the calling thread.
template <typename... Args>
inline void print_msg(std::string_view fmt, Args&&... args) {
	Logging::log(0, format_msg(fmt, std::forward<Args>(args)...), false, true);
}


// --- thread-local stacked indent (replaces the global mutex + map) --------

int& log_indent() noexcept;

// RAII guard: deepens the indent for the rest of the enclosing scope.
struct LogIndent {
	LogIndent() noexcept { ++log_indent(); }
	~LogIndent() noexcept { --log_indent(); }
	LogIndent(const LogIndent&) = delete;
	LogIndent& operator=(const LogIndent&) = delete;
};


// --- the L_* macro surface -----------------------------------------------

#define ASYNC_LOG_LEVEL LOG_ERR   // priority >= this is offloaded to the LOG thread

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-zero-variadic-macro-arguments"

#define L_MERGE_(a, b) a##b
#define L_LABEL_(a) L_MERGE_(__log_scope_, a)
#define L_UNIQUE_NAME L_LABEL_(__LINE__)

// Filter first, so a line below the level evaluates none of its arguments; then
// build the message and route it. Yields a Log handle.
#define L_LOG_BASE(clears, delay, async, info, once, priority, eptr, ...) \
	(((priority) <= Logging::config.log_level) \
		? Logging::do_log_in((clears), (delay), (async), (info), (once), (priority), \
			(eptr), std::source_location::current(), logger_detail::FormattedMsg(__VA_ARGS__)) \
		: Log())

#define LOG(once, priority, ...) \
	L_LOG_BASE(false, std::chrono::milliseconds(0), (priority) >= ASYNC_LOG_LEVEL, true, (once), (priority), logger_detail::NoException{}, __VA_ARGS__)

#define L(priority, ...) LOG(0, (priority), __VA_ARGS__)

#define L_INFO(...)    LOG(0, LOG_INFO, __VA_ARGS__)
#define L_NOTICE(...)  LOG(0, LOG_NOTICE, __VA_ARGS__)
#define L_WARNING(...) LOG(0, LOG_WARNING, __VA_ARGS__)
#define L_ERR(...)     LOG(0, LOG_ERR, __VA_ARGS__)
#define L_CRIT(...)    LOG(0, LOG_CRIT, __VA_ARGS__)
#define L_ALERT(...)   LOG(0, LOG_ALERT, __VA_ARGS__)
#define L_EMERG(...)   LOG(0, LOG_EMERG, __VA_ARGS__)

// Logs the in-flight exception (std::current_exception) at CRIT, asynchronously.
// stash/scheduler bundle a no-op `L_EXC` stub so they build without a logger;
// the real one wins here, which is also how Xapiand wires it.
#undef L_EXC
#define L_EXC(...) \
	L_LOG_BASE(false, std::chrono::milliseconds(0), true, true, 0, LOG_CRIT, std::current_exception(), __VA_ARGS__)

// A per-minute dedup token: the current minute, forced non-zero.
#define L_ONCE_PER_MINUTE_TOKEN \
	(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::minutes>( \
		std::chrono::steady_clock::now().time_since_epoch()).count()) | 1ULL)

#define L_INFO_ONCE(...)    LOG(1, LOG_INFO, __VA_ARGS__)
#define L_NOTICE_ONCE(...)  LOG(1, LOG_NOTICE, __VA_ARGS__)
#define L_WARNING_ONCE(...) LOG(1, LOG_WARNING, __VA_ARGS__)
#define L_ERR_ONCE(...)     LOG(1, LOG_ERR, __VA_ARGS__)

#define L_WARNING_ONCE_PER_MINUTE(...) LOG(L_ONCE_PER_MINUTE_TOKEN, LOG_WARNING, __VA_ARGS__)
#define L_ERR_ONCE_PER_MINUTE(...)     LOG(L_ONCE_PER_MINUTE_TOKEN, LOG_ERR, __VA_ARGS__)

// Deferred "log slow ops only": fires after `delay` unless the handle clears it.
#define L_DELAYED(clears, delay, priority, ...) \
	L_LOG_BASE((clears), (delay), true, true, 0, (priority), logger_detail::NoException{}, __VA_ARGS__)

#define L_DELAYED_100(...)  auto __log_delayed = L_DELAYED(true, std::chrono::milliseconds(100), LOG_WARNING, __VA_ARGS__)
#define L_DELAYED_200(...)  auto __log_delayed = L_DELAYED(true, std::chrono::milliseconds(200), LOG_WARNING, __VA_ARGS__)
#define L_DELAYED_1000(...) auto __log_delayed = L_DELAYED(true, std::chrono::milliseconds(1000), LOG_WARNING, __VA_ARGS__)
#define L_DELAYED_N(delay, ...) auto __log_delayed = L_DELAYED(true, (delay), LOG_WARNING, __VA_ARGS__)

// Called on a Log handle: swap in / cancel its pending line.
#define L_DELAYED_UNLOG(priority, ...) unlog((priority), ::format_msg(__VA_ARGS__))
#define L_DELAYED_CLEAR() clear()
#define L_DELAYED_N_UNLOG(...) __log_delayed.L_DELAYED_UNLOG(LOG_NOTICE, __VA_ARGS__)
#define L_DELAYED_N_CLEAR() __log_delayed.L_DELAYED_CLEAR()

// Arm a "taking too long" line; replace it with a "done" line on completion.
#define L_TIMED(delay, fmt_timeout, fmt_done, ...) \
	auto __log_timed = L_DELAYED(true, (delay), LOG_WARNING, (fmt_timeout), ##__VA_ARGS__); \
	__log_timed.unlog(LOG_NOTICE, ::format_msg((fmt_done), ##__VA_ARGS__))

// Indented (stacked) log: logs after it in this scope are nested one level deeper.
#define L_STACKED(priority, ...) L((priority), __VA_ARGS__); LogIndent L_UNIQUE_NAME

#define L_PRINT(...) ::print_msg(__VA_ARGS__)

// Debug logs compile to nothing by default; define LOGGER_DEBUG to enable them.
#ifdef LOGGER_DEBUG
#define L_DEBUG(...) LOG(0, LOG_DEBUG, __VA_ARGS__)
#else
#define L_DEBUG(...)
#endif

#pragma clang diagnostic pop
