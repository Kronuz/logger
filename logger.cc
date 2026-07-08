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

#include "ansi_color.hh"   // term-color palette: rgb() / clear_color() (stacked escapes).
                           // ansi_color.hh, not colors.h: a consumer (e.g. Xapiand) may
                           // have its own colors.h on the include path; ansi_color.hh is
                           // unambiguous.
#include "collapse.hh"     // term_color::collapse / detect_depth / apply
#include "cppcodec/base64_rfc4648.hpp"   // base64 for the iTerm2 badge

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


// A severity marker: a colored block glyph followed by a reset, built from
// term-color's stacked escapes (16 / 256 / truecolor) so render() collapses it to
// whatever depth the terminal supports. The leading bar carries the level at a
// glance; its width tapers as severity drops.
static std::string
make_marker(std::string_view color, std::string_view glyph)
{
	std::string s(color);
	s += glyph;
	s += std::string_view(clear_color());
	return s;
}

// One marker per syslog priority (LOG_EMERG=0 .. LOG_DEBUG=7), plus a blank
// "verbose" slot. The palette matches Xapiand's long-standing severity colors. A
// host can still override any entry via config.markers.
static const std::string priorities[MAX_LOG_PRIORITY + 1] = {
	make_marker(std::string_view(rgb(238, 82, 83)),  "█"),  // LOG_EMERG   0
	make_marker(std::string_view(rgb(238, 82, 83)),  "▉"),  // LOG_ALERT   1
	make_marker(std::string_view(rgb(238, 82, 83)),  "▊"),  // LOG_CRIT    2
	make_marker(std::string_view(rgb(179, 57, 57)),  "▋"),  // LOG_ERR     3
	make_marker(std::string_view(rgb(255, 177, 66)), "▌"),  // LOG_WARNING 4
	make_marker(std::string_view(rgb(116, 185, 255)), "▍"), // LOG_NOTICE  5
	make_marker(std::string_view(rgb(63, 119, 179)), "▎"),  // LOG_INFO    6
	make_marker(std::string_view(rgb(105, 105, 105)), "▏"), // LOG_DEBUG   7
	"",                                                      // verbose   > 7
};


// Grey ramp for the timestamp gradient, as term-color stacked escapes (collapsed
// per-terminal by render()). default_timestamp() picks among these per digit group.
static const std::string kGrey60 {std::string_view(rgb( 60,  60,  60))};
static const std::string kGrey94 {std::string_view(rgb( 94,  94,  94))};
static const std::string kGrey162{std::string_view(rgb(162, 162, 162))};
static const std::string kGrey230{std::string_view(rgb(230, 230, 230))};


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


// Map the logger's public color config onto term-color's resolution layer, which
// owns the whole NO_COLOR / tty / mode / collapse policy.
static term_color::mode
tc_mode(LogColorMode m)
{
	switch (m) {
		case LogColorMode::always: return term_color::mode::always;
		case LogColorMode::never:  return term_color::mode::never;
		case LogColorMode::automatic:
		default:                   return term_color::mode::automatic;
	}
}

static term_color::target
tc_target(LogColorDepth d)
{
	switch (d) {
		case LogColorDepth::ansi16:    return term_color::target::ansi16;
		case LogColorDepth::ansi256:   return term_color::target::ansi256;
		case LogColorDepth::truecolor: return term_color::target::truecolor;
		case LogColorDepth::stacked:   return term_color::target::stacked;
		case LogColorDepth::automatic:
		default:                       return term_color::target::automatic;
	}
}

// Build the bytes a sink writes: optional priority marker, the message, optional
// newline, then hand the whole line to term-color to gate (mode / NO_COLOR / tty)
// and collapse (or strip, or pass the stack through) for this sink.
static std::string
render(int priority, std::string_view str, bool with_priority, bool with_endl, bool is_terminal)
{
	std::string buf;
	if (with_priority) {
		const std::string& m = Logging::config.markers[priority];
		buf += m.empty() ? priorities[priority] : m;
	}
	buf += str;
	if (with_endl) {
		buf += '\n';
	}
	return term_color::apply(buf, tc_mode(Logging::config.color),
		tc_target(Logging::config.color_depth), is_terminal);
}


// --- default hook implementations ---------------------------------------

