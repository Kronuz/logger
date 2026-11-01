# logger

Part of [Kronuz C++ Libraries](https://github.com/Kronuz/cpp-libraries), a collection of small, reusable C++ libraries.

[![CI](https://github.com/Kronuz/logger/actions/workflows/ci.yml/badge.svg)](https://github.com/Kronuz/logger/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)

Lock-free, deferred logger for C++20: one thread writes the lines in order, and delayed lines log only slow operations.
Many threads hand their lines to that LOG thread with one compare-exchange each,
and a "this is taking too long" line waits in the [stash](https://github.com/Kronuz/stash)
timer wheel until it is due or cancelled with one atomic, so a fast operation
logs nothing. Reach for it in
servers that log from many threads on hot paths.

![logger: the demo's colored output, one marker per severity](docs/logger.svg)

## ✨ Highlights

- **Producers hand lines off.** A routine line reaches the LOG thread with one
  compare-exchange onto the scheduler's lock-free stack; lines more severe than
  `LOG_ERR` are written at the call.
- **Log slow operations only.** `L_DELAYED_200("request {} is slow", id)` prints
  only if it is not cleared within 200 ms, annotated with how long it ran.
- **`std::format` messages**, and no argument is evaluated below the log level.
- **Dedup, indentation, and timing.** `L_INFO_ONCE`,
  `L_WARNING_ONCE_PER_MINUTE`, stacked indentation with `L_STACKED`, and
  `L_TIMED` lines that swap a timeout warning for a "done" line when the work
  finishes.
- **Terminal-aware color** through [term-color](https://github.com/Kronuz/term-color):
  honors `--color`-style modes and `NO_COLOR`, with iTerm2 marks and badges.
- **Backpressure.** `max_pending` drops routine lines past a cap and reports how
  many.
- **Hooks for the host**: exception descriptions, backtraces, thread names, and
  timestamps.

## 🚀 Quick start

```cpp
#include <chrono>
#include <memory>
#include <stdexcept>
#include <thread>

#include "logger.h"

void handle_request(int) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
void risky() { throw std::runtime_error("disk full"); }

int main() {
	Logging::config.log_level = LOG_INFO;
	Logging::add_handler(std::make_unique<StderrLogger>());

	L_INFO("starting up, {} workers", 8);       // written by the LOG thread

	{
		L_DELAYED_200("request {} is slow", 42);   // armed for 200 ms
		handle_request(42);                        // takes 5 ms
		L_DELAYED_N_CLEAR();                       // cancelled: never written
	}

	try {
		risky();
	} catch (...) {
		L_EXC("risky() failed");                   // with the exception's what()
	}

	Logging::finish();                             // drain before exit
}
// stderr, each line after its severity marker:
//   2026-11-01 04:20:53.598 starting up, 8 workers
//   2026-11-01 04:20:53.605 risky() failed: disk full
```

[`examples/demo.cc`](examples/demo.cc) tours every level, deferred lines, once
dedup, stacked indentation, and the iTerm2 integration.

## 📦 Installation

Requires a C++20 compiler and CMake 3.14 or newer; it fetches
[scheduler](https://github.com/Kronuz/scheduler) (with threadpool and stash),
term-color, and the header-only cppcodec, and compiles one source file. CI builds
it on Linux (x86_64, arm64), macOS (arm64, x86_64), and FreeBSD, plus ASan,
UBSan, and TSan builds.

```cmake
include(FetchContent)
FetchContent_Declare(logger
  GIT_REPOSITORY https://github.com/Kronuz/logger.git
  GIT_TAG        8bd9d7fa9545a9646248c3d49489fdb0f61d41a6)  # or main for the tip
FetchContent_MakeAvailable(logger)

target_link_libraries(my_app PRIVATE logger::logger)
```

```cpp
#include "logger.h"
```

## 🎯 When to use it

Use it to log from many threads on hot paths with ordered output, and to warn
only when a request, a commit, or a lock wait is slow.

Not for:

- **Structured logging.** Sinks receive formatted text lines, not fields.
- **A single-threaded tool.** Plain `std::fprintf(stderr, ...)` is simpler.
- **Crash-proof routine lines.** A routine line is written up to 1 ms after the
  call, so a hard crash within that millisecond loses it; `Logging::finish()`
  drains the wheel before exit.
- **One order across severities.** Lines more severe than `LOG_ERR`, and lines
  due at once that are logged after `Logging::finish()` begins, are written at
  the call and can come out ahead of routine lines logged before them. Routine
  lines whose logging is ordered (one thread's calls, or one call finishing
  before another begins) come out in that order; lines of unrelated threads can
  leave due order by the microseconds between a thread's clock read and its push.

## ⚡ Performance

A producer pays for the call, not for the line's trip to the sink. `bench/lg.cc`
times one `L_INFO("value {} and {}", i, "x")` on the producer thread against a
synchronous fmt-and-mutex sink with no I/O. ns per call, Apple clang, Release;
each cell is the range over two runs, each the median of five; with four
producers, wall time over all their calls:

| ns per call | Apple M4 Pro (arm64) | Intel Core i9-9980HK (x86_64) |
|---|---|---|
| 1 producer, LOG thread idle | 65 to 66 | 387 to 402 |
| 1 producer, LOG thread draining | 55 to 69 | 223 to 228 |
| 4 producers, LOG thread draining | 158 to 172 | 81 to 85 |
| fmt-and-mutex sink, 1 producer / 4 producers | 25 to 26 / 85 to 91 | 58 to 59 / 165 to 177 |

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DLOGGER_BUILD_BENCH=ON
cmake --build build --target logger_bench
./build/logger_bench
```

[ARCHITECTURE.md](ARCHITECTURE.md#what-one-call-costs) breaks one call down into
its pieces; `bench/parts.cc` times each piece alone, and `bench/steady.cc` is a
profiling loop.

## 📚 Documentation

- [docs/API.md](docs/API.md): `LogConfig`, `LogHooks`, sinks, the macros, and the
  `Log` handle.
- [ARCHITECTURE.md](ARCHITECTURE.md): the three paths a line takes, how cancel
  and swap work, and what one call costs.
- [docs/ROADMAP.md](docs/ROADMAP.md): open design work, such as isolating slow
  sinks.
- [CHANGELOG.md](CHANGELOG.md): what changed, with its measurements.
- [AGENTS.md](AGENTS.md): repository map, invariants, and traps.
- [Kronuz C++ Libraries](https://github.com/Kronuz/cpp-libraries/blob/main/AGENTS.md#logger): the catalog entry.

## 🛠️ Building and testing

```sh
cmake -B build
cmake --build build
ctest --test-dir build
```

`./build/logger_demo` runs the demo. Add `-DLOGGER_BUILD_BENCH=ON` to build
`logger_bench`, `logger_parts`, `logger_steady`, and `logger_drain`; it fetches
fmt for the comparator.

## 🤝 Contributing

Issues and pull requests are welcome. Read [AGENTS.md](AGENTS.md) and the
catalog's [rules of engagement](https://github.com/Kronuz/cpp-libraries/blob/main/AGENTS.md#rules-of-engagement)
first; CI must pass on every platform.

## 📄 License

MIT, Copyright (c) 2026 Germán Méndez Bravo and Copyright (c) 2015-2020 Dubalu
LLC. See [LICENSE](LICENSE). Extracted from [Xapiand](https://github.com/Kronuz/Xapiand).
