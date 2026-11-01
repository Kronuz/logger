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
Logging::Counter Logging::posted_[logger_detail::kStripes];
Logging::Counter Logging::completed_;
Logging::Counter Logging::dropped_;


// This thread's state, created on first use: threads take counter stripes in turn, so producers
// share a cache line of the lines-posted counters only when there are more of them than stripes.
logger_detail::ThreadState&
logger_detail::thread_state() noexcept
{
	static thread_local ThreadState state = [] {
		static std::atomic<unsigned> next{0};
		ThreadState s;
		s.id = std::this_thread::get_id();
		s.stripe = next.fetch_add(1, std::memory_order_relaxed) % kStripes;
		return s;
	}();
	return state;
}

Logging::Counter&
Logging::stripe() noexcept
{
	return posted_[logger_detail::thread_state().stripe];
}


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
	}
	std::uint64_t now_ns() const noexcept {
		return mach_continuous_time() * numer / denom;
	}
};
const MachClock mach_clock;
#endif
}  // namespace

std::chrono::steady_clock::time_point
logger_detail::steady_now() noexcept
{
#if defined(__APPLE__)
	if (mach_clock.usable) {
		return std::chrono::steady_clock::time_point(std::chrono::nanoseconds(static_cast<std::int64_t>(mach_clock.now_ns())));
	}
#endif
	return std::chrono::steady_clock::now();
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
		return wall;
	}
	return system_clock::time_point(duration_cast<system_clock::duration>(
		nanoseconds(n + clock_offset.offset.load(std::memory_order_relaxed))));
}




// Thread-local stacked-indent depth. Per-thread by construction, so it needs no
// lock and no shared map (unlike the original's mutex-guarded thread_id map).
int&
log_indent() noexcept
{
	return logger_detail::thread_state().indent;
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


// --- Logging -------------------------------------------------------------

static constexpr std::chrono::nanoseconds kCoalesce = 1ms;   // the wheel's resolution

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
	  thread_id(logger_detail::thread_state().id),
	  loc(loc_),
	  indent(logger_detail::thread_state().indent),
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
	: Base(created_at),
	  str(std::move(str_)),
	  priority(priority_),
	  clears(clears_),
	  async(async_),
	  info(info_),
	  once(once_),
	  thread_id(logger_detail::thread_state().id),
	  loc(loc_),
	  indent(logger_detail::thread_state().indent),
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
	// the saved title. (A no-op unless iTerm2 customizations were applied.)
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

	// A line on the wheel due within kCoalesce of now rides the wheel at now + kCoalesce:
	// the LOG thread then sleeps to that time, and the lines queued meanwhile are due
	// later than its wake time, so they need no signal. The shift is max(wakeup, now +
	// kCoalesce) for every scheduled line, `clears` ones included, which is non-decreasing
	// in both the due time and the call time: of two lines due in the order they were
	// logged, the first never lands in a later tick than the second, and the wheel keeps
	// insertion order within a tick. (Shifting only some kinds of line would let a line
	// due 0.3 ms ahead overtake an immediate line logged before it.) A line is written up
	// to kCoalesce after the call; finish() drains the wheel, so it is not lost there.
	const auto due_at = wakeup;
	const bool soon = will_schedule && wakeup <= now + kCoalesce;
	if (soon) {
		wakeup = now + kCoalesce;
	}

	// Backpressure: when the wheel is saturated, drop routine async lines (NOTICE
	// and below severity, non-deferred) rather than grow without bound. Severe and
	// deferred ("clears") lines are never dropped. The drop is counted and a
	// coalesced summary is emitted by the next line that fires.
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
		// async ("now") and deferred ("now + delay") lines ride the wheel; the
		// LOG thread fires operator() and writes them. They are handed to it with
		// one compare-exchange; it files them into the wheel itself.
		entry->scheduled = true;
		auto& counter = stripe();
		counter.n.fetch_add(1, std::memory_order_relaxed);
		// A line due within the tick of its call is due exactly kCoalesce after it, so those arrive in
		// due order and the LOG thread queues them without a wheel insert; the others are filed in the wheel.
		auto result = soon ? scheduler().post_ordered(entry, wakeup, now) : scheduler().post(entry, wakeup, now);
		if (result != schedule_result::scheduled) {
			// Refused (stopping, or past the 24 h horizon); undo the count. The
			// policy goes by the line's kind, not the reason.
			entry->scheduled = false;
			counter.n.fetch_sub(1, std::memory_order_relaxed);
			bool due = due_at <= std::chrono::steady_clock::now();
			if (clears && !due) {
				// A slow-work watchdog: never written now (that would log every fast
				// operation); its cancel point writes it only if the work was slow.
				entry->refused_deadline = due_at;
			} else if (due || priority < LOG_NOTICE) {
				// Async with no delay, or severe (what backpressure never drops):
				// write it now, synchronously.
				entry->Base::clear(true);   // fired: a later clean() keeps its swap
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
	// Report any lines dropped under backpressure since the last fire.
	if (dropped_.n.load(std::memory_order_relaxed) != 0) {
		if (auto d = dropped_.n.exchange(0, std::memory_order_relaxed); d > 0) {
			log(LOG_WARNING, "dropped " + std::to_string(d) + " log lines (backpressure)");
		}
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
	if (config.tint && priority >= 0 && priority < MAX_LOG_PRIORITY) { msg += tints[priority]; }
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
		auto late = refused_late();
		if (Base::clear(true)) {
			if (late) {
				// Refused by the scheduler, and the work outlasted the delay: write
				// it now, as the wheel would have, and keep the swap.
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
	if (!Base::clear(internal)) {
		return false;
	}
	if (late) {
		// A refused watchdog cancelled after its deadline: the wheel would have
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
