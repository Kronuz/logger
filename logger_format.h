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

#pragma once

// The message formatter: a thread's scratch buffers, the fast path for plain `{}` formats, and FormattedMsg.

#include <charconv>      // for std::to_chars
#include <cstddef>       // for std::size_t
#include <cstdint>       // for uint64_t
#include <cstring>       // for std::memcpy, std::memchr
#include <format>        // for std::vformat_to, std::make_format_args
#include <iterator>      // for std::back_inserter
#include <string>        // for std::string
#include <string_view>   // for std::string_view
#include <type_traits>  // for std::remove_cvref_t, std::is_same_v

namespace logger_detail {
// The calling thread's reusable message buffers, and whether a message is being formatted into
// them: a raw buffer the fast formatter writes into, and a string std::vformat appends to. The
// slot is constant-initialized and trivially destructible, so reaching it is a plain thread-local
// access; the buffers are allocated on first use and freed (through a second thread-local) when
// the thread ends, after which the slot is `dead` and a message formats into its own string.
struct ScratchSlot {
	char* buf = nullptr;
	std::size_t cap = 0;
	std::string* text = nullptr;
	bool leased = false;
	bool dead = false;
};
inline thread_local ScratchSlot scratch_slot;
bool scratch_init() noexcept;   // allocates the buffers on first use; false once the slot is dead
char* scratch_grow() noexcept;  // doubles the raw buffer (to 1 MiB at most); null if it cannot

// A fast path for the commonest messages: a format string whose fields are all `{}` (and the
// `{{` `}}` escapes) over integers, bool, char and strings. It writes exactly the bytes
// std::vformat would, straight into a raw buffer, and gives up at the first thing it is not sure
// of: a field with an index or a spec, a malformed format, too few arguments, a null `const char*`,
// or a char array with a NUL before its end (`declined`), or a message that does not fit (`full`).
// The caller then formats with std::vformat, which also decides what an error looks like. It is
// several times faster than std::vformat on these messages.
template <typename T>
inline constexpr bool fast_format_arg_v = [] {
	using U = std::remove_cvref_t<T>;
	if constexpr (std::is_same_v<U, bool> || std::is_same_v<U, char>) {
		return true;
	} else if constexpr (std::is_integral_v<U>) {
		return sizeof(U) <= 8 && !std::is_same_v<U, wchar_t> && !std::is_same_v<U, char8_t>
			&& !std::is_same_v<U, char16_t> && !std::is_same_v<U, char32_t>;
	} else if constexpr (std::is_array_v<U>) {
		return std::is_same_v<std::remove_cv_t<std::remove_extent_t<U>>, char>;
	} else if constexpr (std::is_pointer_v<U>) {
		return std::is_same_v<std::remove_cv_t<std::remove_pointer_t<U>>, char>;
	} else {
		return std::is_same_v<U, std::string> || std::is_same_v<U, std::string_view>;
	}
}();

enum class FastResult : unsigned char { ok, declined, full };

// A write position in the raw buffer.
struct FastOut {
	char* p;
	char* end;
};

// Copies n bytes without a call for the short runs that make up most of a message: two overlapping
// loads and stores of the widest size that fits (they read only inside [s, s + n)).
inline void fast_copy_bytes(char* d, const char* s, std::size_t n) noexcept {
	if (n >= 16) {
		std::memcpy(d, s, n);
	} else if (n >= 8) {
		std::uint64_t a, b;
		std::memcpy(&a, s, 8);
		std::memcpy(&b, s + n - 8, 8);
		std::memcpy(d, &a, 8);
		std::memcpy(d + n - 8, &b, 8);
	} else if (n >= 4) {
		std::uint32_t a, b;
		std::memcpy(&a, s, 4);
		std::memcpy(&b, s + n - 4, 4);
		std::memcpy(d, &a, 4);
		std::memcpy(d + n - 4, &b, 4);
	} else if (n >= 2) {
		std::uint16_t a, b;
		std::memcpy(&a, s, 2);
		std::memcpy(&b, s + n - 2, 2);
		std::memcpy(d, &a, 2);
		std::memcpy(d + n - 2, &b, 2);
	} else if (n == 1) {
		*d = *s;
	}
}

inline FastResult fast_put_bytes(FastOut& o, const char* s, std::size_t n) noexcept {
	if (static_cast<std::size_t>(o.end - o.p) < n) {
		return FastResult::full;
	}
	fast_copy_bytes(o.p, s, n);
	o.p += n;
	return FastResult::ok;
}

template <typename T>
inline FastResult fast_format_put(FastOut& o, const void* p) {
	using U = std::remove_cvref_t<T>;
	if constexpr (std::is_same_v<U, bool>) {
		return *static_cast<const bool*>(p) ? fast_put_bytes(o, "true", 4) : fast_put_bytes(o, "false", 5);
	} else if constexpr (std::is_same_v<U, char>) {
		return fast_put_bytes(o, static_cast<const char*>(p), 1);
	} else if constexpr (std::is_integral_v<U>) {
		if (o.end - o.p < 24) {
			return FastResult::full;
		}
		o.p = std::to_chars(o.p, o.p + 24, *static_cast<const U*>(p)).ptr;
		return FastResult::ok;
	} else if constexpr (std::is_array_v<U>) {
		constexpr std::size_t n = std::extent_v<U>;
		auto s = static_cast<const char*>(p);
		if (n == 0) {
			return FastResult::declined;
		}
		for (std::size_t i = 0; i + 1 < n; ++i) {
			if (s[i] == '\0') {
				return FastResult::declined;   // a NUL before the end: std::vformat decides what that prints
			}
		}
		if (s[n - 1] != '\0') {
			return FastResult::declined;
		}
		return fast_put_bytes(o, s, n - 1);
	} else if constexpr (std::is_pointer_v<U>) {
		auto s = *static_cast<const U*>(p);
		if (s == nullptr) {
			return FastResult::declined;
		}
		return fast_put_bytes(o, s, std::strlen(s));
	} else {
		const auto& v = *static_cast<const U*>(p);
		return fast_put_bytes(o, v.data(), v.size());
	}
}

template <typename... Args>
inline FastResult fast_format(FastOut& o, std::string_view fmt, const Args&... args) {
	constexpr std::size_t N = sizeof...(Args);
	using Put = FastResult (*)(FastOut&, const void*);
	static constexpr Put puts[N] = {&fast_format_put<Args>...};
	const void* const ptrs[N] = {static_cast<const void*>(std::addressof(args))...};
	std::size_t next = 0;
	const char* p = fmt.data();
	const char* const end = p + fmt.size();
	while (p < end) {
		const char* q = p;
		while (q < end && *q != '{' && *q != '}') {
			++q;
		}
		if (auto r = fast_put_bytes(o, p, static_cast<std::size_t>(q - p)); r != FastResult::ok) {
			return r;
		}
		if (q == end) {
			break;
		}
		const bool paired = q + 1 < end && q[1] == q[0];   // `{{` or `}}`
		if (paired) {
			if (auto r = fast_put_bytes(o, q, 1); r != FastResult::ok) {
				return r;
			}
			p = q + 2;
		} else if (*q == '{' && q + 1 < end && q[1] == '}') {
			if (next >= N) {
				return FastResult::declined;
			}
			if (auto r = puts[next](o, ptrs[next]); r != FastResult::ok) {
				return r;
			}
			++next;
			p = q + 2;
		} else {
			return FastResult::declined;   // an index, a spec, a lone brace
		}
	}
	return FastResult::ok;
}

// The fast path into a string, for tests: whether it took the message, and what it wrote.
template <typename... Args>
inline bool fast_format_to(std::string& out, std::string_view fmt, const Args&... args) {
	static thread_local std::string buf(1 << 16, '\0');
	FastOut o{buf.data(), buf.data() + buf.size()};
	if (fast_format(o, fmt, args...) != FastResult::ok) {
		return false;
	}
	out.assign(buf.data(), o.p);
	return true;
}

// A formatted message, valid until the end of the full expression that made it. It lives in the
// thread's reusable buffers, so a thread that logs steadily formats without allocating; when those
// are already in use (a formatter that itself logs) or gone (the thread is exiting), it takes a
// string of its own.
class FormattedMsg {
	ScratchSlot* slot_ = nullptr;
	bool big_ = false;   // std::vformat grew the thread's string past kKeepText: give it back at the end
	std::string local_;
	std::string_view view_;

public:
	// Renders like format_msg: no arguments gives the format string as it is, and a formatting
	// error gives it too.
	template <typename... Args>
	explicit FormattedMsg(std::string_view fmt, Args&&... args) {
		auto& slot = scratch_slot;
		if (!slot.leased && !slot.dead && (slot.buf != nullptr || scratch_init())) {
			slot.leased = true;
			slot_ = &slot;
		}
		try {
			render(fmt, args...);
		} catch (...) {   // out of memory: the destructor will not run, so give the buffers back here
			release();
			throw;
		}
	}
	~FormattedMsg() { release(); }
	FormattedMsg(const FormattedMsg&) = delete;
	FormattedMsg& operator=(const FormattedMsg&) = delete;

