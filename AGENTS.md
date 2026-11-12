# AGENTS.md

Notes for agents and contributors working in this repo. For internal design see
ARCHITECTURE.md; for usage see README.md and docs/API.md; for open design work see docs/ROADMAP.md.

## Repo map

- `logger.h` — the public API: `LogConfig`, `LogHooks`, the `Logger` sink
  interface (`StreamLogger` / `StderrLogger` / `SysLog`), `Logging` (the log
  record, which is also a `wheel::timer_entry`, plus the static facade), the `Log`
  handle, and the `L_*` macro surface. Also the `format_msg` / `print_msg`
  helpers, the `log_indent()` / `LogIndent` thread-local indent, and the
  producer's half of the per-thread queues (`logger_detail::queue_line`, the
  record layouts, `ThreadState`).
- `logger.cc` — implementations: the queues' out-of-line half (`ring_attach`,
  `ring_room`, `ring_published`), the LOG thread (`LogWorker`), the sinks, the three-path `add()` router,
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
- `test/queue.cc` — the per-thread queues under 1 to 16 producers: the wrap at every offset
  (`logger_queue_test wrap` alone), exactly once and in order, order across threads and by due
  time, watchdogs armed and dropped, backpressure, swaps, the sleep handshake, short-lived
  threads, exit without `finish()`, and `finish()` racing the producers.
- `test/transcript.cc` + `test/transcript.cmake` + `test/transcript.golden` — one scripted run
  whose bytes must equal the golden file, which the implementation before the producer path
  changed (`5299eb3`) wrote. Edit the golden only for an intended change to what is written.
- `test/clock.cc` — `steady_now()` and `system_at()` against `steady_clock` and `system_clock`.
- `test/wake.cc` — an isolated line is written at once, and one logged while a line is pending
  waits for the 1 ms batch.
- `test/order.cc`, `test/timestamp.cc`, `test/tint.cc` — the ordering oracle, the timestamp
  formatter against its frozen `snprintf` form, and the tint/collapse paths.
