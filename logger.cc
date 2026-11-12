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
#include <algorithm>       // for std::push_heap, std::pop_heap, std::make_heap
#include <condition_variable>   // for std::condition_variable
#include <limits>          // for std::numeric_limits
#include <mutex>           // for std::mutex, std::lock_guard
#include <sstream>         // for std::ostringstream
#include <system_error>   // for std::system_error
#include <type_traits>    // for std::is_same_v, std::decay_t
#include <unordered_set>   // for std::unordered_set
#include <unistd.h>        // for write, isatty, close, STDERR_FILENO
#if defined(__APPLE__)
#include <mach/mach_time.h>   // for mach_continuous_time, mach_timebase_info
#endif

using namespace std::chrono_literals;


LogConfig Logging::config;
LogHooks Logging::hooks;
std::vector<std::unique_ptr<Logger>> Logging::handlers;
Logging::Counter Logging::completed_;
Logging::Counter Logging::dropped_;


namespace {
// Frees the thread's message buffers when the thread ends; a log call made after that (from
// another thread-local's destructor) formats into its own string.
struct ScratchOwner {
	~ScratchOwner() {
		auto& slot = logger_detail::scratch_slot;
		delete[] slot.buf;
		delete slot.text;
		slot.buf = nullptr;
		slot.cap = 0;
		slot.text = nullptr;
		slot.dead = true;
	}
};
constexpr std::size_t kScratchStart = 512;
constexpr std::size_t kScratchMax = std::size_t{1} << 20;
}  // namespace

bool
logger_detail::scratch_init() noexcept
{
	static thread_local ScratchOwner owner;
	(void)owner;
	if (scratch_slot.dead) {
		return false;
	}
	try {
		auto* text = new std::string();
		text->reserve(256);
		scratch_slot.text = text;
		scratch_slot.buf = new char[kScratchStart];
		scratch_slot.cap = kScratchStart;
	} catch (...) {
		delete scratch_slot.text;
		scratch_slot.text = nullptr;
		scratch_slot.buf = nullptr;
		scratch_slot.cap = 0;
		return false;
	}
	return true;
}

char*
logger_detail::scratch_grow() noexcept
{
	if (scratch_slot.cap >= kScratchMax) {
		return nullptr;
	}
	try {
		auto* bigger = new char[scratch_slot.cap * 2];
		delete[] scratch_slot.buf;
		scratch_slot.buf = bigger;
		scratch_slot.cap *= 2;
	} catch (...) {
		return nullptr;
	}
	return scratch_slot.buf;
}

namespace {
#if defined(__APPLE__)
// mach_continuous_time in nanoseconds: the clock behind steady_clock::now() here (CLOCK_MONOTONIC_RAW).
struct MachClock {
	std::uint32_t numer = 1, denom = 1;
	bool usable = false;
	std::int64_t delta = 0;   // steady_clock less this clock, when they do not agree (usable is false)
	MachClock() {
		mach_timebase_info_data_t tb;
		if (mach_timebase_info(&tb) != KERN_SUCCESS || tb.numer == 0 || tb.denom == 0) {
			return;
		}
		numer = tb.numer;
		denom = tb.denom;
		// Agree with steady_clock to a millisecond, or leave it alone.
		auto before = std::chrono::steady_clock::now();
		auto mine = now_ns();
		auto after = std::chrono::steady_clock::now();
		auto lo = std::chrono::duration_cast<std::chrono::nanoseconds>(before.time_since_epoch()).count() - 1'000'000;
		auto hi = std::chrono::duration_cast<std::chrono::nanoseconds>(after.time_since_epoch()).count() + 1'000'000;
		usable = static_cast<std::int64_t>(mine) >= lo && static_cast<std::int64_t>(mine) <= hi;
		if (!usable) {
			delta = (lo + hi) / 2 - static_cast<std::int64_t>(mine);
		}
	}
	std::uint64_t ns(std::uint64_t ticks) const noexcept {
		return ticks * numer / denom;
	}
	std::uint64_t now_ns() const noexcept {
		return ns(mach_continuous_time());
	}
};
// Made on first use, so a line logged while another file's statics are initialized finds it.
const MachClock& mach_clock() {
	static const MachClock clock;
	return clock;
}
#endif
}  // namespace

std::chrono::steady_clock::time_point
logger_detail::steady_now() noexcept
{
#if defined(__APPLE__)
	if (const auto& c = mach_clock(); c.usable) {
		return std::chrono::steady_clock::time_point(std::chrono::nanoseconds(static_cast<std::int64_t>(c.now_ns())));
	}
#endif
	return std::chrono::steady_clock::now();
}

// A stamp as a steady_clock reading. On macOS a stamp counts mach_continuous_time, the clock behind
// steady_clock, so the conversion is the one steady_clock makes; were they ever to disagree (checked
// at first use), the offset measured then is added.
std::chrono::steady_clock::time_point
logger_detail::stamp_time(std::uint64_t stamp) noexcept
{
#if defined(__APPLE__)
	const auto& c = mach_clock();
	return std::chrono::steady_clock::time_point(std::chrono::nanoseconds(static_cast<std::int64_t>(c.ns(stamp)) + c.delta));
#else
	return std::chrono::steady_clock::time_point(std::chrono::nanoseconds(static_cast<std::int64_t>(stamp)));
#endif
}

namespace {
// system - steady, in nanoseconds, as last measured, and the steady reading it was measured at.
// Separate atomics on one line: producers read them on every line and write them every 20 ms; a
// pair torn between two measurements is as good as either.
struct alignas(64) ClockOffset {
	std::atomic<std::int64_t> offset{0};
	std::atomic<std::int64_t> at{std::numeric_limits<std::int64_t>::min()};
};
ClockOffset clock_offset;
constexpr std::int64_t kClockResync = 20'000'000;   // ns
}  // namespace

std::chrono::system_clock::time_point
logger_detail::system_at(std::chrono::steady_clock::time_point now)
{
	using namespace std::chrono;
	const std::int64_t n = duration_cast<nanoseconds>(now.time_since_epoch()).count();
	const std::int64_t at = clock_offset.at.load(std::memory_order_relaxed);
	// A reading a few ns older than the measurement (another thread measured meanwhile) is as good as one after it.
	const bool fresh = at != std::numeric_limits<std::int64_t>::min() && n - at < kClockResync && at - n < kClockResync;
	if (!fresh) {   // never measured, or the measurement is 20 ms old
		const auto before = logger_detail::steady_now();
		const auto wall = system_clock::now();
		const auto after = logger_detail::steady_now();
		const std::int64_t b = duration_cast<nanoseconds>(before.time_since_epoch()).count();
		const std::int64_t a = duration_cast<nanoseconds>(after.time_since_epoch()).count();
		const std::int64_t w = duration_cast<nanoseconds>(wall.time_since_epoch()).count();
		const std::int64_t mid = b + (a - b) / 2;
		clock_offset.offset.store(w - mid, std::memory_order_relaxed);
		clock_offset.at.store(mid, std::memory_order_relaxed);
		// The reading may be older than this measurement (the LOG thread converts a queued line's stamp).
		return system_clock::time_point(duration_cast<system_clock::duration>(nanoseconds(n + (w - mid))));
	}
	return system_clock::time_point(duration_cast<system_clock::duration>(
		nanoseconds(n + clock_offset.offset.load(std::memory_order_relaxed))));
}




