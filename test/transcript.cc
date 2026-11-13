// A scripted run of the logger that prints every byte its sinks receive (a very long line as its head,
// its length and a hash). test/transcript.cmake requires the output to match test/transcript.golden,
// which the implementation before the producer path changed wrote byte for byte: the logger may change
// how a line travels to the LOG thread, never what is written.
//
// Everything is deterministic: one producer thread, a fixed timestamp, thread name and exception
// description, seeded random values, and a wait for the LOG thread to go idle between steps whose
// order would otherwise depend on timing (an inline line against an async one).
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

#include "logger.h"

using namespace std::chrono_literals;

struct Tape : Logger {
	std::mutex m;
	std::vector<std::string> rows;
	static std::string escape(std::string_view s) {
		std::string out;
		for (unsigned char c : s) {
			if (c >= 0x20 && c < 0x7f && c != '\\') {
				out += static_cast<char>(c);
			} else {
				char b[8];
				std::snprintf(b, sizeof b, "\\x%02x", c);
				out += b;
			}
		}
		if (out.size() > 2048) {   // a long line is kept as its head, its length and a hash of all of it
			std::uint64_t h = 0xcbf29ce484222325ull;
			for (unsigned char c : out) {
				h = (h ^ c) * 0x100000001b3ull;
			}
			char tail[64];
			std::snprintf(tail, sizeof tail, "...[%zu bytes fnv1a %016llx]", out.size(), static_cast<unsigned long long>(h));
			out = out.substr(0, 64) + tail;
		}
		return out;
	}
	// A line written 100 ms or more after it was logged ends with its age (" ~129.9ms"). Whether a line
	// is that late depends on how loaded the machine is (a starved LOG thread writes late), so the
	// comparison leaves the annotation out; "late 8", written well past 100 ms, must have it.
	static bool has_age(const std::string& s) {
		auto tilde = s.rfind(" ~");
		if (tilde == std::string::npos || s.size() < tilde + 5 || s.compare(s.size() - 2, 2, "ms") != 0) {
			return false;
		}
		for (size_t i = tilde + 2; i + 2 < s.size(); ++i) {
			if (!((s[i] >= '0' && s[i] <= '9') || s[i] == '.')) {
				return false;
			}
		}
		return true;
	}
	static std::string without_age(std::string s) {
		if (has_age(s)) {
			s.erase(s.rfind(" ~"));
		}
		return s;
	}
	// The call site as the compiler gave it depends on the build directory and the line moves with edits:
	// the file's name and "LINE" stand for them once the line is the one `located_line` names.
	std::atomic<int> located_line{0};
	std::string without_dirs(std::string s) const {
		const int line = located_line;
		const auto at = s.find("transcript.cc:" + std::to_string(line) + " ");
		if (line != 0 && at != std::string::npos) {
			const auto from = s.rfind(' ', at);
			const auto begin = from == std::string::npos ? 0 : from + 1;
			s.replace(begin, at + std::strlen("transcript.cc:") + std::to_string(line).size() - begin, "transcript.cc:LINE");
		}
		return s;
	}
	bool late_annotated = false;
	void log(int priority, std::string_view s, bool with_priority, bool with_endl) override {
		std::lock_guard<std::mutex> l(m);
		if (s.find("late 8") != std::string_view::npos && has_age(std::string(s))) {
			late_annotated = true;
		}
		rows.push_back(std::to_string(priority) + (with_priority ? " P" : " -") + (with_endl ? "E " : "- ") + escape(without_dirs(without_age(std::string(s)))));
	}
};

static Tape* tape;

// Waits (up to 10 s) until a line containing `text` has been written. The scenarios below need a deferred
// line to have fired before the next step, and a loaded machine can take longer than any fixed sleep (the
// old library, run first, was starved past 120 ms on a CI runner and wrote the line late).
static void wait_for_row(const char* text) {
	for (int i = 0; i < 10000; ++i) {
		{
			std::lock_guard<std::mutex> l(tape->m);
			for (const auto& r : tape->rows) {
				if (r.find(text) != std::string::npos) {
					return;
				}
			}
		}
		std::this_thread::sleep_for(1ms);
	}
}

