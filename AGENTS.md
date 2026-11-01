# AGENTS.md

Notes for agents and contributors working in this repo. For internal design see
ARCHITECTURE.md; for usage see README.md and docs/API.md; for open design work see docs/ROADMAP.md.

## Repo map

- `logger.h` — the public API: `LogConfig`, `LogHooks`, the `Logger` sink
  interface (`StreamLogger` / `StderrLogger` / `SysLog`), `Logging` (the log
  record, which is also a `ScheduledTask`, plus the static facade), the `Log`
  handle, and the `L_*` macro surface. Also the `format_msg` / `print_msg`
  helpers and the `log_indent()` / `LogIndent` thread-local indent.
- `logger.cc` — implementations: the sinks, the three-path `add()` router,
  `operator()` decoration, `clean` / `clear` / `age`, the bounded `OnceFilter`,
  the default hook bodies, and backpressure. This file must be compiled and
  linked; the library is not header-only.
- `examples/demo.cc` — a small runnable showcase (level macros, the deferred
  slow-op line, once-dedup, stacked indent).
- `test/test.cc` — a single self-contained smoke test using a `CHECK` macro
  (see invariants below), no framework.
- `test/format.cc` — the macros' formatter (`logger_detail::FormattedMsg` and its fast path)
  against `std::vformat`, on random and edge messages.
- `test/producer.cc` — many producers against the LOG thread: exactly once, order across
  threads, the counters, and a `finish()` that races the producers (each round is a process).
- `test/transcript.cc` + `test/transcript.cmake` + `test/frozen/` — one scripted run built
  against the library and against a frozen copy of the implementation before the producer
  path changed (`5299eb3`); the bytes must be identical. Never edit `test/frozen/`.
- `test/clock.cc` — `steady_now()` and `system_at()` against `steady_clock` and `system_clock`.
- `test/order.cc`, `test/timestamp.cc`, `test/tint.cc` — the ordering oracle, the timestamp
  formatter against its frozen `snprintf` form, and the tint/collapse paths.
