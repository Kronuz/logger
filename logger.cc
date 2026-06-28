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

#include "logger.h"

#include <cerrno>        // for errno, EINTR
#include <cstdio>        // for snprintf
#include <ctime>         // for localtime_r, std::tm
#include <fcntl.h>       // for open, O_WRONLY, O_CREAT, O_APPEND
#include <system_error>  // for std::system_error
#include <unistd.h>      // for write, isatty, close, STDERR_FILENO

using namespace std::chrono_literals;


LogConfig Logging::config;
std::vector<std::unique_ptr<Logger>> Logging::handlers;


// One marker per syslog priority (LOG_EMERG=0 .. LOG_DEBUG=7), plus a blank
// "verbose" slot. The bar glyph carries the level at a glance; the trailing
// "\033[0m" resets color, and render() strips the whole escape when color is off.
static const std::string priorities[MAX_LOG_PRIORITY + 1] = {
	"\033[1;41m█\033[0m",  // LOG_EMERG   0
	"\033[1;31m▉\033[0m",  // LOG_ALERT   1
	"\033[31m▊\033[0m",    // LOG_CRIT    2
	"\033[31m▋\033[0m",    // LOG_ERR     3
	"\033[33m▌\033[0m",    // LOG_WARNING 4
	"\033[32m▍\033[0m",    // LOG_NOTICE  5
	"\033[36m▎\033[0m",    // LOG_INFO    6
	"\033[90m▏\033[0m",    // LOG_DEBUG   7
	"",                    // verbose   > 7
};


static int
validated_priority(int priority)
{
	if (priority < 0) {
		priority = -priority;
	}
	if (priority > MAX_LOG_PRIORITY) {
		priority = MAX_LOG_PRIORITY;
	}
	return priority;
}


static bool
is_tty()
{
	static const bool tty = ::isatty(STDERR_FILENO) != 0;
	return tty;
}


// write(2) loop that retries on EINTR and short writes.
static void
write_all(int fd, const char* data, size_t size)
{
	while (size > 0) {
		ssize_t n = ::write(fd, data, size);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			break;
		}
		data += n;
		size -= static_cast<size_t>(n);
	}
}


std::string
strip_colors(std::string_view str)
{
	std::string out;
	out.reserve(str.size());
	for (size_t i = 0; i < str.size(); ++i) {
		if (str[i] == '\033' && i + 1 < str.size() && str[i + 1] == '[') {
			i += 2;
			while (i < str.size() && str[i] != 'm') {
				++i;
			}
			// the loop's ++i steps past the terminating 'm'
		} else {
			out.push_back(str[i]);
		}
	}
	return out;
}


// Build the bytes a sink writes: optional priority marker, the message,
// optional newline, with color kept or stripped per `colorize`.
static std::string
render(int priority, std::string_view str, bool with_priority, bool with_endl, bool colorize)
{
	std::string buf;
	if (with_priority) {
		buf += priorities[priority];
	}
	buf += str;
	if (with_endl) {
		buf += '\n';
	}
	return colorize ? buf : strip_colors(buf);
}


// Default timestamp decoration: "YYYY-MM-DD HH:MM:SS.mmm ". A later phase makes
// this an injectable hook so a host can match its own format.
static std::string
default_timestamp(std::chrono::system_clock::time_point tp)
{
	using namespace std::chrono;
	auto t = system_clock::to_time_t(tp);
	auto ms = duration_cast<milliseconds>(tp.time_since_epoch()) % 1000;
	std::tm tm{};
	localtime_r(&t, &tm);
	char buf[40];
	std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d ",
		tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
		tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms.count()));
	return buf;
}


// Compact age annotation for a slow async/deferred line.
static std::string
format_delta(std::chrono::nanoseconds ns)
{
	auto ms = std::chrono::duration<double, std::milli>(ns).count();
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%.1fms", ms);
	return buf;
}


StreamLogger::StreamLogger(const char* filename)
	: fdout(::open(filename, O_WRONLY | O_CREAT | O_APPEND, 0644))
{
	if (fdout == -1) {
		throw std::system_error(errno, std::generic_category());
	}
}

StreamLogger::~StreamLogger() noexcept
{
	if (fdout != -1) {
		::close(fdout);
	}
}

void
StreamLogger::log(int priority, std::string_view str, bool with_priority, bool with_endl)
{
	auto buf = render(priority, str, with_priority, with_endl, Logging::config.colors);
	write_all(fdout, buf.data(), buf.size());
}


void
StderrLogger::log(int priority, std::string_view str, bool with_priority, bool with_endl)
{
	auto buf = render(priority, str, with_priority, with_endl, is_tty() || Logging::config.colors);
	write_all(STDERR_FILENO, buf.data(), buf.size());
}


SysLog::SysLog(const char* ident, int option, int facility)
{
	openlog(ident, option, facility);
}

SysLog::~SysLog() noexcept
{
	closelog();
}

void
SysLog::log(int priority, std::string_view str, bool with_priority, bool /*with_endl*/)
{
	auto buf = render(priority, str, with_priority, false, Logging::config.colors);
	syslog(priority, "%s", buf.c_str());
}


