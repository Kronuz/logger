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
#include <functional>    // for std::function
#include <memory>        // for std::unique_ptr, std::shared_ptr
#include <string>        // for std::string
#include <string_view>   // for std::string_view
#include <thread>        // for std::thread::id
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