// Thread-local stacked-indent depth. Per-thread by construction, so it needs no
// lock and no shared map (unlike the original's mutex-guarded thread_id map).
int&
log_indent() noexcept
{
	return logger_detail::thread_state.indent;
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


// The palette's colors alone, for config.tint.
static const std::string tints[MAX_LOG_PRIORITY] = {
	std::string(std::string_view(rgb(238, 82, 83))),   // LOG_EMERG
	std::string(std::string_view(rgb(238, 82, 83))),   // LOG_ALERT
	std::string(std::string_view(rgb(238, 82, 83))),   // LOG_CRIT
	std::string(std::string_view(rgb(179, 57, 57))),   // LOG_ERR
	std::string(std::string_view(rgb(255, 177, 66))),  // LOG_WARNING
	std::string(std::string_view(rgb(116, 185, 255))), // LOG_NOTICE
	std::string(std::string_view(rgb(63, 119, 179))),  // LOG_INFO
	std::string(std::string_view(rgb(105, 105, 105))), // LOG_DEBUG
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
// Appends v as exactly `width` zero-padded decimal digits (v < 10^width).
static void
put_digits(std::string& s, unsigned long v, int width)
{
	char b[20];
	for (int i = width; i-- > 0; v /= 10) {
		b[i] = static_cast<char>('0' + v % 10);
	}
	s.append(b, static_cast<size_t>(width));
}

// Broken-down local time for the whole second t, cached per thread. localtime_r
// is paid once per second instead of per line; it is redone when the TZ value
// changes (macOS re-reads TZ on every localtime_r call, so the cache is keyed on it).
static const std::tm&
local_tm(std::time_t t)
{
	struct Cache {
		std::time_t sec = 0;
		bool valid = false;
		bool has_tz = false;
		std::string tz;
		std::tm tm{};
	};
	static thread_local Cache c;
	const char* tz = std::getenv("TZ");
	if (!c.valid || c.sec != t || (tz != nullptr) != c.has_tz || (tz != nullptr && c.tz != tz)) {
		localtime_r(&t, &c.tm);
		c.sec = t;
		c.valid = true;
		c.has_tz = tz != nullptr;
		c.tz.assign(tz != nullptr ? tz : "");
	}
	return c.tm;
}

std::string
logger_detail::default_timestamp(std::chrono::system_clock::time_point tp)
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
	const std::tm& tm = local_tm(t);

	// Field widths the integer path reproduces exactly; anything else (pre-epoch
	// fractions, years outside 0..9999, epochs past 10 digits) takes snprintf.
	const bool plain = us >= 0 && t >= 0 && t <= 9999999999L
		&& tm.tm_year + 1900 >= 0 && tm.tm_year + 1900 <= 9999;

	char b[32];
	std::string s;
	s.reserve(96);

	auto d2 = [&](int v, const std::string& col) {
		s += col;
		put_digits(s, static_cast<unsigned long>(v), 2);
	};
	auto d4 = [&](const std::string& col) {
		s += col;
		if (plain) {
			put_digits(s, static_cast<unsigned long>(tm.tm_year + 1900), 4);
		} else {
			std::snprintf(b, sizeof b, "%04d", tm.tm_year + 1900);
			s += b;
		}
	};
	auto num = [&](long v, int width, const char* fmt) {
		if (plain) {
			put_digits(s, static_cast<unsigned long>(v), width);
		} else {
			std::snprintf(b, sizeof b, fmt, v);
			s += b;
		}
	};

	// Dim, optional sub-second suffix (".mmm" / ".uuuuuu") per precision.
	auto frac = [&]() {
		if (cfg.precision == LogPrecision::microseconds) {
			s += g(kGrey60); s += '.'; num(us, 6, "%06ld");
		} else if (cfg.precision == LogPrecision::milliseconds) {
			s += g(kGrey60); s += '.'; num(us / 1000, 3, "%03ld");
		}
	};

	if (cfg.timestamp == LogTimestamp::epoch) {
		s += g(kGrey94);
		if (plain) {
			put_digits(s, static_cast<unsigned long>(t), 10);
		} else {
			std::snprintf(b, sizeof b, "%010ld", static_cast<long>(t));
			s += b;
		}
		frac();
		s += ' ';
		return s;
	}

	if (cfg.timestamp == LogTimestamp::iso8601) {
		// Bright digits (94), dim separators (60).
		d4(g(kGrey94));
		s += g(kGrey60); s += '-';
		d2(tm.tm_mon + 1, g(kGrey94));
		s += g(kGrey60); s += '-';
		d2(tm.tm_mday, g(kGrey94));
		s += g(kGrey60); s += ' ';
		d2(tm.tm_hour, g(kGrey94));
		s += g(kGrey60); s += ':';
		d2(tm.tm_min, g(kGrey94));
		s += g(kGrey60); s += ':';
		d2(tm.tm_sec, g(kGrey94));
		frac();
		s += ' ';
		return s;
	}

	// LogTimestamp::datetime -- compact YYYYMMDDHHMMSS, brightness ramps to the hour.
	d4(g(kGrey60));
	d2(tm.tm_mon + 1, g(kGrey94));
	d2(tm.tm_mday,    g(kGrey162));
	d2(tm.tm_hour,    g(kGrey230));
	d2(tm.tm_min,     g(kGrey162));
	d2(tm.tm_sec,     g(kGrey94));
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
	return Logging::hooks.timestamp ? Logging::hooks.timestamp(tp) : logger_detail::default_timestamp(tp);
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


// --- The per-thread queues and the LOG thread ------------------------------
//
// Each thread that logs owns a queue: a byte ring it writes records into (a routine line's message
// in place, or a reference to a full record) and publishes with one store of its write index. The
// LOG thread reads every queue, files deferred lines in a wheel by due time, and writes the lines
// in the order of their calls: it merges the queues by the clock reading each line was stamped with,
// and only lines stamped before it began a look, so a line is never written ahead of one that was
// logged before it and was not yet visible. See ARCHITECTURE.md.

namespace logger_detail {

// What a written line consists of, from a full record or from a queued one.
struct Emit {
	int priority;
	bool info;
	bool async;
	std::uint64_t once;
	std::string_view str;
	const std::exception_ptr* eptr;   // null: no exception
	std::thread::id thread_id;
	std::source_location loc;
	int indent;
	std::chrono::system_clock::time_point timestamp;
	std::chrono::nanoseconds age;     // for the " ~N" annotation of an async line
};

// A queued full record: a reference the queue holds. A line due within a tick of its call (`ordered`)
// is written in queue order at its due time; any other (a deferred line) is filed by due time.
struct TaskRecord {
	std::uint32_t size;      // sizeof(TaskRecord)
	std::uint8_t kind;       // kRecTask
	std::uint8_t ordered;
	std::uint8_t pad_[2];
	std::int64_t due;        // steady_clock nanoseconds
	std::int64_t created;    // steady_clock nanoseconds
	std::shared_ptr<Logging> task;
};
static_assert(sizeof(TaskRecord) % 8 == 0, "records are 8-byte aligned");

// The size and kind every record starts with.
static void record_head(const char* p, std::uint32_t& size, std::uint8_t& kind) noexcept {
	RecordHead h;
	std::memcpy(&h, p, sizeof(h));
	size = h.size;
	kind = h.kind;
}

// Pads from `p` to the end of the buffer, `size` bytes (at least 8): only the record head is written,
// whatever the room, never a whole record header.
static void write_wrap(char* p, std::uint64_t size) noexcept {
	RecordHead h{static_cast<std::uint32_t>(size), kRecWrap, {}};
	std::memcpy(p, &h, sizeof(h));
}

static std::int64_t steady_ns(std::chrono::steady_clock::time_point t) noexcept {
	return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
}

constexpr std::int64_t kTickNs = 1'000'000;   // the batching tick, kCoalesce
constexpr std::int64_t kWakeSlackNs = 8 * kTickNs;   // a push due within this of the sleep's end wakes the LOG thread

// A queued line's due time and call time, in steady_clock nanoseconds.
static void line_key(const LineRecord* l, std::int64_t& due, std::int64_t& created) noexcept {
	created = steady_ns(stamp_time(l->stamp));
	due = (l->flags & kLineAtOnce) != 0 ? created : created + kTickNs;
}

// One tick in stamp units, rounded up.
static std::uint64_t tick_stamps() noexcept {
#if defined(__APPLE__)
	static const std::uint64_t tick = [] {
		mach_timebase_info_data_t tb;
		if (mach_timebase_info(&tb) != KERN_SUCCESS || tb.numer == 0) {
			return std::uint64_t{1'000'000};
		}
		return (std::uint64_t{1'000'000} * tb.denom + tb.numer - 1) / tb.numer;
	}();
	return tick;
#else
	return 1'000'000;
#endif
}

// One buffer of a queue. The producer publishes its write index, the LOG thread its read index, each
// on its own cache line. `next` is the bigger buffer the producer moved to when this one filled up; it
// is set after the last write index published here.
struct RingBuf {
	alignas(kCacheLine) std::atomic<std::uint64_t> pub_w{0};
	alignas(kCacheLine) std::atomic<std::uint64_t> pub_r{0};
	std::atomic<RingBuf*> next{nullptr};
	std::uint64_t cap = 0;
	char* data = nullptr;

	static RingBuf* make(std::uint64_t cap) noexcept {
		auto* b = new (std::nothrow) RingBuf;
		if (b == nullptr) {
			return nullptr;
		}
		b->data = static_cast<char*>(::operator new(cap, std::align_val_t{64}, std::nothrow));
		if (b->data == nullptr) {
			delete b;
			return nullptr;
		}
		b->cap = cap;
		return b;
	}
	~RingBuf() {
		::operator delete(data, std::align_val_t{64});
	}
};

// Who a queue belongs to: a live thread, nobody since its thread ended (the LOG thread drains it, then
// frees it), nobody (a thread that logs next takes it), or every thread already exiting (a lock guards it).
enum : int { kOwned = 0, kOrphaned = 1, kFree = 2, kShared = 3 };
constexpr std::uint64_t kRingStart = 16 * 1024;   // a queue's first buffer, in bytes; it doubles when full

// A queue: the producer's half (what the thread-local block points to), then the LOG thread's.
struct Ring : RingProducer {
	RingBuf* wbuf = nullptr;                // the producer's buffer
	alignas(kCacheLine) RingBuf* rbuf = nullptr;   // the LOG thread's buffer
	std::uint64_t r = 0;                    // the LOG thread's read index in it
	std::thread::id tid;                    // the owning thread, set before its first record
	std::atomic<int> owner{kOwned};
	Ring* next_ring = nullptr;              // the registry, newest first; a queue is never unlinked or freed
};

std::atomic<Ring*> ring_list{nullptr};
std::mutex shared_ring_mutex;               // guards the producer's half of the shared queue
Ring* shared_ring = nullptr;

static Ring* new_ring(int owner) noexcept {
	auto* g = new (std::nothrow) Ring;
	if (g == nullptr) {
		return nullptr;
	}
	RingBuf* b = RingBuf::make(kRingStart);
	if (b == nullptr) {
		delete g;
		return nullptr;
	}
	g->wbuf = g->rbuf = b;
	g->data = b->data;
	g->mask = b->cap - 1;
	g->room = b->cap;
	g->pub_w = &b->pub_w;
	g->owner.store(owner, std::memory_order_relaxed);
	g->tid = std::this_thread::get_id();
	// seq_cst: a sleeping LOG thread's look at the queues sees the queue, or its producer sees it sleeping.
	Ring* head = ring_list.load(std::memory_order_relaxed);
	do {
		g->next_ring = head;
	} while (!ring_list.compare_exchange_weak(head, g, std::memory_order_seq_cst, std::memory_order_relaxed));
	return g;
}

// Releases the thread's queue when the thread ends: the LOG thread writes what is left in it and then
// lets another thread take it. A line logged after this (by another thread-local's destructor) goes
// through the shared queue.
struct RingRelease {
	~RingRelease() {
		auto& ts = thread_state;
		ts.dead = true;
		if (auto* g = static_cast<Ring*>(ts.ring)) {
			ts.ring = nullptr;
			g->owner.store(kOrphaned, std::memory_order_release);
		}
	}
};

// The LOG thread.
class LogWorker : public Thread<LogWorker, ThreadPolicyType::logging> {
	struct Deferred {
		std::int64_t due;
		std::int64_t created;
		Logging* record;   // filed: its wheel_hold keeps it alive until it is written or dropped
	};
	// A queue's next line: its key is (due, call time).
	struct Head {
		std::int64_t due;
		std::int64_t created;
		Ring* ring;
	};
	// Heap orders: the earliest first.
	static bool later(const Head& a, const Head& b) noexcept {
		return a.due != b.due ? a.due > b.due : a.created > b.created;
	}
	// Batch order: the earliest first.
	static bool earlier_deferred(const Deferred& a, const Deferred& b) noexcept {
		return a.due != b.due ? a.due < b.due : a.created < b.created;
	}

	std::mutex mtx;
	std::condition_variable cv;
	std::atomic<int> ending{-1};             // finish()'s countdown of passes; -1 until it begins
	std::atomic<bool> final_done{false};     // the last look at the queues is over
	// LOG thread only: deferred lines filed in a wheel by due time; a pass takes the due ones into
	// `batch`, sorted by (due, created), and merges it with the queues' lines.
	std::unique_ptr<wheel::timer_wheel> wheel_ = std::make_unique<wheel::timer_wheel>(
		static_cast<std::uint64_t>(steady_ns(std::chrono::steady_clock::now())));
	std::vector<Deferred> batch;
	std::size_t batch_at = 0;
	std::int64_t pass_now = std::numeric_limits<std::int64_t>::min();   // during a pass: its `now`
	bool hot = false;                        // LOG thread only: the last passes filed deferred lines
	std::uint64_t filed = 0;                 // LOG thread only: deferred lines filed so far
	std::vector<Head> heads;                 // LOG thread only: each queue's next line, during a pass
	long written = 0;                        // LOG thread only: queued lines written this pass
	std::int64_t next_due = std::numeric_limits<std::int64_t>::max();   // after a pass: the earliest line left

	// Moves `g`'s read index to its next line written in queue order: past wrap records and spent
	// buffers, filing the deferred tasks it meets. True with `h` set if there is one.
	bool advance(Ring& g, Head& h) {
		for (;;) {
			RingBuf* b = g.rbuf;
			if (g.r == b->pub_w.load(std::memory_order_acquire)) {
				RingBuf* n = b->next.load(std::memory_order_acquire);
				if (n == nullptr) {
					return false;
				}
				if (g.r != b->pub_w.load(std::memory_order_acquire)) {
					continue;   // the producer's last records here, published before it moved on
				}
				g.rbuf = n;
				g.r = 0;
				delete b;
				continue;
			}
			char* p = b->data + (g.r & (b->cap - 1));
			std::uint32_t size;
			std::uint8_t kind;
			record_head(p, size, kind);
			if (kind == kRecWrap) {
				g.r += size;
				continue;
			}
			if (kind == kRecTask) {
				auto* t = reinterpret_cast<TaskRecord*>(p);
				if (t->ordered == 0) {
					file_deferred(std::move(t->task), t->due, t->created);
					t->~TaskRecord();
					g.r += size;
					continue;
				}
				h = {t->due, t->created, &g};
				return true;
			}
			std::int64_t due, created;
			line_key(reinterpret_cast<const LineRecord*>(p), due, created);
			h = {due, created, &g};
			return true;
		}
	}

	// Writes `g`'s next line (the one advance() found) and moves past it.
	void fire_head(Ring& g) {
		RingBuf* b = g.rbuf;
		char* p = b->data + (g.r & (b->cap - 1));
		std::uint32_t size;
		std::uint8_t kind;
		record_head(p, size, kind);
		if (kind == kRecTask) {
			auto* t = reinterpret_cast<TaskRecord*>(p);
			std::shared_ptr<Logging> task = std::move(t->task);
			t->~TaskRecord();
			g.r += size;
			fire(task);
			return;
		}
		write_line(g, reinterpret_cast<const LineRecord*>(p));
		g.r += size;
		++written;
	}

	static void fire(std::shared_ptr<Logging>& task) {
		if (*task && task->clear(true)) {
			task->run();   // skips a task its handle cancelled meanwhile
		}
	}

	static void write_line(const Ring& g, const LineRecord* l) {
		Emit line{l->priority, (l->flags & kLineInfo) != 0, true, l->once,
			std::string_view(reinterpret_cast<const char*>(l) + sizeof(LineRecord), l->len), nullptr, g.tid, l->loc,
			l->indent, system_at(stamp_time(l->stamp)), std::chrono::nanoseconds(0)};
		Logging::emit(line);
	}

	// Files a deferred line in the wheel, or, already due during a pass, straight into its batch.
	void file_deferred(std::shared_ptr<Logging>&& task, std::int64_t due, std::int64_t created) {
		++filed;
		if (!wheel_) {
			return;   // after the last look: released
		}
		Logging& l = *task;
		l.wheel_hold = std::move(task);
		if (due <= pass_now) {
			Deferred d{due, created, &l};
			batch.insert(std::upper_bound(batch.begin() + static_cast<std::ptrdiff_t>(batch_at), batch.end(), d, earlier_deferred),
				d);
			return;
		}
		l.fire = [](wheel::timer_entry& e, bool) noexcept {   // the wheel's destruction: release it
			static_cast<Logging&>(e).wheel_hold.reset();
		};
		wheel_->arm(l, static_cast<std::uint64_t>(due));
	}

	// When the wheel next has due lines (the end of their 1 ms slot), or never.
	std::int64_t wheel_next() const noexcept {
		const std::uint64_t n = wheel_ ? wheel_->next() : wheel::timer_wheel::never;
		return n >= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ? std::numeric_limits<std::int64_t>::max()
			: static_cast<std::int64_t>(n);
	}

	// Drops cancelled lines from the front of the batch.
	void purge() {
		while (batch_at < batch.size() && !*batch[batch_at].record) {
			batch[batch_at++].record->wheel_hold.reset();
		}
	}

	// One look at the queues: writes, in due order (and call order within a due time), every queued
	// line stamped at or before `gate` and due by `ring_due`, and every deferred line due by `now`.
	// Leaves in `next_due` the earliest due time of what is left. True if it wrote or filed anything.
	// Only lines stamped before the look began are taken (`gate`), so a line is never written ahead of
	// one logged before it that was not yet visible.
	bool pass(std::int64_t gate, std::int64_t ring_due, std::int64_t now) {
		bool worked = false;
		std::int64_t later_head = std::numeric_limits<std::int64_t>::max();
		pass_now = now;
		batch.clear();
		batch_at = 0;
		if (wheel_) {
			wheel_->expire(static_cast<std::uint64_t>(now), [this](wheel::timer_entry& e) {
				Logging& l = static_cast<Logging&>(e);
				batch.push_back({static_cast<std::int64_t>(e.deadline), l.atom_created_at.load().time_since_epoch().count(), &l});
			});
			std::sort(batch.begin(), batch.end(), earlier_deferred);
		}
		heads.clear();
		auto* first = ring_list.load(std::memory_order_acquire);
		for (auto* g = first; g != nullptr; g = g->next_ring) {
			const auto before = filed;
			Head h;
			if (advance(*g, h)) {
				if (h.created <= gate && h.due <= ring_due) {
					heads.push_back(h);
				} else {
					later_head = std::min(later_head, h.due);
				}
			}
			worked |= filed != before;
		}
		std::make_heap(heads.begin(), heads.end(), later);
		for (;;) {
			purge();
			const bool ring = !heads.empty();
			const bool heap = batch_at < batch.size();
			if (!ring && !heap) {
				break;
			}
			worked = true;
			bool take_ring = ring;
			if (ring && heap) {
				const auto& a = heads.front();
				const auto& d = batch[batch_at];
				take_ring = a.due != d.due ? a.due < d.due : a.created <= d.created;
			}
			if (take_ring) {
				std::pop_heap(heads.begin(), heads.end(), later);
				Ring& g = *heads.back().ring;
				heads.pop_back();
				fire_head(g);
				Head h;
				if (advance(g, h)) {
					if (h.created <= gate && h.due <= ring_due) {
						heads.push_back(h);
						std::push_heap(heads.begin(), heads.end(), later);
					} else {
						later_head = std::min(later_head, h.due);
					}
				}
			} else {
				auto task = std::move(batch[batch_at++].record->wheel_hold);
				fire(task);
			}
		}
		for (auto* g = first; g != nullptr; g = g->next_ring) {
			g->rbuf->pub_r.store(g->r, std::memory_order_release);
			if (g->owner.load(std::memory_order_acquire) == kOrphaned && drained(*g)) {
				g->owner.store(kFree, std::memory_order_release);
			}
		}
		if (written != 0) {
			// release: whoever sees the count drained (acquire in pending_count) sees the writes.
			Logging::completed_.n.fetch_add(written, std::memory_order_release);
			written = 0;
		}
		batch.clear();
		batch_at = 0;
		pass_now = std::numeric_limits<std::int64_t>::min();
		next_due = std::min(later_head, wheel_next());
		return worked;
	}

	static bool drained(const Ring& g) noexcept {
		return g.r == g.rbuf->pub_w.load(std::memory_order_seq_cst) && g.rbuf->next.load(std::memory_order_seq_cst) == nullptr;
	}

	// Whether any queue holds a record (seq_cst: the half of the sleep handshake after kSleeping).
	static bool queued() noexcept {
		for (auto* g = ring_list.load(std::memory_order_seq_cst); g != nullptr; g = g->next_ring) {
			if (!drained(*g)) {
				return true;
			}
		}
		return false;
	}

	// The earliest due time among the queues' next lines and the deferred lines, filing the deferred
	// lines it meets (the second look before a sleep).
	std::int64_t earliest() {
		std::int64_t e = std::numeric_limits<std::int64_t>::max();
		for (auto* g = ring_list.load(std::memory_order_acquire); g != nullptr; g = g->next_ring) {
			Head h;
			if (advance(*g, h)) {
				e = std::min(e, h.due);
			}
		}
		e = std::min(e, wheel_next());   // a cancelled line only wakes it early
		return e;
	}

	// The last look: no queued line is left behind (a producer that queued one meanwhile sees kClosed
	// and writes it itself unless this look got it), deferred lines due by now are written, and the
	// rest are released.
	void close() {
		worker_state.fetch_or(kClosed, std::memory_order_seq_cst);
		const auto now = steady_ns(std::chrono::steady_clock::now());
		constexpr auto all = std::numeric_limits<std::int64_t>::max();
		while (pass(all, all, now)) {   // every queued line, and the deferred lines due by now
		}
		final_done.store(true, std::memory_order_release);
		wheel_.reset();   // releases what is still filed
	}

	LogWorker() = default;

public:
	// The LOG thread, started on first use. It is never destroyed, so a line logged while statics are
	// destroyed still finds it (stopped, so the line is written on the calling thread); at exit it is
	// stopped as Logging::finish(1) would.
	static LogWorker& get() {
		static LogWorker* worker = [] {
			auto* w = new LogWorker;
			w->run();
			return w;
		}();
		static struct Finisher {
			~Finisher() {
				worker->end(1);
				worker->join(200ms);
			}
		} finisher;
		(void)finisher;
		return *worker;
	}

	const std::string& name() const noexcept {
		static const std::string n = "LOG";
		return n;
	}

	// finish(): stop taking lines, and count down `wait` passes (each up to 100 ms) to write what is due.
	void end(int wait) noexcept {
		ending.store(wait);
		worker_state.fetch_or(kStopping, std::memory_order_seq_cst);
		std::lock_guard<std::mutex> lk(mtx);
		cv.notify_all();
	}

	// Wakes the LOG thread if it sleeps past `due` (steady_clock nanoseconds).
	void wake_for(std::int64_t due) noexcept {
		// Slack: a timed wait wakes late, and a line due just after the wake would be filed already due.
		if (due - kWakeSlackNs < worker_wake.load(std::memory_order_relaxed)
				&& (worker_state.fetch_and(~kSleeping, std::memory_order_seq_cst) & kSleeping) != 0) {
			std::lock_guard<std::mutex> lk(mtx);
			cv.notify_one();
		}
	}

	// After a producer saw kClosed: once the last look is over, whether it took the record at `at`.
	static bool taken(const Ring& g, std::uint64_t at) noexcept {
		auto& w = get();
		while (!w.final_done.load(std::memory_order_acquire)) {
			std::this_thread::yield();
		}
		return g.rbuf == g.wbuf && g.r > at;
	}

	// A line the LOG thread did not take: the producer writes it.
	static void write_untaken(const Ring& g, std::uint64_t at) {
		write_line(g, reinterpret_cast<const LineRecord*>(g.data + (at & g.mask)));
		Logging::completed_.n.fetch_add(1, std::memory_order_release);
	}

	// Queues a full record on the calling thread's queue, due at `due` (steady_clock nanoseconds; in
	// queue order if `ordered`, by due time otherwise). False if it was refused (finish() has begun, or
	// no memory): the caller handles the line by its kind.
	static bool push_task(const std::shared_ptr<Logging>& task, std::int64_t due, std::int64_t created, bool ordered) noexcept {
		if ((worker_state.load(std::memory_order_relaxed) & kStopping) != 0) {
			return false;
		}
		auto& ts = thread_state;
		Ring* g = static_cast<Ring*>(ts.ring);
		std::unique_lock<std::mutex> lk;
		if (g == nullptr && !ts.dead) {
			g = static_cast<Ring*>(ring_attach());
		}
		if (g == nullptr) {
			lk = std::unique_lock<std::mutex>(shared_ring_mutex);
			if (shared_ring == nullptr) {
				get();
				shared_ring = new_ring(kShared);
			}
			g = shared_ring;
			if (g == nullptr) {
				return false;
			}
		}
		constexpr std::size_t need = sizeof(TaskRecord);
		char* p = g->data + (g->w & g->mask);
		const std::uint64_t to_end = g->mask + 1 - (g->w & g->mask);
		if (g->room - g->w < need || to_end < need) {
			p = ring_room(*g, need);
			if (p == nullptr) {
				return false;
			}
		}
		auto* rec = new (p) TaskRecord{static_cast<std::uint32_t>(need), kRecTask, static_cast<std::uint8_t>(ordered ? 1 : 0), {}, due, created, task};
		const std::uint64_t at = g->w;
		g->w = at + need;
		g->posted.store(g->posted.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
		g->pub_w->store(g->w, std::memory_order_seq_cst);
		const unsigned st = worker_state.load(std::memory_order_seq_cst);
		if ((st & kClosed) != 0 && !taken(*g, at)) {
			// The LOG thread closed without it: take it back, and refuse.
			rec->~TaskRecord();
			g->posted.store(g->posted.load(std::memory_order_relaxed) - 1, std::memory_order_relaxed);
			return false;
		}
		if ((st & kSleeping) != 0) {
			get().wake_for(due);
		}
		return true;
	}

	// The LOG thread's loop.
	void operator()() {
		using namespace std::chrono;
		for (;;) {
			if ((worker_state.load(std::memory_order_relaxed) & kStopping) != 0) {
				// finish(): each pass writes what is due, then waits up to 100 ms for what comes due next;
				// the loop ends when nothing is left or the countdown runs out.
				for (auto e = ending.load(); e > 0 && !ending.compare_exchange_weak(e, e - 1);) {
				}
				for (;;) {
					const auto t = steady_ns(steady_clock::now());
					if (!pass(t, t, t)) {
						break;
					}
				}
				if (ending.load() == 0 || (!queued() && (!wheel_ || wheel_->size() == 0))) {
					break;
				}
				const auto until = std::min(next_due, steady_ns(steady_clock::now() + 100ms));
				std::unique_lock<std::mutex> lk(mtx);
				cv.wait_until(lk, steady_clock::time_point(nanoseconds(until)), [&] { return ending.load() == 0; });
				continue;
			}
			const auto t = steady_ns(steady_clock::now());
			const auto filed_before = filed;
			if (pass(t, t, t)) {
				hot |= filed != filed_before;
				continue;   // it wrote or filed lines: look again at once
			}
			// Nothing due: sleep until the earliest line it knows of. A producer that queues one due
			// earlier sees kSleeping (or this second look sees its line) and wakes it.
			std::unique_lock<std::mutex> lk(mtx);
			if ((worker_state.load(std::memory_order_relaxed) & kStopping) != 0) {
				continue;
			}
			// After filing lines, sleep at most one tick: more are likely in the queues, and filed late
			// they would be due already.
			const auto until = hot ? std::min(next_due, t + kTickNs) : next_due;
			hot = false;
			worker_wake.store(until, std::memory_order_relaxed);
			worker_state.fetch_or(kSleeping, std::memory_order_seq_cst);
			std::atomic_thread_fence(std::memory_order_seq_cst);
			if (earliest() >= until) {
				auto awake = [&] { return (worker_state.load(std::memory_order_seq_cst) & (kSleeping | kStopping)) != kSleeping; };
				if (until == std::numeric_limits<std::int64_t>::max()) {
					cv.wait(lk, awake);
				} else {
					cv.wait_until(lk, steady_clock::time_point(nanoseconds(until)), awake);
				}
			}
			worker_state.fetch_and(~kSleeping, std::memory_order_seq_cst);
		}
		close();
	}
};

}  // namespace logger_detail


logger_detail::RingProducer*
logger_detail::ring_attach() noexcept
{
	auto& ts = thread_state;
	if (ts.dead) {
		return nullptr;
	}
	LogWorker::get();   // the LOG thread runs before the first line is queued
	Ring* g = nullptr;
	for (auto* x = ring_list.load(std::memory_order_acquire); x != nullptr; x = x->next_ring) {
		int f = kFree;
		if (x->owner.load(std::memory_order_relaxed) == kFree && x->owner.compare_exchange_strong(f, kOwned, std::memory_order_acquire)) {
			g = x;
			g->tid = std::this_thread::get_id();
			break;
		}
	}
	if (g == nullptr && (g = new_ring(kOwned)) == nullptr) {
		return nullptr;
	}
	static thread_local RingRelease release;
	(void)release;
	ts.tick = tick_stamps();
	ts.ring = g;
	return g;
}


std::size_t
logger_detail::queue_count() noexcept
{
	std::size_t n = 0;
	for (auto* g = ring_list.load(std::memory_order_acquire); g != nullptr; g = g->next_ring) {
		++n;
	}
	return n;
}


long
logger_detail::pending_count() noexcept
{
	return Logging::pending_count();
}


char*
logger_detail::ring_room(RingProducer& p, std::size_t need) noexcept
{
	auto& g = static_cast<Ring&>(p);
	need = (need + 7) & ~std::size_t{7};
	RingBuf* b = g.wbuf;
	g.room = b->pub_r.load(std::memory_order_acquire) + b->cap;
	const std::uint64_t off = g.w & g.mask;
	const std::uint64_t to_end = b->cap - off;
	const std::uint64_t free = g.room - g.w;
	if (to_end >= need && free >= need) {
		return g.data + off;
	}
	if (to_end < need && free >= to_end + need) {
		// Pad to the end of the buffer and start over at its beginning (published with the next record).
		write_wrap(g.data + off, to_end);
		g.w += to_end;
		return g.data;
	}
	// Full: move to a buffer at least twice as big and four times the record. The LOG thread reads this
	// one to its end, then follows `next`.
	std::uint64_t cap = b->cap * 2;
	while (cap < 4 * need) {
		cap *= 2;
	}
	RingBuf* nb = RingBuf::make(cap);
	if (nb == nullptr) {
		return nullptr;
	}
	b->next.store(nb, std::memory_order_seq_cst);
	g.wbuf = nb;
	g.data = nb->data;
	g.mask = cap - 1;
	g.w = 0;
	g.room = cap;
	g.pub_w = &nb->pub_w;
	return g.data;
}


void
logger_detail::ring_published(RingProducer& p, unsigned state, std::uint64_t at) noexcept
{
	auto& g = static_cast<Ring&>(p);
	if ((state & kClosed) != 0) {
		if (!LogWorker::taken(g, at)) {
			try {
				LogWorker::write_untaken(g, at);
			} catch (...) {
			}
		}
		return;
	}
	if ((state & kSleeping) != 0) {
		std::int64_t due, created;
		line_key(reinterpret_cast<const LineRecord*>(g.data + (at & g.mask)), due, created);
		LogWorker::get().wake_for(due);
	}
}


// --- Logging -------------------------------------------------------------

static constexpr std::chrono::nanoseconds kCoalesce = 1ms;   // the batching tick

Logging::Logging(std::string&& str_, bool clears_, bool async_, bool info_, uint64_t once_, int priority_,
		std::exception_ptr eptr_, std::source_location loc_,
		std::chrono::steady_clock::time_point created_at,
		std::chrono::system_clock::time_point timestamp_)
	: atom_created_at(created_at),
	  atom_cleared_at(std::chrono::steady_clock::time_point{}),
	  str(std::move(str_)),
	  priority(priority_),
	  clears(clears_),
	  async(async_),
	  info(info_),
	  atom_cancelled(false),
	  once(once_),
	  eptr(std::move(eptr_)),
	  thread_id(std::this_thread::get_id()),
	  loc(loc_),
	  indent(logger_detail::thread_state.indent),
	  scheduled(false),
	  timestamp(timestamp_),
	  atom_cleaned_at(std::chrono::steady_clock::time_point{}),
	  unlog_priority(0)
{
}

Logging::Logging(std::string&& str_, bool clears_, bool async_, bool info_, uint64_t once_, int priority_,
		std::source_location loc_,
		std::chrono::steady_clock::time_point created_at,
		std::chrono::system_clock::time_point timestamp_)
	: atom_created_at(created_at),
	  atom_cleared_at(std::chrono::steady_clock::time_point{}),
	  str(std::move(str_)),
	  priority(priority_),
	  clears(clears_),
	  async(async_),
	  info(info_),
	  atom_cancelled(false),
	  once(once_),
	  thread_id(std::this_thread::get_id()),
	  loc(loc_),
	  indent(logger_detail::thread_state.indent),
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
		// release: whoever sees the count drained (acquire in pending_count) sees the line's writes.
		completed_.n.fetch_add(1, std::memory_order_release);
	}
}


bool
Logging::finish(int wait)
{
	auto& worker = logger_detail::LogWorker::get();
	worker.end(wait);
	bool ok = wait == 0 || worker.join(std::chrono::milliseconds(2 * wait * 100));
	// Restore the terminal at shutdown: clear the iTerm2 badge / tab tint and pop
	// the saved title. (A no-op unless iTerm2 customizations were applied.)
	reset_iterm2();
	return ok;
}

bool
Logging::join(int timeout)
{
	return logger_detail::LogWorker::get().join(std::chrono::milliseconds(timeout));
}


long
Logging::pending_count() noexcept
{
	// `completed_` first: a line's count in its queue's `posted` comes before its publish, which the LOG
	// thread acquired before it completed the line (a release add), so every line counted in `done` has
	// its count visible to the reads below and the difference cannot fall short.
	long done = completed_.n.load(std::memory_order_acquire);
	long n = 0;
	for (auto* g = logger_detail::ring_list.load(std::memory_order_acquire); g != nullptr; g = g->next_ring) {
		n += g->posted.load(std::memory_order_relaxed);
	}
	n -= done;
	return n > 0 ? n : 0;
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
		auto now = std::chrono::steady_clock::now();
		return add(wakeup, std::move(str), clears, async, info, once, priority, eptr ? &eptr : nullptr, loc,
			now, std::chrono::system_clock::now(), now);
	}
	return Log();
}


