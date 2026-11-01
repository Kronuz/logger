// FormattedMsg (the macros' formatter, which renders into a per-thread reusable buffer)
// against format_msg (std::vformat into a fresh string): the same text for every format
// and argument list, including malformed formats, which render as the format string.
//
// Random formats built from valid and invalid pieces, random arguments of the common types,
// and the edges that matter to a reused buffer: very long messages, a buffer taken while
// another message is being formatted (a formatter that itself formats), a formatter that
// throws, and a message formatted after the thread's buffer has been freed.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <atomic>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <vector>

#include "logger.h"

#define CHECK(cond) do { \
	if (!(cond)) { \
		std::fprintf(stderr, "CHECK FAILED at line %d: %s\n", __LINE__, #cond); \
		std::abort(); \
	} \
} while (0)

// A type whose formatter formats (and so takes the thread's buffer while it is leased).
struct Nested {
	int v;
};
template <>
struct std::formatter<Nested> : std::formatter<std::string> {
	auto format(const Nested& n, std::format_context& ctx) const {
		logger_detail::FormattedMsg inner("<{}:{}>", n.v, n.v * 2);
		return std::formatter<std::string>::format(std::string(inner.view()), ctx);
	}
};

// A type whose formatter throws.
struct Boom {};
template <>
struct std::formatter<Boom> : std::formatter<std::string> {
	auto format(const Boom&, std::format_context&) const -> std::format_context::iterator {
		throw std::runtime_error("boom");
	}
};

static long checked = 0;

template <typename... Args>
static void same(std::string_view fmt, Args&&... args) {
	logger_detail::FormattedMsg got(fmt, args...);
	std::string want = format_msg(fmt, args...);
	if (got.view() != want) {
		std::fprintf(stderr, "MISMATCH for \"%.*s\":\n  got  \"%.*s\"\n  want \"%s\"\n", static_cast<int>(fmt.size()), fmt.data(),
			static_cast<int>(got.view().size()), got.view().data(), want.c_str());
		std::abort();
	}
	++checked;
}

static std::mt19937_64 rng(20261031);

static std::string random_text(size_t max_len) {
	static const char alphabet[] = "abcXYZ019 .,:;-_\"'\\\n\t\xc3\xa9";
	std::string s(rng() % (max_len + 1), ' ');
	for (auto& c : s) {
		c = alphabet[rng() % (sizeof(alphabet) - 1)];
	}
	return s;
}

static std::string random_format() {
	static const char* const pieces[] = {
		"{}", "{}", "{}", "{:>8}", "{:<6}", "{:^7}", "{:*^9}", "{:08.3f}", "{:.2f}", "{:x}", "{:#x}", "{:X}", "{:b}", "{:o}", "{:e}",
		"{:g}", "{:+}", "{: }", "{:5}", "{:.3}", "{:c}", "{:s}", "{:d}", "{:p}", "{0}", "{1}", "{2}", "{0:>4}", "{1:<4}",
		"{{", "}}", "{{}}", "{", "}", "{:z}", "{:", "{3}", "{x}", "{:08}", "{:.0f}", "{:10.4}",
	};
	std::string f;
	int parts = static_cast<int>(rng() % 6);
	for (int i = 0; i < parts; ++i) {
		f += rng() % 3 == 0 ? random_text(6) : pieces[rng() % (sizeof(pieces) / sizeof(pieces[0]))];
	}
	return f;
}

// Random values of the types the tests draw arguments from. A "pattern" is a list of types; the
// cases pick a pattern, fill it with random values, and render it with a random format, so the
// number of template instantiations stays linear in the number of patterns.
// Backing store for the string_views and char pointers: a ring, so the several of them in one
// pattern (at most four) never share a string.
static const std::string& next_text(size_t max_len) {
	static std::string ring[8];
	static unsigned next = 0;
	auto& s = ring[next++ % 8];
	s = random_text(max_len);
	return s;
}

