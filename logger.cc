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

#include <cerrno>          // for errno, EINTR
#include <cstdint>         // for uint8_t, uint32_t
#include <cstdio>          // for snprintf
#include <cstdlib>         // for std::getenv
#include <ctime>           // for localtime_r, std::tm
#include <exception>       // for std::rethrow_exception
#include <fcntl.h>         // for open, O_WRONLY, O_CREAT, O_APPEND
#include <functional>      // for std::hash
#include <mutex>           // for std::mutex, std::lock_guard
#include <sstream>         // for std::ostringstream
#include <system_error>   // for std::system_error
#include <unordered_set>   // for std::unordered_set
#include <unistd.h>        // for write, isatty, close, STDERR_FILENO

using namespace std::chrono_literals;


LogConfig Logging::config;
LogHooks Logging::hooks;
std::vector<std::unique_ptr<Logger>> Logging::handlers;
std::atomic<long> Logging::pending{0};
std::atomic<long> Logging::dropped{0};


// Thread-local stacked-indent depth. Per-thread by construction, so it needs no
// lock and no shared map (unlike the original's mutex-guarded thread_id map).
int&
log_indent() noexcept
{
	thread_local int depth = 0;
	return depth;
}


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


// Bounded dedup for `once` lines. Two alternating sets: when the active set
// fills, it becomes the "previous" set and a fresh active starts. Memory is
// capped at ~2x the cap; the cost is that a key can re-emit once after it ages
// out of both sets. That re-emission is a far better failure than the original's
// forever-growing filter, which silently swallows first-time lines once it
// saturates (see IMPROVEMENTS.md).
namespace {
class OnceFilter {
	std::mutex mtx;
	std::unordered_set<uint64_t> active;
	std::unordered_set<uint64_t> previous;
	size_t cap;

public:
	explicit OnceFilter(size_t cap_ = 8192) : cap(cap_) {}

	bool seen_or_add(uint64_t key) {
		std::lock_guard<std::mutex> lk(mtx);
		if (active.count(key) != 0 || previous.count(key) != 0) {
			return true;
		}
		if (active.size() >= cap) {
			previous = std::move(active);
			active.clear();
		}
		active.insert(key);
		return false;
	}
};
}  // namespace


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


// True once if stderr is an iTerm2 terminal, detected from the environment iTerm2
// exports. Gated on stderr being a tty so redirected output never gets escapes.
static bool
detect_iterm2()
{
	if (!is_tty()) {
		return false;
	}
	const char* tp = std::getenv("TERM_PROGRAM");
	if (tp != nullptr && std::string_view(tp) == "iTerm.app") {
		return true;
	}
	const char* lt = std::getenv("LC_TERMINAL");
	return lt != nullptr && std::string_view(lt) == "iTerm2";
}


