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
#include <memory>        // for std::unique_ptr, std::shared_ptr
#include <string>        // for std::string
#include <string_view>   // for std::string_view
#include <vector>        // for std::vector
#include <syslog.h>      // for LOG_WARNING, LOG_ERR, LOG_DEBUG, ...

#include "scheduler.h"   // for Scheduler, ScheduledTask, ThreadPolicyType


#define DEFAULT_LOG_LEVEL LOG_WARNING       // emitted when priority <= this
#define MAX_LOG_PRIORITY  (LOG_DEBUG + 1)   // highest formatted level (+1 = verbose)


// Runtime configuration. Replaces Xapiand's global `opts`; later phases grow
// this with more format toggles and the async cutoff.
struct LogConfig {
	int log_level = DEFAULT_LOG_LEVEL;
	bool colors = false;          // keep ANSI color in the output; false strips it
	bool with_timestamp = true;   // prepend a timestamp in the decorated path
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
// thread is the one that fires operator() and writes the line. See
// ARCHITECTURE.md for the three paths a log can take.
class Logging : public ScheduledTask<Scheduler<Logging, ThreadPolicyType::logging>, Logging, ThreadPolicyType::logging> {
	friend class Log;

	using Base = ScheduledTask<Scheduler<Logging, ThreadPolicyType::logging>, Logging, ThreadPolicyType::logging>;

	static Scheduler<Logging, ThreadPolicyType::logging>& scheduler();

	std::string str;
	int priority;
	bool clears;        // cancel the scheduled line when the handle is cleaned
	bool async;         // offloaded to the LOG thread (vs. inline on the caller)
	std::chrono::system_clock::time_point timestamp;
	std::atomic<std::chrono::steady_clock::time_point> atom_cleaned_at;

	// The message that replaces this one when the handle is cleaned (the "swap").
	std::string unlog_str;
	int unlog_priority;

	Logging(const Logging&) = delete;
	Logging& operator=(const Logging&) = delete;

	// Build a Logging and route it: inline if it is due now and synchronous,
	// otherwise onto the scheduler keyed at `wakeup`.
	static Log add(std::chrono::steady_clock::time_point wakeup, std::string&& str, bool clears, bool async, int priority,
		std::chrono::steady_clock::time_point created_at = std::chrono::steady_clock::now(),
		std::chrono::system_clock::time_point timestamp = std::chrono::system_clock::now());

public:
	static LogConfig config;
	static std::vector<std::unique_ptr<Logger>> handlers;

	Logging(std::string&& str, bool clears, bool async, int priority,
		std::chrono::steady_clock::time_point created_at = std::chrono::steady_clock::now(),
		std::chrono::system_clock::time_point timestamp = std::chrono::system_clock::now());
	~Logging() noexcept;

	// Low-level fan-out: write `str` to every handler now, on the calling thread.
	static void log(int priority, std::string str, bool with_priority = true, bool with_endl = true);

	// The public entry point. Renders nothing if priority is filtered out.
	static Log do_log(bool clears, std::chrono::steady_clock::time_point wakeup, bool async, int priority, std::string&& str);

	// Drain and stop the LOG thread. Call at shutdown before the statics tear
	// down, or pending async lines can outlive their sinks.
	static bool finish(int wait = 10);
	static bool join(int timeout = 60000);

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