// --- Logging -------------------------------------------------------------

Logging::Logging(std::string&& str_, bool clears_, bool async_, int priority_,
		std::chrono::steady_clock::time_point created_at,
		std::chrono::system_clock::time_point timestamp_)
	: Base(created_at),
	  str(std::move(str_)),
	  priority(priority_),
	  clears(clears_),
	  async(async_),
	  timestamp(timestamp_),
	  atom_cleaned_at(std::chrono::steady_clock::time_point{}),
	  unlog_priority(0)
{
}

Logging::~Logging() noexcept
{
	try {
		clean();
	} catch (...) {
	}
}


// Static-init-order safe singleton: the LOG thread starts on first use.
Scheduler<Logging, ThreadPolicyType::logging>&
Logging::scheduler()
{
	static Scheduler<Logging, ThreadPolicyType::logging> scheduler("LOG");
	return scheduler;
}


bool
Logging::finish(int wait)
{
	return scheduler().finish(wait);
}

bool
Logging::join(int timeout)
{
	return scheduler().join(std::chrono::milliseconds(timeout));
}


void
Logging::log(int priority, std::string str, bool with_priority, bool with_endl)
{
	priority = validated_priority(priority);
	for (auto& handler : handlers) {
		handler->log(priority, str, with_priority, with_endl);
	}
}


Log
Logging::do_log(bool clears, std::chrono::steady_clock::time_point wakeup, bool async, int priority, std::string&& str)
{
	if (priority <= config.log_level) {
		return add(wakeup, std::move(str), clears, async, priority);
	}
	return Log();
}


Log
Logging::add(std::chrono::steady_clock::time_point wakeup, std::string&& str, bool clears, bool async, int priority,
		std::chrono::steady_clock::time_point created_at,
		std::chrono::system_clock::time_point timestamp)
{
	auto entry = std::make_shared<Logging>(std::move(str), clears, async, priority, created_at, timestamp);

	if (async || wakeup > std::chrono::steady_clock::now()) {
		// async ("now") and deferred ("now + delay") logs ride the wheel; the
		// LOG thread fires operator() and writes them.
		scheduler().add(entry, wakeup);
	} else {
		// immediate logs run inline on the calling thread.
		(*entry)();
	}

	return Log(entry);
}


void
Logging::operator()()
{
	std::string msg;
	if (config.with_timestamp) {
		msg += default_timestamp(timestamp);
	}
	msg += str;

	if (async) {
		auto a = age();
		if (a > 100ms) {
			msg += " ~";
			msg += format_delta(a);
		}
	}

	log(priority, std::move(msg));
}


void
Logging::vunlog(int priority_, std::string&& str_)
{
	unlog_priority = priority_;
	unlog_str = std::move(str_);
}


void
Logging::clean()
{
	if (clears) {
		// If we cancel the scheduled line before it fired, stay silent: drop the
		// swap message too. If it already fired, clear(true) loses the one-shot
		// CAS and the swap below still runs.
		if (clear(true)) {
			unlog_str.clear();
		}
	}

	auto now = std::chrono::steady_clock::now();
	std::chrono::steady_clock::time_point c{};
	if (atom_cleaned_at.compare_exchange_strong(c, now)) {
		if (!unlog_str.empty() && unlog_priority <= config.log_level) {
			add(now, std::move(unlog_str), false, async, unlog_priority, atom_created_at.load(), timestamp);
			unlog_str.clear();
		}
	}
}


bool
Logging::clear(bool internal)
{
	if (!internal) {
		unlog_str.clear();
	}
	return Base::clear(internal);
}


std::chrono::nanoseconds
Logging::age()
{
	auto created_at = atom_created_at.load();

	auto cleaned_at = atom_cleaned_at.load();
	if (cleaned_at > created_at) {
		return std::chrono::duration_cast<std::chrono::nanoseconds>(cleaned_at - created_at);
	}

	auto cleared_at = atom_cleared_at.load();
	if (cleared_at > created_at) {
		return std::chrono::duration_cast<std::chrono::nanoseconds>(cleared_at - created_at);
	}

	return 0ns;
}


// --- Log handle ----------------------------------------------------------

Log::Log(LogType entry_)
	: entry(std::move(entry_))
{
}

Log::Log(Log&& o) noexcept
	: entry(std::move(o.entry))
{
}

Log&
Log::operator=(Log&& o) noexcept
{
	entry = std::move(o.entry);
	o.entry.reset();
	return *this;
}

Log::~Log() noexcept
{
	try {
		if (entry) {
			entry->clean();
		}
	} catch (...) {
	}
}

void
Log::unlog(int priority, std::string str)
{
	if (entry) {
		entry->vunlog(priority, std::move(str));
	}
}

bool
Log::clear()
{
	return entry ? entry->clear() : false;
}

std::chrono::nanoseconds
Log::age()
{
	return entry ? entry->age() : 0ns;
}

LogType
Log::release()
{
	auto ret = entry;
	entry.reset();
	return ret;
}