// Minimal RFC 4648 base64, for the iTerm2 badge (which wants base64 text). Kept
// inline so the logger has no dependency beyond the scheduler stack.
static std::string
base64(std::string_view in)
{
	static const char tbl[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out;
	out.reserve(((in.size() + 2) / 3) * 4);
	size_t i = 0;
	for (; i + 3 <= in.size(); i += 3) {
		uint32_t n = (uint32_t(uint8_t(in[i])) << 16) | (uint32_t(uint8_t(in[i + 1])) << 8) | uint8_t(in[i + 2]);
		out += tbl[(n >> 18) & 63];
		out += tbl[(n >> 12) & 63];
		out += tbl[(n >> 6) & 63];
		out += tbl[n & 63];
	}
	if (i < in.size()) {
		bool two = (i + 1 < in.size());
		uint32_t n = uint32_t(uint8_t(in[i])) << 16;
		if (two) {
			n |= uint32_t(uint8_t(in[i + 1])) << 8;
		}
		out += tbl[(n >> 18) & 63];
		out += tbl[(n >> 12) & 63];
		out += two ? tbl[(n >> 6) & 63] : '=';
		out += '=';
	}
	return out;
}


// Write an iTerm2 escape sequence to stderr, only when iTerm2 is available+enabled.
static void
iterm2_emit(std::string_view seq)
{
	if (Logging::iterm2_available()) {
		write_all(STDERR_FILENO, seq.data(), seq.size());
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


// --- default hook implementations ---------------------------------------

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

static std::string
default_thread_name(std::thread::id id)
{
	std::ostringstream os;
	os << id;
	return os.str();
}

static std::string
default_describe_exception(std::exception_ptr e)
{
	if (!e) {
		return {};
	}
	try {
		std::rethrow_exception(e);
	} catch (const std::exception& ex) {
		return ex.what();
	} catch (...) {
		return "unknown exception";
	}
}

static std::string hooked_timestamp(std::chrono::system_clock::time_point tp) {
	return Logging::hooks.timestamp ? Logging::hooks.timestamp(tp) : default_timestamp(tp);
}
static std::string hooked_thread_name(std::thread::id id) {
	return Logging::hooks.thread_name ? Logging::hooks.thread_name(id) : default_thread_name(id);
}
static std::string hooked_describe_exception(std::exception_ptr e) {
	return Logging::hooks.describe_exception ? Logging::hooks.describe_exception(e) : default_describe_exception(e);
}
static std::string hooked_backtrace() {
	return Logging::hooks.backtrace ? Logging::hooks.backtrace() : std::string();
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


// --- sinks ---------------------------------------------------------------

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

Logging::Logging(std::string&& str_, bool clears_, bool async_, bool info_, uint64_t once_, int priority_,
		std::exception_ptr eptr_, const char* function_, const char* filename_, int line_,
		std::chrono::steady_clock::time_point created_at,
		std::chrono::system_clock::time_point timestamp_)
	: Base(created_at),
	  str(std::move(str_)),
	  priority(priority_),
	  clears(clears_),
	  async(async_),
	  info(info_),
	  once(once_),
	  eptr(std::move(eptr_)),
	  thread_id(std::this_thread::get_id()),
	  function(function_),
	  filename(filename_),
	  line(line_),
	  indent(log_indent()),
	  scheduled(false),
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
	if (scheduled) {
		pending.fetch_sub(1, std::memory_order_relaxed);
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
Logging::do_log(bool clears, std::chrono::steady_clock::time_point wakeup, bool async, bool info,
		uint64_t once, int priority, std::exception_ptr eptr, const char* function, const char* filename, int line,
		std::string&& str)
{
	if (priority <= config.log_level) {
		return add(wakeup, std::move(str), clears, async, info, once, priority, std::move(eptr), function, filename, line);
	}
	return Log();
}


Log
Logging::do_log(bool clears, std::chrono::steady_clock::time_point wakeup, bool async, int priority, std::string&& str)
{
	return do_log(clears, wakeup, async, true, 0, priority, std::exception_ptr{}, nullptr, nullptr, 0, std::move(str));
}


Log
Logging::add(std::chrono::steady_clock::time_point wakeup, std::string&& str, bool clears, bool async, bool info,
		uint64_t once, int priority, std::exception_ptr eptr, const char* function, const char* filename, int line,
		std::chrono::steady_clock::time_point created_at,
		std::chrono::system_clock::time_point timestamp)
{
	bool will_schedule = async || wakeup > std::chrono::steady_clock::now();

	// Backpressure: when the wheel is saturated, drop routine async lines (NOTICE
	// and below severity, non-deferred) rather than grow without bound. Severe and
	// deferred ("clears") lines are never dropped. The drop is counted and a
	// coalesced summary is emitted by the next line that fires.
	if (will_schedule && config.max_pending > 0
			&& pending.load(std::memory_order_relaxed) >= static_cast<long>(config.max_pending)
			&& !clears && priority >= LOG_NOTICE) {
		dropped.fetch_add(1, std::memory_order_relaxed);
		return Log();
	}

	auto entry = std::make_shared<Logging>(std::move(str), clears, async, info, once, priority,
		std::move(eptr), function, filename, line, created_at, timestamp);

	if (will_schedule) {
		// async ("now") and deferred ("now + delay") lines ride the wheel; the
		// LOG thread fires operator() and writes them.
		entry->scheduled = true;
		pending.fetch_add(1, std::memory_order_relaxed);
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
	// Report any lines dropped under backpressure since the last fire.
	if (auto d = dropped.exchange(0, std::memory_order_relaxed); d > 0) {
		log(LOG_WARNING, "dropped " + std::to_string(d) + " log lines (backpressure)");
	}

	// `once` dedup: emit only the first occurrence of this (message, token).
	if (once != 0) {
		static OnceFilter filter;
		uint64_t key = std::hash<std::string>{}(str) ^ (once * 0x9E3779B97F4A7C15ULL);
		if (filter.seen_or_add(key)) {
			return;
		}
	}

	std::string msg;
	if (info) {
		if (config.with_timestamp) {
			msg += hooked_timestamp(timestamp);
		}
		if (config.with_threads) {
			msg += '(';
			msg += hooked_thread_name(thread_id);
			msg += ") ";
		}
		if (config.with_location && function != nullptr) {
			msg += filename != nullptr ? filename : "";
			msg += ':';
			msg += std::to_string(line);
			msg += " at ";
			msg += function;
			msg += ": ";
		}
	}

	if (indent > 0) {
		msg.append(static_cast<size_t>(indent) * 2, ' ');
	}
	msg += str;

	if (eptr) {
		if (!str.empty()) {
			msg += ": ";
		}
		msg += hooked_describe_exception(eptr);
		auto bt = hooked_backtrace();
		if (!bt.empty()) {
			msg += '\n';
			msg += bt;
		}
	}

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
			add(now, std::move(unlog_str), false, async, info, 0, unlog_priority,
				std::exception_ptr{}, function, filename, line, atom_created_at.load(), timestamp);
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


// --- iTerm2 terminal integration -----------------------------------------
//
// iTerm2 proprietary escape codes (https://iterm2.com/documentation-escape-codes.html).
// Each is a no-op unless stderr is an iTerm2 terminal and config.iterm2 is set.

bool
Logging::iterm2_available()
{
	static const bool is_iterm = detect_iterm2();   // terminal type never changes mid-run
	return is_iterm && config.iterm2;               // but the host can toggle the flag
}

void
Logging::set_mark()
{
	iterm2_emit("\033]1337;SetMark\a");
}

void
Logging::tab_rgb(int red, int green, int blue)
{
	iterm2_emit(std::format(
		"\033]6;1;bg;red;brightness;{}\a\033]6;1;bg;green;brightness;{}\a\033]6;1;bg;blue;brightness;{}\a",
		red, green, blue));
}

void
Logging::tab_title(std::string_view title)
{
	iterm2_emit(std::format("\033]0;{}\a", title));
}

void
Logging::badge(std::string_view text)
{
	iterm2_emit(std::format("\033]1337;SetBadgeFormat={}\a", base64(text)));
}

void
Logging::growl(std::string_view text)
{
	iterm2_emit(std::format("\033]9;{}\a", text));
}

void
Logging::reset_iterm2()
{
	iterm2_emit("\033]1337;SetBadgeFormat=\a\033]6;1;bg;*;default\a");
}