static void settle() {
	for (int i = 0; i < 5000 && Logging::pending_count() > 0; ++i) {
		std::this_thread::sleep_for(1ms);
	}
	std::this_thread::sleep_for(4ms);
}

static void flush_step(const char* name) {
	settle();
	std::lock_guard<std::mutex> l(tape->m);
	std::printf("== %s (%zu)\n", name, tape->rows.size());
	for (auto& r : tape->rows) {
		std::printf("%s\n", r.c_str());
	}
	tape->rows.clear();
}

struct Printable {
	int v;
};
template <>
struct std::formatter<Printable> : std::formatter<std::string> {
	auto format(const Printable& p, std::format_context& ctx) const {
		return std::formatter<std::string>::format("P" + std::to_string(p.v), ctx);
	}
};

static void reset_config() {
	auto& c = Logging::config;
	c = LogConfig{};
	c.color = LogColorMode::never;
	c.log_level = LOG_DEBUG;
	c.with_timestamp = true;
	c.with_threads = false;
	c.with_location = false;
	c.tint = false;
	c.iterm2 = false;
}

int main() {
	auto t = std::make_unique<Tape>();
	tape = t.get();
	Logging::handlers.clear();
	Logging::handlers.push_back(std::move(t));
	Logging::hooks.timestamp = [](std::chrono::system_clock::time_point) { return std::string("TS "); };
	Logging::hooks.thread_name = [](std::thread::id) { return std::string("worker"); };
	Logging::hooks.describe_exception = [](std::exception_ptr e) {
		try {
			std::rethrow_exception(e);
		} catch (const std::exception& ex) {
			return std::string("[") + ex.what() + "]";
		} catch (...) {
			return std::string("[?]");
		}
	};
	Logging::hooks.backtrace = [] { return std::string(); };
	reset_config();

	// Every level, inline (EMERG..CRIT) and async (ERR..DEBUG).
	for (int p = 0; p <= 7; ++p) {
		L(p, "level {} says {}", p, "hello");
		settle();
	}
	flush_step("levels");

	// Format strings and arguments of every common kind, and the ones that fail.
	{
		int i = -42;
		unsigned long u = 18446744073709551615UL;
		double d = 3.14159265358979;
		float f = 2.5f;
		bool b = true;
		char c = 'q';
		const char* s = "c string";
		std::string str = "std string";
		std::string_view sv = "view";
		const void* ptr = nullptr;
		Printable pr{7};
		L_INFO("plain message");
		L_INFO("braces {{kept}} and {} {}", i, u);
		L_INFO("{:>8}|{:<8}|{:^8}|{:08.3f}|{:x}|{:#b}|{:e}", i, u, s, d, 255, 5, f);
		L_INFO("{} {} {} {} {} {} {} {} {}", b, c, s, str, sv, ptr, pr, d, f);
		L_INFO("{0} {1} {0} {2:.3}", s, i, d);
		L_INFO("{");                         // no arguments: logged as written
		L_INFO("{", i);                      // malformed with arguments: the format string
		L_INFO("{:z} {}", i, s);
		L_INFO("too {} few {}", i);
		L_INFO("{}", str);
		L_INFO(str);
		L_INFO(sv);
		L_INFO("");
		L_INFO("unicode \xc3\xa9\xe2\x82\xac {} \xf0\x9f\x98\x80", s);
		L_INFO("tab\there\nnewline {}", i);
		std::string big(5000, 'a');
		L_INFO("big {} end", big);
		std::string huge(300000, 'z');
		L_INFO("{}", huge);
		L_INFO("short again {}", i);
		L_NOTICE("notice {}", 1);
		L_WARNING("warning {}", 2);
		L_ERR("err {}", 3);
		settle();   // the lines below are written inline, so wait for the ones above
		L_CRIT("crit {}", 4);
		L_ALERT("alert {}", 5);
		L_EMERG("emerg {}", 6);
	}
	flush_step("formats");

	// Seeded random values through a handful of call shapes.
	{
		std::mt19937_64 rng(31102026);
		for (int k = 0; k < 400; ++k) {
			int a = static_cast<int>(rng());
			double d = static_cast<double>(static_cast<long long>(rng())) / 977.0;
			const std::size_t len = rng() % 60;   // drawn in this order, not as unsequenced arguments
			const char ch = 'a' + static_cast<char>(rng() % 26);
			const std::string s(len, ch);
			switch (rng() % 4) {
				case 0: L_INFO("r{} {} {}", k, a, s); break;
				case 1: L_NOTICE("r{} {:.4f} {:>10}", k, d, s); break;
				case 2: L_WARNING("r{} {:x} {}", k, static_cast<unsigned>(a), d); break;
				default: L_ERR("r{} {}", k, s); break;
			}
		}
	}
	flush_step("random");

	// Decoration: timestamp, thread, location, tint, indent.
	{
		Logging::config.with_threads = true;
		Logging::config.with_location = true;
		tape->located_line = __LINE__ + 1;
		L_INFO("with thread and location");
		settle();
		Logging::config.with_threads = false;
		Logging::config.with_location = false;
		Logging::config.tint = true;
		L_NOTICE("tinted");
		L_WARNING("tinted");
		settle();
		Logging::config.tint = false;
		Logging::config.with_timestamp = false;
		L_INFO("no timestamp");
		settle();
		Logging::config.with_timestamp = true;
		{
			L_STACKED(LOG_INFO, "stacked one");
			L_INFO("indented once");
			{
				L_STACKED(LOG_INFO, "stacked two");
				L_INFO("indented twice");
			}
			L_INFO("back to once");
		}
		L_INFO("no indent");
	}
	flush_step("decoration");

	// Exceptions.
	{
		try {
			throw std::runtime_error("something failed");
		} catch (...) {
			L_EXC("caught {}", 5);
			L_EXC("");
		}
	}
	flush_step("exceptions");

	// Dedup.
	{
		for (int k = 0; k < 5; ++k) {
			L_INFO_ONCE("once only");
			L_WARNING_ONCE("warning once {}", 1);
			L_ERR_ONCE("err once");
		}
		L_INFO_ONCE("once only");
	}
	flush_step("once");

	// Filtered levels write nothing, and evaluate nothing.
	{
		Logging::config.log_level = LOG_WARNING;
		int evaluated = 0;
		L_INFO("filtered {}", ++evaluated);
		L_DEBUG("filtered {}", ++evaluated);
		L_WARNING("kept {}", ++evaluated);
		std::printf("evaluated %d\n", evaluated);
		Logging::config.log_level = LOG_DEBUG;
	}
	flush_step("filter");

	// The public entry points.
	{
		Logging::do_log(false, std::chrono::steady_clock::now(), true, LOG_INFO, "do_log string");
		Logging::do_log(false, std::chrono::steady_clock::now(), true, true, 0, LOG_NOTICE, std::exception_ptr{}, std::source_location{}, "do_log full");
		settle();
		Logging::log(LOG_WARNING, "Logging::log inline");
		L_PRINT("print {} {}", 1, "two");
		print_msg("print_msg {}", 3);
	}
	flush_step("public");

	// Deferred lines, the handle's cancel and swap.
	{
		{   // cancelled before it fires: silent, and so is its swap
			auto h = L_DELAYED(true, 150ms, LOG_WARNING, "slow {}", 1);
			h.L_DELAYED_UNLOG(LOG_NOTICE, "done {}", 1);
			h.clear();
		}
		{   // the work is fast: the handle drops, the watchdog is cancelled, the swap is dropped
			auto h = L_DELAYED(true, 150ms, LOG_WARNING, "slow {}", 2);
			h.L_DELAYED_UNLOG(LOG_NOTICE, "done {}", 2);
		}
		{   // fires (slow work), then the swap follows when the handle drops
			auto h = L_DELAYED(true, 20ms, LOG_WARNING, "slow {}", 3);
			h.L_DELAYED_UNLOG(LOG_NOTICE, "done {}", 3);
			std::this_thread::sleep_for(120ms);
			wait_for_row("slow 3");
		}
		{   // slow work well past 100 ms: the line that fires ends with its age
			auto h = L_DELAYED(true, 130ms, LOG_WARNING, "late {}", 8);
			std::this_thread::sleep_for(260ms);
			wait_for_row("late 8");
		}
		{   // a deferred line that does not clear fires on its own
			auto h = L_DELAYED(false, 20ms, LOG_NOTICE, "later {}", 4);
			std::this_thread::sleep_for(120ms);
			wait_for_row("later 4");
			std::printf("clear after fire: %d\n", h.clear() ? 1 : 0);
		}
		{   // an immediate line with a swap: both are written, in order
			auto h = L_INFO("first {}", 5);
			h.L_DELAYED_UNLOG(LOG_NOTICE, "second {}", 5);
		}
		{   // a handle kept past the line, cleared too late
			auto h = L_NOTICE("kept {}", 6);
			std::this_thread::sleep_for(60ms);
			std::printf("clear after immediate fire: %d\n", h.clear() ? 1 : 0);
		}
		L_TIMED(100ms, "timed out {}", "finished {}", 7);
	}
	flush_step("deferred");

	// A real sink: the marker, the colors and the line, as the file receives them.
	{
		char path[] = "/tmp/logger_transcript_XXXXXX";
		int fd = ::mkstemp(path);
		if (fd < 0) {
			return 1;
		}
		::close(fd);
		Logging::handlers.push_back(std::make_unique<StreamLogger>(path));
		Logging::config.color = LogColorMode::always;
		Logging::config.color_depth = LogColorDepth::truecolor;
		Logging::config.timestamp_gradient = true;
		Logging::config.tint = true;
		for (int p = 0; p <= 7; ++p) {
			L(p, "file level {}", p);
			settle();
		}
		Logging::config.color_depth = LogColorDepth::ansi16;
		L_INFO("ansi16 {}", 1);
		settle();
		Logging::config.color = LogColorMode::never;
		L_INFO("never {}", 1);
		L_ERR("never {}", 2);
		settle();
		std::ifstream in(path, std::ios::binary);
		std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		std::printf("== file (%zu bytes)\n%s\n", bytes.size(), Tape::escape(bytes).c_str());
		::unlink(path);
		Logging::handlers.pop_back();
		reset_config();
		tape->rows.clear();
	}

	// After finish(): every line is refused and written inline, a watchdog is kept off the wheel.
	{
		Logging::finish();
		L_INFO("after finish {}", 1);
		L_NOTICE("after finish {}", 2);
		L_ERR("after finish {}", 3);
		L_CRIT("after finish {}", 4);
		{
			auto h = L_DELAYED(true, 150ms, LOG_WARNING, "watchdog {}", 5);   // fast work: stays silent
		}
		{
			auto h = L_DELAYED(true, 20ms, LOG_WARNING, "watchdog {}", 6);    // slow work: written when it ends
			h.L_DELAYED_UNLOG(LOG_NOTICE, "done {}", 6);
			std::this_thread::sleep_for(80ms);
		}
		{
			Logging::do_log(false, std::chrono::steady_clock::now() + 5ms, true, LOG_INFO, "future routine line");   // lost
			Logging::do_log(false, std::chrono::steady_clock::now() + 5ms, true, LOG_ERR, "future severe line");     // written now
		}
	}
	flush_step("after finish");
	if (!tape->late_annotated) {
		std::fprintf(stderr, "a line written well past 100 ms did not end with its age\n");
		return 1;
	}
	return 0;
}