- `bench/lg.cc` (`logger_bench`) — producer cost against a synchronous fmt + mutex logger and,
  with `-DLOGGER_BENCH_QUILL=ON`, quill, with a same-text check (`--check`, a ctest); `bench/parts.cc`
  (each piece of a call alone), `bench/steady.cc` (a profiling loop), `bench/drain.cc` (the LOG
  thread's drain over time), `bench/latency.cc` (an isolated line from the call to its handler).
  Built with `-DLOGGER_BUILD_BENCH=ON`.
- `CMakeLists.txt` — FetchContents `threadpool`, `wheel`, `term-color` and `cppcodec`, builds the static library, and
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
- The core depends on `Kronuz/threadpool`, `Kronuz/wheel` and `Kronuz/term-color` (the palette plus
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
- **The LOG thread is the consumer and the writer.** There is one LOG thread
  (`LogWorker`); it drains the queues and does the `write`s. Call `Logging::finish()` at
  shutdown before the statics tear down, or pending async lines can outlive their
  sinks.
- **`make_format_args` needs lvalues.** `format_msg` passes `args...` (not
  `std::forward`ed) into `std::make_format_args`. Do not change it to
  `std::forward<Args>(args)...`; that reintroduces the rvalue temporaries it
  rejects.
- **`L_EXC` undefs a stub.** stash bundles a no-op `L_EXC` (and
  `L_NOTHING` / `L_DEBUG_HOOK` / `L_CALL`) so they build without a logger. The
  macro block `#undef`s `L_EXC` before defining the real one. Keep that undef.
- **`config` / `hooks` / `handlers` are read unsynchronized.** Install handlers
  and set config before logging starts. Mutating them while the LOG thread runs
  is a data race (see docs/ROADMAP.md).
- **A refused line keeps its meaning.** `Logging::add` checks `LogWorker::push_task`'s
  result. On false (`finish()` began, a delay of a day or more, or no memory) it undoes
  `scheduled`, then: a future `clears` watchdog stays armed with `refused_deadline`, and its
  cancel point (`clean()` or `clear()`) writes it only if the deadline has passed; a line already
  due, or a severe one (priority above `NOTICE`), is written inline now; a routine future line
  that does not clear is lost (no cancel point). Never write a refused watchdog early: it would
  log every fast operation. A routine line the fast path refuses (`queue_line` returns 0) takes
  this path as a full record.
- **Indent is captured at creation, on the producer thread.** `log_indent()` is
  `thread_local`; a line records the depth when it is built (on the producer),
  not when it fires (on the LOG thread). Keep the capture in the `Logging`
  constructor.
- **The producer path takes no lock, allocates nothing and does no read-modify-write; keep it
  that way.** A routine line is formatted into the thread's own queue, stamped with one clock
  read, and published with one store; the only shared data it writes is its queue. Do not add a
  clock read, a lock, a shared write, a read-modify-write, a thread-local access or an allocation
  to `queue_line` without timing `bench/lg.cc` on both machines (a line costs about 27 ns on an
  M1 Pro; each of those is 3 to 20 ns).
- **`FormattedMsg` renders the bytes `format_msg` renders.** Its fast path takes only fields
  that are exactly `{}` (plus `{{` and `}}`) over integers, `bool`, `char`, `std::string`,
  `std::string_view`, `const char*` and char arrays, and declines everything else (a spec or an
  index, a null pointer, a NUL inside an array, a malformed format) to `std::vformat`, which also
  decides what an error looks like (the format string itself). `test/format.cc` checks every
  accepted message against `std::vformat`; extend its generators when you widen the fast path.
- **A line due within a tick of its call is due 1 ms after it, unless it starts a burst.**
  A line that is not `clears`, whose thread queued no such line in the last 1 ms
  (`ThreadState::last_soon`), and that finds `pending_count()` at 0 is due at its call, and the
  LOG thread is woken for it, so an isolated line is written at once. Keep all three conditions:
  without the pending check a thread's first line after a pause signals the LOG thread even while
  it has lines to write, without the gap one thread's due times can decrease (the LOG thread reads
  each queue in order, so a line due before the one ahead of it would wait for it), and a `clears`
  line needs the full tick for its handle to cancel it. `test/wake.cc` fails if the exception is
  gone, or if it fires while a line is pending.
- **The LOG thread takes only lines stamped before its pass began.** `LogWorker::pass(gate, ...)`
  merges the queues' heads by (due, call time) and skips a head stamped after `gate`, its own clock
  read before it looked. Without the gate a line could be written ahead of one logged before it on
  another thread and not yet published (`test/queue.cc`'s order tests). Lines of two threads
  stamped within one tick of the stamp clock can come out in either order (41.67 ns on Apple
  silicon); that is the documented limit.
- **Publish, then read the LOG thread's state, both sequentially consistent.** The producer stores
  its write index and then loads `worker_state`; the LOG thread sets `kSleeping` (or `kClosed`),
  fences, and looks at the queues once more. Either it sees the line or the producer sees it
  sleeping (and wakes it if the line is due before `worker_wake`) or closed (and writes the line
  itself unless the last look took it). Weakening either side loses a wakeup or a line at
  `finish()`; the sleep handshake and finish race in `test/queue.cc` catch it.
- **A wrap is only a record head, and needs room for the padding and the record.** `ring_room`
  pads to the end of the buffer with an 8-byte `RecordHead` written with `memcpy`, whatever room is
  left, and wraps only if the room (up to the published read index) holds the padding and the record;
  otherwise it moves to a bigger buffer. A placement `new` of a `LineRecord` there (whose
  `std::source_location` member's constructor writes) overran the buffer by up to its header, and a
  wrap with room for the record alone writes it over unread lines; `logger_queue_test wrap` (its
  every-offset and boundary tests, under ASan) catches both.
- **`pending_count()` is a difference.** Each queue counts its own lines in `posted` (only its
  thread writes it), `completed_` counts what left the queues (the LOG thread adds its written lines
  once per pass, a record adds itself as it is destroyed; release adds, so a reader
  that sees the count drained sees the writes), and `pending_count()` is the sum less
  `completed_`. Do not put a shared counter on the producer path, and keep `dropped_`,
  `completed_`, `worker_state` and each queue's producer and consumer halves on their own 128 bytes.
- **The wall-clock stamp is derived.** `logger_detail::system_at(steady_now())` adds the offset
  between the clocks, measured again every 20 ms; it is exact to the drift over 20 ms, not to the
  nanosecond. `Logging::do_log` (the public entry point) still reads both clocks.
- **`steady_now()` and the stamps agree with `steady_clock::now()`.** On macOS `steady_now()` reads
  `mach_continuous_time` after a check at start-up that it matches `steady_clock` to a millisecond,
  and a queued line's stamp is that clock's raw count, which `stamp_time` converts exactly; the LOG
  thread and the refusal path compare against `steady_clock::now()`, so a clock that did not match
  would put every line at the wrong due time.

