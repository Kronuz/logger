# logger

Part of [Kronuz C++ Libraries](https://github.com/Kronuz/cpp-libraries), a collection of small, reusable C++ libraries.

[![CI](https://github.com/Kronuz/logger/actions/workflows/ci.yml/badge.svg)](https://github.com/Kronuz/logger/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)

Lock-free, deferred logger for C++20: one thread writes the lines in order, and delayed lines log only slow operations.
Each thread formats its lines into its own queue, with no lock and no allocation,
and a "this is taking too long" line waits until it is due or is cancelled with
one atomic, so a fast operation logs nothing. Reach for it in servers that log
from many threads on hot paths.

![logger: the demo's colored output, one marker per severity](docs/logger.svg)

## ✨ Highlights

- **Producers hand lines off.** A routine line is formatted into the thread's own
  queue and published with one store, with no lock, allocation or read-modify-write;
  lines more severe than `LOG_ERR` are written at the call.
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
[wheel](https://github.com/Kronuz/wheel) (the timer wheel deferred lines wait in),
[scheduler](https://github.com/Kronuz/scheduler) (for its threadpool's thread),
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
- **Crash-proof routine lines.** A routine line logged while nothing is pending
  is written at once, but one logged while lines are pending waits for the next
  1 ms batch, so a hard crash within that millisecond loses it; `Logging::finish()`
  drains the queues before exit.
- **Sub-millisecond deferred timing.** A deferred line waits in a timer wheel of
  1 ms slots: it is never written before its due time, and is written by the end
  of its slot plus the LOG thread's wake, about 1 ms late.
- **One order across severities.** Lines more severe than `LOG_ERR`, and lines
  due at once that are logged after `Logging::finish()` begins, are written at
  the call and can come out ahead of routine lines logged before them. Routine
  lines whose logging is ordered (one thread's calls, or one call finishing
  before another begins) come out in that order; lines of two threads stamped
  within one tick of the stamp clock (41.67 ns on Apple silicon) can come out in
  either order.

## ⚡ Performance

`bench/lg.cc` times what a producer thread pays for one `L_INFO("value {} and {}", i, "x")`, in
nanoseconds per call, the median of five rounds; each cell is the range over the runs of one
`quiet strict` hold per machine. *Burst* times batches of 1,000 calls on each producer with 5 ms pauses between them, in which an
asynchronous logger's consumer writes the batch, so it is a producer's own time per call; the
producer spins through the pause, since a sleeping one would time the core's clock ramp. *Back to
back* times calls back to back while the consumer writes them, as wall time divided by all calls,
so with four producers it is their combined rate. `L_INFO` is this library. [quill](https://github.com/odygrd/quill)
v13.1.0 is an asynchronous logger whose caller copies the arguments into a per-thread queue and
whose backend thread formats them; *fmt + mutex* is a synchronous logger whose caller formats the
message with [fmt](https://github.com/fmtlib/fmt) 10.2.1 into a buffer and counts its bytes under a
`std::mutex`, with no timestamp and no I/O. All three sinks receive the same text (`logger_bench
--check`: 100,000 messages each, none different); the logger's sink also receives the timestamp,
which its LOG thread formats.

| Producers, row | M4 Pro: `L_INFO` | quill | fmt + mutex | i9-9980HK: `L_INFO` | quill | fmt + mutex |
|---|---:|---:|---:|---:|---:|---:|
| 1, burst | 22.7 | 6.5 to 7.0 | 27.8 to 28.8 | 59.3 to 60.1 | 27.4 to 30.5 | 62.5 to 67.7 |
| 1, back to back | 21.0 to 21.8 | 8.6 to 9.1 | 28.2 to 29.0 | 82.2 to 83.6 | 57.8 to 66.1 | 61.2 to 62.8 |
| 4, burst | 21.7 to 22.1 | 6.5 to 6.8 | 41.5 to 49.2 | 65.5 to 67.1 | 35.1 to 37.8 | 137.0 to 152.7 |
| 4, back to back | 6.0 to 6.1 | 5.4 to 7.4 | 79.7 to 84.8 | 23.4 to 24.0 | 25.9 to 28.1 | 171.2 to 173.1 |

One `L_INFO` producer pays less than fmt + mutex on the M4 Pro in both rows and on the i9 in
bursts, and more on the i9 back to back (83 against 62 ns): there its LOG thread writes a line in
about 535 ns, so the queue grows to hold the backlog and the producer's stores miss the cache, and
the stamp clock alone (`mach_continuous_time`) costs 20 ns. Four producers pay a half to a
thirteenth of what they pay under one mutex. quill's producer is two to three and a half times cheaper in
bursts and about even with four producers back to back, because its caller copies the arguments
and its backend thread formats them, where `L_INFO` formats on the calling thread; its backend
writes a line in about 365 ns on the M4 Pro and 620 on the i9, against 270 and 535 for the LOG
thread (*end-to-end* in `logger_bench`).

`bench/latency.cc` times one isolated `L_INFO` (300 of them, 5 ms apart, so nothing is pending
when each is logged) from the call until the handler runs, in microseconds. Such a line is written
at once; a line logged while others are pending waits for the next 1 ms batch instead.

| Machine | median | 90th percentile |
|---|---:|---:|
| Apple M4 Pro | 13.7 | 29 to 39 |
| Intel Core i9-9980HK | 40 | 72 to 73 |

Apple M4 Pro (arm64), macOS 26.7.2, and Intel Core i9-9980HK (x86_64), macOS 15.8.1, both with
Apple clang 17.0.0 and `-O3 -DNDEBUG`, at the commit that added the per-thread queues:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DLOGGER_BUILD_BENCH=ON -DLOGGER_BENCH_QUILL=ON
cmake --build build --target logger_bench logger_latency
./build/logger_bench --check && ./build/logger_bench
./build/logger_latency
```

[ARCHITECTURE.md](ARCHITECTURE.md#what-one-call-costs) breaks one call down into its pieces;
`bench/parts.cc` times each piece alone, `bench/drain.cc` the LOG thread's drain, and
`bench/steady.cc` is a profiling loop.

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
`logger_bench`, `logger_parts`, `logger_steady`, `logger_drain`, and `logger_latency`; it fetches
fmt for the synchronous comparator, and `-DLOGGER_BENCH_QUILL=ON` fetches quill for the
asynchronous one.

## 🤝 Contributing

Issues and pull requests are welcome. Read [AGENTS.md](AGENTS.md) and the
catalog's [rules of engagement](https://github.com/Kronuz/cpp-libraries/blob/main/AGENTS.md#rules-of-engagement)
first; CI must pass on every platform.

## 📄 License

MIT, Copyright (c) 2026 Germán Méndez Bravo and Copyright (c) 2015-2020 Dubalu
LLC. See [LICENSE](LICENSE). Extracted from [Xapiand](https://github.com/Kronuz/Xapiand).
