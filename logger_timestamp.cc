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

// The default decorated-line timestamp: the three styles, the grey gradient and the cached local time.

#include "logger.h"

#include "ansi_color.hh"   // term-color palette: rgb()

#include <cstdio>          // for snprintf
#include <cstdlib>         // for std::getenv
#include <cstring>         // for std::strlen, std::memcmp, std::memcpy
#include <ctime>           // for localtime_r, std::tm

// Grey ramp for the timestamp gradient. default_timestamp() picks among these per digit group.
namespace {
constexpr auto kGrey60 = rgb(60, 60, 60);
constexpr auto kGrey94 = rgb(94, 94, 94);
constexpr auto kGrey162 = rgb(162, 162, 162);
constexpr auto kGrey230 = rgb(230, 230, 230);
}  // namespace

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
	// Trivially destructible, so a line written after the thread's thread-locals are destroyed
	// (a static's destructor logging at exit) still finds it. A TZ value too long to keep is not cached.
	struct Cache {
		std::time_t sec = 0;
		bool valid = false;
		bool has_tz = false;
		std::size_t tz_len = 0;
		char tz[64];
		std::tm tm{};
	};
	static_assert(std::is_trivially_destructible_v<Cache>);
	static thread_local Cache c;
	const char* tz = std::getenv("TZ");
	const std::size_t len = tz != nullptr ? std::strlen(tz) : 0;
	if (!c.valid || c.sec != t || (tz != nullptr) != c.has_tz
			|| (tz != nullptr && (len != c.tz_len || std::memcmp(c.tz, tz, len) != 0))) {
		localtime_r(&t, &c.tm);
		c.sec = t;
		c.has_tz = tz != nullptr;
		c.valid = len < sizeof c.tz;
		c.tz_len = c.valid ? len : 0;
		if (c.valid && tz != nullptr) {
			std::memcpy(c.tz, tz, len);
		}
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
	auto g = [grad](std::string_view col) { return grad ? col : std::string_view(); };

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

	auto d2 = [&](int v, std::string_view col) {
		s += col;
		put_digits(s, static_cast<unsigned long>(v), 2);
	};
	auto d4 = [&](std::string_view col) {
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
