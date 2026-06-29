# logger

Deferred, lock-free logging built on the Kronuz
[scheduler](https://github.com/Kronuz/scheduler) and
[stash](https://github.com/Kronuz/stash) wheel. Extracted from
[Xapiand](https://github.com/Kronuz/Xapiand), where it is the original reason the
wheel exists.

## What it is

Many threads arm log lines without ever blocking each other; a single thread
drains them and writes them in order; and a scheduled "this is taking too long"
line can be cancelled with one atomic when the operation finishes in time, so the
common case logs nothing. That last part is the point: it is built for *log slow
ops only*, and for any pattern where you arm a timed line and usually cancel it.

A line takes one of three paths (see [ARCHITECTURE.md](ARCHITECTURE.md)): the
severe levels write inline on the calling thread; routine levels are handed to
the LOG thread keyed at *now*; deferred lines are keyed at *now + delay* and fire
only if not cancelled first.

## When to use it / when not

Use it when logging is on a hot path with many threads, when you want slow-path
or debounced lines that you arm and usually cancel, and when one thread writing
ordered output is what you want. It depends only on the scheduler.

Reach for something simpler if you have a single thread, or no notion of *when*,
or you need pluggable structured-logging sinks and severity routing beyond what
`Logger` handlers give you. This is a fast, lock-free arm with a deferred-cancel
trick, not a full logging framework.

## Install

Header + one source file, consumed via CMake `FetchContent`:

```cmake
include(FetchContent)
FetchContent_Declare(logger
  GIT_REPOSITORY https://github.com/Kronuz/logger.git
  GIT_TAG        main)
FetchContent_MakeAvailable(logger)
target_link_libraries(your_target PRIVATE logger::logger)
```

That transitively pulls `scheduler` (and its `threadpool` + `stash`). The core
needs nothing else.

## Usage

```cpp
#include "logger.h"

int main() {
    Logging::config.log_level = LOG_INFO;        // emit INFO and more severe
    Logging::config.colors = true;
    Logging::add_handler(std::make_unique<StderrLogger>());

    L_INFO("starting up, {} workers", 8);
    L_WARNING("disk at {}%", 91);

    {
        // Arm a "too slow" line 200ms out; cancel it if we finish in time.
        L_DELAYED_200("request {} is slow", req_id);
        handle_request(req_id);
        L_DELAYED_N_CLEAR();   // finished in time -> the line never prints
    }

    try { risky(); }
    catch (...) { L_EXC("risky() failed"); }     // logs the in-flight exception

    Logging::finish();   // drain and stop the LOG thread before exit
}
```

The bundled [`examples/demo.cc`](examples/demo.cc) is a runnable tour, built by a
top-level CMake build:

```sh
cmake -B build && cmake --build build && ./build/logger_demo
```

It tours the severity palette (each level's rgb marker, brightest = most severe),
the grey-gradient timestamp, `std::format` messages, exception logging, once-dedup,
stacked indentation, the deferred "log slow ops only" pattern, and the iTerm2
integration. Colorized on a tty (the demo forces it on); abbreviated here:

```
█ 2026-06-29 08:55:43.557 L_EMERG   system is unusable
▉ 2026-06-29 08:55:43.557 L_ALERT   action must be taken immediately
▊ 2026-06-29 08:55:43.557 L_CRIT    critical condition
▋ 2026-06-29 08:55:43.557 L_ERR     error condition
▌ 2026-06-29 08:55:43.557 L_WARNING warning condition
▍ 2026-06-29 08:55:43.557 L_NOTICE  normal but significant
▎ 2026-06-29 08:55:43.558 L_INFO    informational
▊ 2026-06-29 08:55:43.558 caught while doing work: something broke
▎ 2026-06-29 08:55:43.558   item A
▌ 2026-06-29 08:55:43.583 slow_op is taking too long ~201.7ms
```

Note what is *not* there: `fast_op`'s "too long" line, because it finished in time
and cancelled it (`slow_op`'s fired, annotated with how long it ran), and the
second identical once-line, which was deduped.

## API reference

### `LogConfig` (`Logging::config`)

`log_level` (emit when `priority <= log_level`), `colors`, `with_timestamp`,
`with_threads`, `with_location`, and `max_pending` (backpressure: 0 = unbounded,
otherwise routine async lines are dropped past it with a coalesced summary).

### `LogHooks` (`Logging::hooks`)

Optional, each with a std default: `describe_exception`, `backtrace`,
`thread_name`, `timestamp`. A host injects richer versions (e.g. real backtraces,
named threads) without the core depending on them.

### Sinks (`Logger`)

`StreamLogger` (a file), `StderrLogger` (tty-aware), `SysLog`. Install with
`Logging::add_handler(...)`; every line fans out to all of them.

### Macros

`L_INFO` / `L_NOTICE` / `L_WARNING` / `L_ERR` / `L_CRIT` / `L_ALERT` / `L_EMERG`,
`L_EXC` (the in-flight exception), the `*_ONCE` and `*_ONCE_PER_MINUTE` variants,
`L_DELAYED_*` with `L_DELAYED_N_UNLOG` / `L_DELAYED_N_CLEAR`, `L_TIMED`,
`L_STACKED` (nested indent), `L_PRINT` (undecorated), and `L_DEBUG` (compiled out
unless `LOGGER_DEBUG`). A line below the level evaluates none of its arguments.

### The `Log` handle

Returned by the deferred macros. `clear()` cancels the pending line; `unlog()`
swaps its message; `age()` reports how long it lived. Dropping the handle runs
the cancel/swap.

## License

MIT. See [LICENSE](LICENSE).