template <typename T>
static T make() {
	// Integers stay small: pieces of a random format can join into a dynamic width ("{:" "{}" "}"), and
	// the next argument is then the width, so a large one asks for a string of gigabytes. (The fast
	// path's own test, below, covers the full range of every integer type.)
	if constexpr (std::is_same_v<T, int>) { return static_cast<int>(rng() % 40001) - 20000; }
	else if constexpr (std::is_same_v<T, unsigned long>) { return rng() % 20000; }
	else if constexpr (std::is_same_v<T, long long>) { return static_cast<long long>(rng() % 40001) - 20000; }
	else if constexpr (std::is_same_v<T, double>) { return static_cast<double>(static_cast<long long>(rng())) / static_cast<double>(1 + rng() % 100000); }
	else if constexpr (std::is_same_v<T, float>) { return static_cast<float>(rng() % 100000) / 7.0f; }
	else if constexpr (std::is_same_v<T, bool>) { return (rng() & 1) != 0; }
	else if constexpr (std::is_same_v<T, char>) { return static_cast<char>('a' + rng() % 26); }
	else if constexpr (std::is_same_v<T, const char*>) { return next_text(40).c_str(); }
	else if constexpr (std::is_same_v<T, std::string>) { return random_text(40); }
	else if constexpr (std::is_same_v<T, std::string_view>) { return next_text(40); }
	else if constexpr (std::is_same_v<T, const void*>) { return reinterpret_cast<const void*>(static_cast<uintptr_t>(rng() >> 16)); }
	else if constexpr (std::is_same_v<T, Nested>) { return Nested{static_cast<int>(rng() % 1000)}; }
	else if constexpr (std::is_same_v<T, std::nullptr_t>) { return nullptr; }
	else { static_assert(!sizeof(T), "no generator"); }
}

template <typename... Ts>
static void pattern(std::string_view fmt) {
	std::tuple<Ts...> values{make<Ts>()...};
	std::apply([&](auto&... a) { same(fmt, a...); }, values);
}

static void random_cases(int count) {
	using P = void (*)(std::string_view);
	static const P patterns[] = {
		&pattern<int>, &pattern<unsigned long>, &pattern<long long>, &pattern<double>, &pattern<float>, &pattern<bool>,
		&pattern<char>, &pattern<const char*>, &pattern<std::string>, &pattern<std::string_view>, &pattern<const void*>,
		&pattern<Nested>, &pattern<std::nullptr_t>,
		&pattern<int, const char*>, &pattern<double, std::string>, &pattern<std::string_view, int>, &pattern<bool, char>,
		&pattern<unsigned long, double>, &pattern<float, long long>, &pattern<Nested, int>, &pattern<const void*, bool>,
		&pattern<int, int, int>, &pattern<std::string, double, char>, &pattern<long long, std::string_view, float>,
		&pattern<const char*, Nested, bool>, &pattern<unsigned long, int, std::string>, &pattern<double, double, double>,
		&pattern<int, double, std::string, char>, &pattern<std::string_view, bool, unsigned long, float>,
		&pattern<const char*, const char*, int, long long>, &pattern<Nested, std::string, double, const void*>,
	};
	for (int i = 0; i < count; ++i) {
		patterns[rng() % (sizeof(patterns) / sizeof(patterns[0]))](random_format());
	}
}

static void edges() {
	int i = 42;
	const char* s = "text";
	// The messages the macros see most.
	same("value {} and {}", i, s);
	same("plain");
	same("a {{literal}} brace");        // no arguments: the format string as it is
	same("{} {{}}", i);                 // with arguments: escapes are processed
	same("{");                          // no arguments: as it is
	same("{", i);                       // malformed with arguments: the format string
	same("{:z}", i);
	// A width taken from an argument (the random formats keep their integers small: a random int as a
	// width asks for a string of up to 2 GB, which once took the CI runners' memory).
	int width = 9;
	same("{:{}}", i, width);
	same("{:>{}}|{}", s, width, i);
	same("{} {}", i);                   // too few arguments
	same("", i);
	same("{}", std::string());

	// Long messages: the buffer grows, and the next message is not affected.
	std::string big(100000, 'x');
	same("{}", big);
	same("short {}", i);
	same("{}{}{}", big, big, i);
	same("short again");
	std::string_view sv = big;
	same("{:>200000}", sv);
	same("after {}", i);
	same("{:>1500000}", sv);            // std::vformat's string passes 1 MiB: the thread gives it back
	same("after the wide one {}", i);
	// Past the buffer's 1 MiB ceiling the fast path gives up and std::vformat renders it.
	std::string giant(2'500'000, 'g');
	same("{}", giant);
	same("{} {}", giant, i);
	same("after the giant {}", i);

	// A formatter that formats while the outer message holds the thread's buffer.
	Nested n{7};
	same("outer {} {} outer", n, n);
	same("{}{}{}", n, i, n);

	// A formatter that throws: the format string, as format_msg gives it.
	Boom b;
	same("before {} after", b);
	same("{} {}", i, b);
	same("recovered {}", i);

	// Two live messages at once, the second in a buffer of its own.
	{
		logger_detail::FormattedMsg first("first {}", i);
		logger_detail::FormattedMsg second("second {}", i);
		CHECK(first.view() == "first 42");
		CHECK(second.view() == "second 42");
	}
	{
		logger_detail::FormattedMsg again("again {}", i);
		CHECK(again.view() == "again 42");
	}
}


