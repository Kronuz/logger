/*
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

#include <atomic>        // for std::atomic
#include <chrono>        // for steady_clock, system_clock, time_point
#include <cstdint>       // for uint64_t
#include <exception>     // for std::exception_ptr
#include <format>        // for std::vformat, std::make_format_args
#include <functional>    // for std::function
#include <memory>        // for std::unique_ptr, std::shared_ptr
#include <string>        // for std::string
#include <string_view>   // for std::string_view
#include <thread>        // for std::thread::id
#include <utility>       // for std::forward
#include <vector>        // for std::vector
#include <syslog.h>      // for LOG_WARNING, LOG_ERR, LOG_DEBUG, ...

#include "scheduler.h"   // for Scheduler, ScheduledTask, ThreadPolicyType


#define DEFAULT_LOG_LEVEL LOG_WARNING       // emitted when priority <= this
#define MAX_LOG_PRIORITY  (LOG_DEBUG + 1)   // highest formatted level (+1 = verbose)


// Runtime configuration. Replaces Xapiand's global `opts`.
struct LogConfig {
	int log_level = DEFAULT_LOG_LEVEL;
	bool colors = false;          // keep ANSI color in the output; false strips it
	bool with_timestamp = true;   // prepend a timestamp in the decorated path
	bool with_threads = false;    // prepend "(thread-name) "
	bool with_location = false;   // prepend "file:line at function: "

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

	static std::atomic<long> pending;   // lines on the wheel (for backpressure)
	static std::atomic<long> dropped;   // lines dropped under backpressure, not yet reported

	std::string str;
	int priority;
	bool clears;        // cancel the scheduled line when the handle is cleaned
	bool async;         // offloaded to the LOG thread (vs. inline on the caller)
	bool info;          // decorate with timestamp / thread / location
	uint64_t once;      // dedup token; 0 = no dedup
	std::exception_ptr eptr;
	std::thread::id thread_id;
	const char* function;
	const char* filename;
	int line;
	int indent;         // stacked-indent depth, captured at creation (thread-local)
	bool scheduled;     // was put on the wheel (so ~Logging decrements `pending`)
	std::chrono::system_clock::time_point timestamp;
	std::atomic<std::chrono::steady_clock::time_point> atom_cleaned_at;

	// The message that replaces this one when the handle is cleaned (the "swap").
	std::string unlog_str;
	int unlog_priority;

	Logging(const Logging&) = delete;
	Logging& operator=(const Logging&) = delete;

	static Log add(std::chrono::steady_clock::time_point wakeup, std::string&& str, bool clears, bool async, bool info,
		uint64_t once, int priority, std::exception_ptr eptr, const char* function, const char* filename, int line,
		std::chrono::steady_clock::time_point created_at = std::chrono::steady_clock::now(),
		std::chrono::system_clock::time_point timestamp = std::chrono::system_clock::now());

public:
	static LogConfig config;
	static LogHooks hooks;
	static std::vector<std::unique_ptr<Logger>> handlers;

	Logging(std::string&& str, bool clears, bool async, bool info, uint64_t once, int priority,
		std::exception_ptr eptr, const char* function, const char* filename, int line,
		std::chrono::steady_clock::time_point created_at = std::chrono::steady_clock::now(),
		std::chrono::system_clock::time_point timestamp = std::chrono::system_clock::now());
	~Logging() noexcept;

	// Install a sink. Call before logging starts; handlers are read unsynchronized.
	static void add_handler(std::unique_ptr<Logger> handler) { handlers.push_back(std::move(handler)); }

	// Low-level fan-out: write `str` to every handler now, on the calling thread.
	static void log(int priority, std::string str, bool with_priority = true, bool with_endl = true);

	// Full entry point used by the macros. Renders nothing if priority is filtered.
	static Log do_log(bool clears, std::chrono::steady_clock::time_point wakeup, bool async, bool info,
		uint64_t once, int priority, std::exception_ptr eptr, const char* function, const char* filename, int line,
		std::string&& str);

	// Convenience entry point: a plain decorated line, no exception or location.
	static Log do_log(bool clears, std::chrono::steady_clock::time_point wakeup, bool async, int priority, std::string&& str);

	// Drain and stop the LOG thread. Call at shutdown before the statics tear down.
	static bool finish(int wait = 10);
	static bool join(int timeout = 60000);

	// Lines dropped under backpressure so far (observability / tests).
	static long dropped_count() { return dropped.load(std::memory_order_relaxed); }

	void vunlog(int priority, std::string&& str);
	void clean();
	bool clear(bool internal = false);
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
		? Logging::do_log((clears), std::chrono::steady_clock::now() + (delay), (async), (info), (once), (priority), \
			(eptr), __func__, __FILE__, __LINE__, ::format_msg(__VA_ARGS__)) \
		: Log())

#define LOG(once, priority, ...) \
	L_LOG_BASE(false, std::chrono::milliseconds(0), (priority) >= ASYNC_LOG_LEVEL, true, (once), (priority), std::exception_ptr{}, __VA_ARGS__)

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
	L_LOG_BASE((clears), (delay), true, true, 0, (priority), std::exception_ptr{}, __VA_ARGS__)

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
