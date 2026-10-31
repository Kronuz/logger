# logger

Part of [Kronuz C++ Libraries](https://github.com/Kronuz/cpp-libraries), a collection of small, reusable C++ libraries.

[![CI](https://github.com/Kronuz/logger/actions/workflows/ci.yml/badge.svg)](https://github.com/Kronuz/logger/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)

Lock-free, deferred logging for C++20: many threads log without blocking each
other, one thread writes in order, and a "this is taking too long" line can be
armed and cancelled with one atomic, so fast operations log nothing. For
servers that want to log slow operations only.

![logger: the demo's colored output, one marker per severity](docs/logger.svg)

## ✨ Highlights

- **Never blocks the caller.** Routine lines are handed to the log thread through
  the lock-free [stash](https://github.com/Kronuz/stash) wheel; severe lines
  write immediately.
- **Log slow operations only.** `L_DELAYED_200("request {} is slow", id)` fires
  only if it is not cancelled within 200 ms, annotated with how long it ran.
- **`std::format` messages**, and arguments are not even evaluated below the
  log level.
- **Once, once per minute, stacked indentation, and timed lines.**
- **Terminal-aware color** through [term-color](https://github.com/Kronuz/term-color):
  honors `--color`-style modes and `NO_COLOR`, with iTerm2 marks and badges.
- **Backpressure.** `max_pending` drops routine lines past a cap and reports how
  many.
- **Hooks for the host**: exception descriptions, backtraces, thread names, and
  timestamps.

## 📦 Installation

Requires a C++20 compiler and CMake 3.14 or newer; it fetches
[scheduler](https://github.com/Kronuz/scheduler) (with threadpool and stash) and
term-color, and compiles one source file. CI builds it on Linux (x86_64, arm64),
macOS (arm64, x86_64), and FreeBSD, plus ASan, UBSan, and TSan builds.

```cmake
include(FetchContent)
FetchContent_Declare(logger
  GIT_REPOSITORY https://github.com/Kronuz/logger.git
  GIT_TAG        main)
FetchContent_MakeAvailable(logger)

target_link_libraries(my_app PRIVATE logger::logger)
```

## 🚀 Quick start

```cpp
#include "logger.h"

void handle_request(int);   // your code
void risky();

int main() {
	Logging::config.log_level = LOG_INFO;
	Logging::add_handler(std::make_unique<StderrLogger>());

	L_INFO("starting up, {} workers", 8);

	{
		L_DELAYED_200("request {} is slow", 42);   // armed for 200 ms
		handle_request(42);
		L_DELAYED_N_CLEAR();                      // fast enough: never prints
	}

	try { risky(); } catch (...) { L_EXC("risky() failed"); }   // logs the exception
	Logging::finish();                                          // drain before exit
}
```

[`examples/demo.cc`](examples/demo.cc) tours every level, deferred lines, once
dedup, stacked indentation, and the iTerm2 integration.

## 🎯 When to use it

- ✅ Logging from many threads on hot paths, with ordered output.
- ✅ "Warn me only if this is slow" around requests, commits, or locks.
- ❌ Structured logging (JSON fields, log shipping): sinks receive formatted
  text lines.
- ❌ A single-threaded tool: plain `std::fprintf(stderr, …)` is simpler.

## ⚡ Performance

On Apple silicon, one producer thread pays about 235 ns per `L_INFO` with the LOG thread idle (310 ns before), and a line goes from call to written in about 460 ns (670 before); four producers pay about 360 ns each (610 before). Three things make that cheap:

- A line due within 1 ms (the wheel's resolution) of the call, routine, deferred or `clears`, is scheduled 1 ms after the call. The LOG thread sleeps until then, and the lines queued meanwhile are due later than its wake time, so the producers send it no signal. The scheduler also skips the signal while its thread is awake.

**Ordering and the 1 ms delay.** A line is written up to 1 ms after the call, and `Logging::finish()` drains the wheel, so a line logged just before `finish()` is written. A hard crash within that millisecond loses the line (it used to lose it within the signal latency). Order: of two lines due in the order they were logged, the first is never written after the second, and lines in one 1 ms tick come out in the order they were logged. (Before this fix the shift skipped `clears` lines, so a `clears` line due 0.3 ms out could overtake an immediate line logged before it.)
- The timestamp's broken-down local time is cached per thread per whole second and rebuilt when the second or the `TZ` value changes; the digits are written as integers, with `snprintf` kept only for years outside 0..9999 and pre-epoch times.
- A call reads the steady clock once and the system clock once.

`bench/lg.cc` (`-DLOGGER_BUILD_BENCH=ON`) measures the producer cost against a synchronous fmt-and-mutex sink.

## 📚 Documentation

- [docs/API.md](docs/API.md): `LogConfig`, `LogHooks`, sinks, the macros, and the
  `Log` handle.
- [ARCHITECTURE.md](ARCHITECTURE.md): the three paths a line takes, and how
  cancel and swap work.
- [docs/ROADMAP.md](docs/ROADMAP.md): open design work, such as isolating slow
  sinks.
- [AGENTS.md](AGENTS.md): repository map, invariants, and traps.

## 🛠️ Building and testing

```sh
cmake -B build && cmake --build build && ctest --test-dir build
./build/logger_demo
```

## 🤝 Contributing

Issues and pull requests are welcome. Read [AGENTS.md](AGENTS.md) first. CI must
pass on every platform.

## 📄 License

MIT, Copyright (c) 2015-2019 Dubalu LLC. See [LICENSE](LICENSE). Extracted from
[Xapiand](https://github.com/Kronuz/Xapiand), which now builds on this library.