	std::string_view view() const noexcept { return view_; }

private:
	// The most capacity the thread's string keeps once a message is done with it.
	static constexpr std::size_t kKeepText = std::size_t{1} << 20;

	void release() noexcept {
		if (slot_) {
			if (big_) {
				std::string().swap(*slot_->text);
			}
			slot_->leased = false;
			slot_ = nullptr;
		}
	}

	// The message through std::vformat into the thread's string, or this object's when that is busy.
	template <typename... Args>
	void render_std(std::string_view fmt, Args&... args) {
		std::string* text = slot_ != nullptr ? slot_->text : &local_;
		text->clear();
		if constexpr (sizeof...(Args) == 0) {
			text->assign(fmt);
		} else {
			try {
				std::vformat_to(std::back_inserter(*text), fmt, std::make_format_args(args...));
			} catch (...) {
				text->assign(fmt);
			}
		}
		view_ = *text;
		big_ = slot_ != nullptr && text->capacity() > kKeepText;
	}

	template <typename... Args>
	void render(std::string_view fmt, Args&... args) {
		if constexpr (sizeof...(Args) != 0 && (fast_format_arg_v<Args> && ...)) {
			if (slot_ != nullptr) {
				for (;;) {
					FastOut o{slot_->buf, slot_->buf + slot_->cap};
					auto r = fast_format(o, fmt, args...);
					if (r == FastResult::ok) {
						view_ = std::string_view(slot_->buf, static_cast<std::size_t>(o.p - slot_->buf));
						return;
					}
					if (r == FastResult::declined || scratch_grow() == nullptr) {
						break;
					}
				}
			}
		}
		render_std(fmt, args...);
	}
};
}