// The decorated-line timestamp. Honors config.timestamp (the style),
// config.precision (the sub-second digits), and config.timestamp_gradient (whether
// to grey-ramp the digits with term-color escapes, collapsed per-terminal by
// render()). Three styles:
//   datetime  YYYYMMDDHHMMSS, gradient ramps up to the hour then back down
//   iso8601   YYYY-MM-DD HH:MM:SS, bright digits / dim separators
//   epoch     seconds since the epoch
// The returned string ends without a color reset; operator() appends one after the
// whole info decoration (so a trailing thread/location inherits the dim tail color,
// matching the original).
static std::string
default_timestamp(std::chrono::system_clock::time_point tp)
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

	// Dim, optional sub-second suffix (".mmm" / ".uuuuuu") per precision.
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
		// Bright digits (94), dim separators (60).
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

	// LogTimestamp::datetime -- compact YYYYMMDDHHMMSS, brightness ramps to the hour.
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
	// A file is never a terminal: color only when forced (config.color == always).
	auto buf = render(priority, str, with_priority, with_endl, /*is_terminal=*/false);
	write_all(fdout, buf.data(), buf.size());
}


void
StderrLogger::log(int priority, std::string_view str, bool with_priority, bool with_endl)
{
	auto buf = render(priority, str, with_priority, with_endl, /*is_terminal=*/is_tty());
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
	// syslog is never a terminal: color only when forced (config.color == always).
	auto buf = render(priority, str, with_priority, false, /*is_terminal=*/false);
	syslog(priority, "%s", buf.c_str());
}


// --- Logging -------------------------------------------------------------

Logging::Logging(std::string&& str_, bool clears_, bool async_, bool info_, uint64_t once_, int priority_,
		std::exception_ptr eptr_, std::source_location loc_,
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
	  loc(loc_),
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
	bool ok = scheduler().finish(wait);
	// Restore the terminal at shutdown: clear the iTerm2 badge / tab tint and pop
	// the saved title. (A no-op unless iTerm2 customizations were applied.) This was
	// part of the original logger's finish() and must not be lost on extraction.
	reset_iterm2();
	return ok;
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
		uint64_t once, int priority, std::exception_ptr eptr, std::source_location loc,
		std::string&& str)
{
	if (priority <= config.log_level) {
		return add(wakeup, std::move(str), clears, async, info, once, priority, std::move(eptr), loc);
	}
	return Log();
}


Log
Logging::do_log(bool clears, std::chrono::steady_clock::time_point wakeup, bool async, int priority, std::string&& str)
{
	// No call site for this convenience entry point: a default-constructed
	// source_location reads as line()==0, which the decorator treats as "no location".
	return do_log(clears, wakeup, async, true, 0, priority, std::exception_ptr{}, std::source_location{}, std::move(str));
}


Log
Logging::add(std::chrono::steady_clock::time_point wakeup, std::string&& str, bool clears, bool async, bool info,
		uint64_t once, int priority, std::exception_ptr eptr, std::source_location loc,
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
		std::move(eptr), loc, created_at, timestamp);

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
		std::string deco;
		if (config.with_timestamp) {
			deco += hooked_timestamp(timestamp);
		}
		if (config.with_threads) {
			deco += '(';
			deco += hooked_thread_name(thread_id);
			deco += ") ";
		}
		if (config.with_location && loc.line() != 0) {
			deco += loc.file_name();
			deco += ':';
			deco += std::to_string(loc.line());
			deco += " at ";
			deco += loc.function_name();
			deco += ": ";
		}
		if (!deco.empty()) {
			// Reset after the decoration so the gradient's dim tail (which a trailing
			// thread/location inherits) does not bleed into the message body. The
			// stacked reset is collapsed or stripped with everything else by render().
			msg += deco;
			msg += std::string_view(clear_color());
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
				std::exception_ptr{}, loc, atom_created_at.load(), timestamp);
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
	// Save the terminal's current title the first time we change it, so reset_iterm2()
	// can restore it at shutdown. \033[22;0t pushes the icon+window title onto the
	// terminal's title stack (xterm window op; iTerm2 implements it).
	static std::atomic<bool> saved{false};
	if (iterm2_available() && !saved.exchange(true)) {
		iterm2_emit("\033[22;0t");
	}
	iterm2_emit(std::format("\033]0;{}\a", title));
}

void
Logging::badge(std::string_view text)
{
	iterm2_emit(std::format("\033]1337;SetBadgeFormat={}\a", cppcodec::base64_rfc4648::encode(text)));
}

void
Logging::growl(std::string_view text)
{
	iterm2_emit(std::format("\033]9;{}\a", text));
}

void
Logging::reset_iterm2()
{
	// Clear the badge, reset the tab tint to the profile default, and pop the title
	// saved by the first tab_title() (\033[23;0t restores it from the title stack).
	iterm2_emit("\033]1337;SetBadgeFormat=\a\033]6;1;bg;*;default\a\033[23;0t");
}