- `bench/lg.cc` (`logger_bench`) — producer cost against a synchronous fmt sink; `bench/parts.cc`
  (each piece of a call alone), `bench/steady.cc` (a profiling loop), `bench/drain.cc` (the LOG
  thread's drain over time). Built with `-DLOGGER_BUILD_BENCH=ON`.
- `CMakeLists.txt` — FetchContents `scheduler`, builds the static library, and
  wires the test and demo when this repo is the top-level project.

## Build & run

```sh
cmake -B build && cmake --build build
ctest --test-dir build      # or: ./build/logger_test
./build/logger_demo         # the showcase, to stderr
```

The test and demo targets are only added when this repo is top-level, so a
consumer vendoring it via `FetchContent` won't build them.

Sanitizers: use Homebrew LLVM, not Apple clang (whose ASan/TSan are broken on
recent macOS):

```sh
cmake -B build-tsan -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=thread -g -O1"
cmake --build build-tsan && ./build-tsan/logger_test
```

## Conventions

- C++20. Double quotes in code. No em dashes in docs.
- MIT-licensed; keep the copyright header (Copyright (c) 2015-2019 Dubalu LLC) on
  source files.
- The core depends on `Kronuz/scheduler` and `Kronuz/term-color` (the palette plus
  the `collapse`/`apply` resolution the sink runs per line), plus `cppcodec`
  (header-only) used only in `logger.cc` to base64-encode the iTerm2 badge text.
  Everything *host-specific* (exception description, real backtraces, thread names,
  timestamp format) goes through `LogHooks`, not a new dependency. Do not pull
  `located-exception`, `Xapian::Error`, etc. into the core; a consumer injects those
  via hooks.
- Keep `Xapian::Error` and real backtraces out of the core; they reach it through
  `LogHooks` (a consumer-side adapter, as Xapiand's). The iTerm2 integration
  (marks / tab tint / badge / notifications, gated by `config.iterm2`) ships in
  the core.

## Load-bearing invariants

- **Tests use `CHECK`, not `assert`.** The CMake build can be `Release`
  (`-DNDEBUG`), which compiles `assert` out, so a test written with `assert`
  verifies nothing. `test/test.cc` defines a `CHECK` that aborts regardless. This
  already caught a wrong expectation once; keep it.
- **The cancel/swap semantics are exact.** A deferred line that `clears` and is
  cancelled *before it fires* stays silent, and its swap (`unlog`) message is
  dropped too; a line whose timeout already fired keeps its swap. This rests on
  the one-shot `clear(true)` CAS result in `clean()` (`logger.cc`): if `clear(true)`
  wins, the line had not fired, so suppress the swap. Don't "simplify" that
  branch.
- **The LOG thread is the consumer and the writer.** There is one scheduler
  thread; it drains the wheel and does the `write`s. Call `Logging::finish()` at
  shutdown before the statics tear down, or pending async lines can outlive their
  sinks.
- **`make_format_args` needs lvalues.** `format_msg` passes `args...` (not
  `std::forward`ed) into `std::make_format_args`. Do not change it to
  `std::forward<Args>(args)...`; that reintroduces the rvalue temporaries it
  rejects.
- **`L_EXC` undefs a stub.** stash and scheduler bundle a no-op `L_EXC` (and
  `L_NOTHING` / `L_DEBUG_HOOK` / `L_CALL`) so they build without a logger. The
  macro block `#undef`s `L_EXC` before defining the real one. Keep that undef.
- **`config` / `hooks` / `handlers` are read unsynchronized.** Install handlers
  and set config before logging starts. Mutating them while the LOG thread runs
  is a data race (see docs/ROADMAP.md).
- **A refused line keeps its meaning.** `Logging::add` checks the scheduler's
  `add` result. On false (shutdown, or past the 24 h horizon) it undoes
  `scheduled` and the `pending` increment, then: a future `clears` watchdog stays
  armed with `refused_deadline`, and its cancel point (`clean()` or `clear()`)
  writes it only if the deadline has passed; a line already due, or a severe one
  (priority above `NOTICE`), is written inline now; a routine future line that
  does not clear is lost (no cancel point). Never write a refused watchdog early:
  it would log every fast operation. A refusal that `post` can report is handled this way
  (`stopping`; a wakeup more than an hour out past the wheel's span). One that happens while the
  LOG thread files the line (an allocation failure, or a wakeup under an hour out on a wheel the
  LOG thread has not walked for most of a day) cannot reach the poster: the LOG thread runs the
  line if it is due and drops it otherwise, so a severe future line refused there is not
  written inline.
- **Indent is captured at creation, on the producer thread.** `log_indent()` is
  `thread_local`; a line records the depth when it is built (on the producer),
  not when it fires (on the LOG thread). Keep the capture in the `Logging`
  constructor.
- **The producer path allocates and shares as little as it can; keep it that way.** A routine
  line is formatted into the thread's reusable buffer (`logger_detail::FormattedMsg`), copied
  into one `Logging` record, and posted with one compare-exchange. Do not add a clock read, a
  lock, a shared counter write, a thread-local access or an allocation to it without measuring
  `bench/steady.cc` (a line costs about 75 ns on an M1 Pro; each of those is 3-20 ns).
- **`FormattedMsg` renders the bytes `format_msg` renders.** Its fast path takes only fields
  that are exactly `{}` (plus `{{` and `}}`) over integers, `bool`, `char`, `std::string`,
  `std::string_view`, `const char*` and char arrays, and declines everything else (a spec or an
  index, a null pointer, a NUL inside an array, a malformed format) to `std::vformat`, which also
  decides what an error looks like (the format string itself). `test/format.cc` checks every
  accepted message against `std::vformat`; extend its generators when you widen the fast path.
- **A line due within a tick of its call is posted ordered.** `Logging::add_line` posts a line
  whose wakeup is at or before `now + kCoalesce` with `post_ordered` (its wakeup is exactly
  `now + kCoalesce`, so one thread's wakeups never decrease, which is what the scheduler's arrival
  queue wants of a poster); every other line goes through `post` into the wheel. Across threads
  the wakeups can fall out of order by the gap between a thread's clock read and its push, so a
  line may wait behind a later-due head, by up to that gap. The guarantee is the one `test/order.cc`
  states: of two lines whose logging is ordered (one thread, or one thread's push completing before
  the other's clock read) and due in that order, the first is written first. Lines of unordered
  threads can leave due order by that gap. A poster that could post a wakeup earlier than its own
  earlier ordered post breaks the queue's order.
- **`pending_count()` is a difference.** Producers add to a striped `posted_` counter (a
  cache line per stripe), the LOG thread adds to `completed_` when a record is destroyed (a
  release add, so a reader that sees the count drained sees the writes), and `pending_count()`
  is the sum less `completed_`. Do not put a shared counter back on the producer path, and keep
  `dropped_`, `completed_` and each stripe on their own cache lines.
- **The wall-clock stamp is derived.** `logger_detail::system_at(steady_now())` adds the offset
  between the clocks, measured again every 20 ms; it is exact to the drift over 20 ms, not to the
  nanosecond. `Logging::do_log` (the public entry point) still reads both clocks.
- **`steady_now()` agrees with `steady_clock::now()`.** On macOS it reads `mach_continuous_time`
  after a check at start-up that it matches `steady_clock` to a millisecond; the scheduler and the
  refusal path compare against `steady_clock::now()`, so a clock that did not match would put
  every line in the wrong tick.