// The fast path alone: whenever it accepts a message it writes what std::vformat writes.
static long fast_accepted = 0, fast_declined = 0;

template <typename... Args>
static void fast_same(std::string_view fmt, Args&&... args) {
	std::string out = "prefix that must not survive";
	out.clear();
	bool ok = logger_detail::fast_format_to(out, fmt, args...);
	if (!ok) {
		++fast_declined;
		return;
	}
	++fast_accepted;
	std::string want;
	try {
		want = std::vformat(fmt, std::make_format_args(args...));
	} catch (...) {
		std::fprintf(stderr, "fast path accepted a format std::vformat rejects: \"%.*s\"\n", static_cast<int>(fmt.size()), fmt.data());
		std::abort();
	}
	if (out != want) {
		std::fprintf(stderr, "FAST MISMATCH for \"%.*s\":\n  got  \"%s\"\n  want \"%s\"\n", static_cast<int>(fmt.size()), fmt.data(), out.c_str(), want.c_str());
		std::abort();
	}
}

static std::string fast_format_string() {
	static const char* const pieces[] = {"{}", "{}", "{}", "{}", "{{", "}}", "{{}}", "{}}", "{{{}", "{", "}", "{:}", "{0}", "{:>3}"};
	std::string f;
	int parts = static_cast<int>(rng() % 7);
	for (int i = 0; i < parts; ++i) {
		f += rng() % 3 == 0 ? random_text(5) : pieces[rng() % (sizeof(pieces) / sizeof(pieces[0]))];
	}
	return f;
}

template <typename T>
static T random_integer() {
	switch (rng() % 6) {
		case 0: return std::numeric_limits<T>::min();
		case 1: return std::numeric_limits<T>::max();
		case 2: return 0;
		case 3: return static_cast<T>(1 + rng() % 9);
		default: return static_cast<T>(rng()) >> (rng() % (sizeof(T) * 8 - 1));
	}
}

template <typename T>
static T fast_make() {
	if constexpr (std::is_same_v<T, bool>) { return (rng() & 1) != 0; }
	else if constexpr (std::is_same_v<T, char>) { return static_cast<char>(rng()); }
	else if constexpr (std::is_integral_v<T>) { return random_integer<T>(); }
	else if constexpr (std::is_same_v<T, std::string>) {
		std::string v = random_text(30);
		if (rng() % 4 == 0) { v.push_back('\0'); v += "tail"; }
		return v;
	}
	else if constexpr (std::is_same_v<T, std::string_view>) { return next_text(30); }
	else if constexpr (std::is_same_v<T, const char*>) { return next_text(30).c_str(); }
	else { static_assert(!sizeof(T), "no generator"); }
}

template <typename... Ts>
static void fast_pattern(std::string_view fmt) {
	std::tuple<Ts...> values{fast_make<Ts>()...};
	std::apply([&](auto&... a) { fast_same(fmt, a...); }, values);
}

// Arguments that are not values: a string literal, an empty one, a char pointer, an array with a NUL
// before its end (the fast path declines it).
static void fast_literal_cases(std::string_view fmt) {
	char buf[8] = "abcdefg";
	const char embedded[5] = {'a', '\0', 'b', 'c', '\0'};
	int i = static_cast<int>(rng());
	fast_same(fmt, "literal");
	fast_same(fmt, "");
	fast_same(fmt, embedded);
	fast_same(fmt, "x", i);
	fast_same(fmt, i, "x");
	char* p = buf;
	fast_same(fmt, p, i);
	fast_same(fmt, "x", p, i, "");
}