Log
Logging::do_log_in(bool clears, std::chrono::steady_clock::duration delay, bool async, bool info,
		uint64_t once, int priority, logger_detail::NoException, std::source_location loc,
		const logger_detail::FormattedMsg& msg)
{
	if (priority <= config.log_level) {
		auto now = logger_detail::steady_now();
		return add(now + delay, msg.view(), clears, async, info, once, priority, nullptr, loc,
			now, logger_detail::system_at(now), now);
	}
	return Log();
}


Log
Logging::do_log_in(bool clears, std::chrono::steady_clock::duration delay, bool async, bool info,
		uint64_t once, int priority, std::exception_ptr eptr, std::source_location loc,
		const logger_detail::FormattedMsg& msg)
{
	if (priority <= config.log_level) {
		auto now = logger_detail::steady_now();
		return add(now + delay, msg.view(), clears, async, info, once, priority, eptr ? &eptr : nullptr, loc,
			now, logger_detail::system_at(now), now);
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
		uint64_t once, int priority, std::exception_ptr* eptr, std::source_location loc,
		std::chrono::steady_clock::time_point created_at,
		std::chrono::system_clock::time_point timestamp,
		std::chrono::steady_clock::time_point now)
{
	return add_line(wakeup, std::move(str), clears, async, info, once, priority, eptr, loc,
		created_at, timestamp, now);
}


Log
Logging::add(std::chrono::steady_clock::time_point wakeup, std::string_view str, bool clears, bool async, bool info,
		uint64_t once, int priority, std::exception_ptr* eptr, std::source_location loc,
		std::chrono::steady_clock::time_point created_at,
		std::chrono::system_clock::time_point timestamp,
		std::chrono::steady_clock::time_point now)
{
	return add_line(wakeup, str, clears, async, info, once, priority, eptr, loc,
		created_at, timestamp, now);
}


template <typename Msg>
Log
Logging::add_line(std::chrono::steady_clock::time_point wakeup, Msg&& str, bool clears, bool async, bool info,
		uint64_t once, int priority, std::exception_ptr* eptr, std::source_location loc,
		std::chrono::steady_clock::time_point created_at,
		std::chrono::system_clock::time_point timestamp,
		std::chrono::steady_clock::time_point now)
{
	bool will_schedule = async || wakeup > now;

	// A line due within kCoalesce of its call is written in queue order, batched: it is due at
	// now + kCoalesce, so the LOG thread sleeps to that time and the lines queued meanwhile need no
	// signal. The shift is max(wakeup, now + kCoalesce), non-decreasing in both the due time and the
	// call time, so of two lines due in the order they were logged the first is never due later.
	// A line that would start a batch is not held: when its thread queued no such line in the last
	// tick, nothing is pending (every line logged before it has been written or cancelled) and it is
	// not a `clears` line (whose handle needs the full tick to cancel it), it is due at its own time,
	// no earlier than its thread's clock, and the LOG thread is woken for it. The fast path in
	// logger.h applies the same rule; both keep the thread's last such stamp.
	const auto due_at = wakeup;
	const bool soon = will_schedule && wakeup <= now + kCoalesce;
	if (soon) {
		auto& ts = logger_detail::thread_state;
		const auto stamp = logger_detail::stamp_now();
		if (!clears && stamp - ts.last_soon >= ts.tick && pending_count() == 0) {
			const auto at = logger_detail::stamp_time(stamp);
			if (wakeup < at) {
				wakeup = at;
			}
		} else {
			wakeup = now + kCoalesce;
		}
		ts.last_soon = stamp;
	}

	// Backpressure: when the queues are saturated, drop routine async lines (NOTICE
	// and below severity, non-deferred) rather than grow without bound. Severe and
	// deferred ("clears") lines are never dropped. The drop is counted and a
	// coalesced summary is emitted by the next line that is written.
	if (will_schedule && config.max_pending > 0
			&& pending_count() >= static_cast<long>(config.max_pending)
			&& !clears && priority >= LOG_NOTICE) {
		dropped_.n.fetch_add(1, std::memory_order_relaxed);
		return Log();
	}

	std::shared_ptr<Logging> entry;
	if constexpr (std::is_same_v<std::decay_t<Msg>, std::string_view>) {
		entry = std::make_shared<Logging>(std::string(), clears, async, info, once, priority,
			loc, created_at, timestamp);
		entry->str.assign(str.data(), str.size());
	} else {
		entry = std::make_shared<Logging>(std::move(str), clears, async, info, once, priority,
			loc, created_at, timestamp);
	}
	if (eptr != nullptr) {
		entry->eptr = std::move(*eptr);
	}

	if (will_schedule) {
		// Async ("now") and deferred ("now + delay") lines go to the LOG thread through this thread's
		// queue, which keeps them in the order they were logged; the LOG thread writes a line due now
		// in that order and keeps a deferred one until it is due, unless its handle cancels it first.
		// A delay of a day or more is refused.
		entry->scheduled = true;
		const auto ns = [](std::chrono::steady_clock::time_point t) {
			return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
		};
		bool queued = wakeup - now < 24h && logger_detail::LogWorker::push_task(entry, ns(wakeup), ns(now), soon);
		if (!queued) {
			// Refused (stopping, or past the 24 h horizon). The policy goes by the line's kind,
			// not the reason.
			entry->scheduled = false;
			bool due = due_at <= std::chrono::steady_clock::now();
			if (clears && !due) {
				// A slow-work watchdog: never written now (that would log every fast
				// operation); its cancel point writes it only if the work was slow.
				entry->refused_deadline = due_at;
			} else if (due || priority < LOG_NOTICE) {
				// Async with no delay, or severe (what backpressure never drops):
				// write it now, synchronously.
				entry->claim(true);   // fired: a later clean() keeps its swap
				(*entry)();
			}
			// Else a routine deferred line that does not clear: lost, as before.
		}
	} else {
		// immediate logs run inline on the calling thread.
		(*entry)();
	}

	return Log(std::move(entry));
}


void
Logging::operator()()
{
	logger_detail::Emit line{priority, info, async, once, str, eptr ? &eptr : nullptr, thread_id, loc, indent,
		timestamp, async ? age() : std::chrono::nanoseconds(0)};
	emit(line);
}


void
Logging::emit(const logger_detail::Emit& line)
{
	// Report any lines dropped under backpressure since the last line written.
	if (dropped_.n.load(std::memory_order_relaxed) != 0) {
		if (auto d = dropped_.n.exchange(0, std::memory_order_relaxed); d > 0) {
			log(LOG_WARNING, "dropped " + std::to_string(d) + " log lines (backpressure)");
		}
	}

	// `once` dedup: emit only the first occurrence of this (message, token).
	if (line.once != 0) {
		static OnceFilter filter;
		uint64_t key = std::hash<std::string_view>{}(line.str) ^ (line.once * 0x9E3779B97F4A7C15ULL);
		if (filter.seen_or_add(key)) {
			return;
		}
	}

	std::string msg;
	if (line.info) {
		std::string deco;
		if (config.with_timestamp) {
			deco += hooked_timestamp(line.timestamp);
		}
		if (config.with_threads) {
			deco += '(';
			deco += hooked_thread_name(line.thread_id);
			deco += ") ";
		}
		if (config.with_location && line.loc.line() != 0) {
			deco += line.loc.file_name();
			deco += ':';
			deco += std::to_string(line.loc.line());
			deco += " at ";
			deco += line.loc.function_name();
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

	if (line.indent > 0) {
		msg.append(static_cast<size_t>(line.indent) * 2, ' ');
	}
	if (config.tint && line.priority >= 0 && line.priority < MAX_LOG_PRIORITY) { msg += tints[line.priority]; }
	msg += line.str;

	if (line.eptr != nullptr && *line.eptr) {
		if (!line.str.empty()) {
			msg += ": ";
		}
		msg += hooked_describe_exception(*line.eptr);
		auto bt = hooked_backtrace();
		if (!bt.empty()) {
			msg += '\n';
			msg += bt;
		}
	}

	if (line.async && line.age > 100ms) {
		msg += " ~";
		msg += format_delta(line.age);
	}

	log(line.priority, std::move(msg));
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
		auto late = refused_late();
		if (claim(true)) {
			if (late) {
				// Refused by the queue, and the work outlasted the delay: write
				// it now, as the LOG thread would have, and keep the swap.
				(*this)();
			} else {
				unlog_str.clear();
			}
		}
	}

	// Already cleaned (the handle's drop got here first): nothing to do, and no clock read.
	std::chrono::steady_clock::time_point c{};
	if (atom_cleaned_at.load(std::memory_order_relaxed) != c) {
		return;
	}
	auto now = logger_detail::steady_now();
	if (atom_cleaned_at.compare_exchange_strong(c, now)) {
		if (!unlog_str.empty() && unlog_priority <= config.log_level) {
			add(now, std::move(unlog_str), false, async, info, 0, unlog_priority,
				nullptr, loc, atom_created_at.load(), timestamp, now);
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
	auto late = refused_late();
	if (!claim(internal)) {
		return false;
	}
	if (late) {
		// A refused watchdog cancelled after its deadline: the LOG thread would have
		// written it by now, so write it (its swap stays cancelled).
		(*this)();
	}
	return true;
}


bool
Logging::refused_late() const
{
	return refused_deadline != std::chrono::steady_clock::time_point{} &&
	       std::chrono::steady_clock::now() >= refused_deadline;
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
	: entry(std::move(o.entry)),
	  swap(std::move(o.swap)),
	  stamp(std::exchange(o.stamp, 0)),
	  loc(o.loc),
	  info(o.info)
{
}

Log&
Log::operator=(Log&& o) noexcept
{
	if (!entry && swap) {
		drop();   // a queued line's swap is written when its handle goes, as a record's is when it is freed
	}
	entry = std::move(o.entry);
	o.entry.reset();
	swap = std::move(o.swap);
	stamp = std::exchange(o.stamp, 0);
	loc = o.loc;
	info = o.info;
	return *this;
}

void
Log::drop() noexcept
{
	try {
		if (entry) {
			entry->clean();
			return;
		}
		auto sw = std::move(swap);
		if (sw && stamp != 0 && sw->priority <= Logging::config.log_level) {
			// What clean() does for a record: the swap is a new line, with the queued line's call site,
			// stamp and wall-clock time.
			const auto created = logger_detail::stamp_time(stamp);
			const auto now = logger_detail::steady_now();
			Logging::add(now, std::move(sw->str), false, true, info, 0, sw->priority, nullptr, loc,
				created, logger_detail::system_at(created), now);
		}
	} catch (...) {
	}
}

void
Log::unlog(int priority, std::string str)
{
	if (entry) {
		entry->vunlog(priority, std::move(str));
	} else if (stamp != 0) {
		swap = std::make_unique<Swap>(Swap{priority, std::move(str)});
	}
}

bool
Log::clear()
{
	if (entry) {
		return entry->clear();
	}
	swap.reset();   // a queued line is already on its way: only its swap is cancelled
	return false;
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
	swap.reset();
	stamp = 0;
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
