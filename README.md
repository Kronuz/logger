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

A producer pays for the call, not for the line's trip to the sink. `bench/lg.cc` times one
`L_INFO("value {} and {}", i, "x")` on the producer thread against a synchronous fmt-and-mutex sink
with no I/O (median of five runs under `quiet strict`, two runs each):

| ns per call | Apple M4 Pro (arm64) |
|---|---|
| 1 producer, LOG thread idle | 62 to 64 |
| 1 producer, LOG thread draining | 59 to 62 |
| 4 producers, LOG thread draining | 150 to 154 |
| fmt-and-mutex sink, 1 producer / 4 producers | 25 to 26 / 86 to 90 |

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DLOGGER_BUILD_BENCH=ON
cmake --build build --target logger_bench && ./build/logger_bench
```

`bench/steady.cc` is a profiling loop, and `bench/parts.cc` times each piece of a call alone.

What a call does, and what it leaves to the LOG thread:

- The message is formatted into the thread's reusable buffer: plain `{}` fields over integers,
  `bool`, `char` and strings are written straight into it, and anything else goes through
  `std::vformat` into the thread's reusable string. Nothing is allocated for the text until the
  line is copied into its record.
- One steady-clock read stamps the line (on macOS, `mach_continuous_time`, checked against
  `steady_clock` at start-up); the wall-clock stamp is derived from it with an offset measured again
  every 20 ms. The thread's id and indent come from one thread-local block.
- The record is pushed onto the scheduler's lock-free stack with one compare-exchange, and the LOG
  thread, not the producer, files it. The LOG thread is signalled only when it sleeps past the line,
  once per sleep. A line due within 1 ms (the wheel's resolution) of its call is due exactly 1 ms
  after it, so those lines wait in an arrival queue in order and never enter the wheel.
- The pending count is a striped counter the producers add to and a consumer-owned counter the LOG
  thread adds to, so no cache line is written by both sides.
- The timestamp's broken-down local time is cached per thread per whole second and rebuilt when the
  second or the `TZ` value changes.

What is left, on the Intel laptop, is two allocations per line (the record, about 55 ns, and the
text, about 40 to 80 ns for a message past the short-string size), two clock reads (20 ns each) and the
formatting itself; the M1 Pro's allocator and clock are three times cheaper.

**Ordering and the 1 ms delay.** A line is written up to 1 ms after the call, and
`Logging::finish()` drains the wheel, so a line logged just before `finish()` is
written. A hard crash within that millisecond loses the line. Of two lines that go through the LOG
thread, whose logging is ordered (one thread's calls, or one thread's call finishing before
another's begins) and that are due in that order, the first is never written after the second, and
lines in one 1 ms tick come out in the order they were logged. Lines of unrelated threads can leave
due order by the gap between a thread's clock read and its push (microseconds). Two kinds of line
skip the LOG thread and are written at the call, so they can come out ahead of routine lines logged
before them: lines more severe than `LOG_ERR` (`L_CRIT`, `L_ALERT`, `L_EMERG`), and lines due at once
that are logged after `Logging::finish()` has begun, while it drains the earlier ones.

## 📚 Documentation

- [docs/API.md](docs/API.md): `LogConfig`, `LogHooks`, sinks, the macros, and the
  `Log` handle.
- [ARCHITECTURE.md](ARCHITECTURE.md): the three paths a line takes, and how
  cancel and swap work.
- [docs/ROADMAP.md](docs/ROADMAP.md): open design work, such as isolating slow
  sinks.
- [AGENTS.md](AGENTS.md): repository map, invariants, and traps.
- [CHANGELOG.md](CHANGELOG.md): behavior changes and their measurements.

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
[Xapiand](https://github.com/Kronuz/Xapiand), which builds on this library.