static void fast_random_cases(int count) {
	using P = void (*)(std::string_view);
	static const P patterns[] = {
		&fast_pattern<bool>, &fast_pattern<char>, &fast_pattern<signed char>, &fast_pattern<unsigned char>, &fast_pattern<short>,
		&fast_pattern<unsigned short>, &fast_pattern<int>, &fast_pattern<unsigned>, &fast_pattern<long>, &fast_pattern<unsigned long>,
		&fast_pattern<long long>, &fast_pattern<unsigned long long>, &fast_pattern<std::string>, &fast_pattern<std::string_view>,
		&fast_pattern<const char*>,
		&fast_pattern<int, int>, &fast_pattern<int, std::string>, &fast_pattern<std::string_view, unsigned long>,
		&fast_pattern<long long, bool>, &fast_pattern<char, const char*>, &fast_pattern<unsigned char, signed char>,
		&fast_pattern<short, unsigned short, int>, &fast_pattern<std::string, std::string, long>,
		&fast_pattern<bool, bool, char>, &fast_pattern<unsigned long long, long long, std::string_view>,
		&fast_pattern<int, long, std::string, const char*>, &fast_pattern<char, bool, unsigned, std::string_view>,
		&fast_pattern<std::string, int, int, int>,
	};
	for (int i = 0; i < count; ++i) {
		std::string fmt = fast_format_string();
		if (i % 7 == 0) {
			fast_literal_cases(fmt);
		} else {
			patterns[rng() % (sizeof(patterns) / sizeof(patterns[0]))](fmt);
		}
	}
}

// The messages the fast path exists for must take it.
static void fast_must_accept() {
	int i = 42;
	long long big = -9223372036854775807LL - 1;
	std::string s = "text";
	std::string_view sv = "view";
	const char* c = "cstr";
	std::string out;
	auto accepts = [&](std::string_view fmt, auto&... a) {
		out.clear();
		CHECK(logger_detail::fast_format_to(out, fmt, a...));
		CHECK(out == std::vformat(fmt, std::make_format_args(a...)));
	};
	accepts("value {} and {}", i, "x");
	accepts("{} {} {} {}", i, big, s, sv);
	bool yes = true;
	char z = 'z';
	accepts("{}:{} {}", c, i, yes);
	accepts("{{{}}}", i);
	accepts("{} {}", s, z);
	accepts("{} then {{ and }}", i);
	// ...and the ones it must decline.
	auto declines = [&](std::string_view fmt, auto&... a) {
		out.clear();
		CHECK(!logger_detail::fast_format_to(out, fmt, a...));
	};
	double d = 1.5;
	const char* null = nullptr;
	declines("{:>5}", i);
	declines("{0}", i);
	declines("{} {}", i);
	declines("{", i);
	declines("}", i);
	declines("{x}", i);
	declines("{}", null);
	(void)d;
}

// A thread-local whose destructor logs runs after the thread's buffer has been freed.
struct LateLogger {
	static inline std::atomic<int> ran{0};
	~LateLogger() {
		int v = 5;
		logger_detail::FormattedMsg m("late {}", v);
		CHECK(m.view() == "late 5");
		ran.fetch_add(1);
	}
};

static void late_thread_exit() {
	for (int round = 0; round < 50; ++round) {
		std::thread t([] {
			static thread_local LateLogger late;   // constructed before the buffer exists, so destroyed after it
			(void)late;
			int v = 1;
			logger_detail::FormattedMsg m("early {}", v);
			CHECK(m.view() == "early 1");
		});
		t.join();
	}
	CHECK(LateLogger::ran.load() == 50);
}

static void many_threads() {
	std::vector<std::thread> ts;
	for (int t = 0; t < 8; ++t) {
		ts.emplace_back([t] {
			for (int k = 0; k < 5000; ++k) {
				int a = k, b = t;
				logger_detail::FormattedMsg m("thread {} line {}", b, a);
				std::string want = std::format("thread {} line {}", b, a);
				CHECK(m.view() == want);
			}
		});
	}
	for (auto& t : ts) {
		t.join();
	}
}

int main() {
	edges();
	fast_must_accept();
	fast_random_cases(400000);
	CHECK(fast_accepted > 50000);
	// Most random formats are invalid for their arguments, and each one throws and catches twice (a
	// std::format_error costs about 50 us on macOS): 50000 cases take a few seconds there.
	random_cases(50000);
	late_thread_exit();
	many_threads();
	std::printf("logger format OK: %ld messages match format_msg; the fast path took %ld and declined %ld, all equal to std::vformat\n", checked, fast_accepted, fast_declined);
	return 0;
}
