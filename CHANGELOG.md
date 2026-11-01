# Changelog

All notable changes to `logger` are recorded here, newest first. The format
follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## 2026-10-31 (producer path)

### Changed

- One `L_INFO` costs a producer thread about 75 ns on an Apple M1 Pro (470 ns before) and about
  225 ns on an Intel i9-9980HK (715 ns before); a synchronous fmt-and-mutex sink with no I/O costs
  35 and 55 ns. Measured under `quiet strict` with `bench/steady.cc` (old and new alternated, three
  runs each, twice: 469 to 477 against 73.5 to 77 ns on the M1 Pro, 714 to 717 against 224 to 225 ns on the
  laptop) and `bench/lg.cc`. What changed:
  - The record is handed to the LOG thread with one compare-exchange (the scheduler's `post()` and
    `post_ordered()`); the LOG thread files it. The producer no longer does the wheel insert, whose
    chain walk cost thousands of hops in a 1 ms slot holding a busy thread's lines. Lines due within
    1 ms of their call wait in an arrival queue, ordered, and never enter the wheel.
  - The macros format into a per-thread reusable buffer (`logger_detail::FormattedMsg`): plain `{}`
    fields over integers, `bool`, `char` and strings are written directly, and everything else goes
    through `std::vformat`. `test/format.cc` checks every message against `std::vformat`.
  - The pending count is a striped producer counter and a consumer-owned completed counter, not one
    atomic both sides write. `pending_count()` is their difference.
  - A call reads the steady clock once and derives the wall-clock stamp from it (the offset between the
    clocks is measured again every 20 ms); on macOS the steady read is `mach_continuous_time`, checked
    against `steady_clock` at start-up. The thread's id and indent come from one thread-local block, and
    the common path builds no `std::exception_ptr`.
  - The scheduler pin moves to the commit with `post()`.
- The wall-clock stamp is exact to the drift over 20 ms (within 2 us of `system_clock` at the default
  microsecond precision), not to the nanosecond, and a step of the wall clock shows at the next
  re-measurement.
- A refusal while the LOG thread files a line (an allocation failure, or a wakeup under an hour out on a
  wheel the LOG thread has not walked for most of a day) no longer reaches the poster: the LOG thread runs
  the line if it is due and drops it otherwise. A line that races `finish()` is refused, or filed
  and written if it is due; it is not left on a stack nobody drains.
- Ordered posts are ordered per thread. Across threads a line may wait behind one due microseconds
  later; the guarantee between lines whose logging is ordered is unchanged (`test/order.cc`).

### Added

- Tests: `test/format.cc`, `test/producer.cc` (exactly once from many threads, order across threads,
  the counters, a `finish()` that races the producers), `test/clock.cc`, rounds in `test/order.cc` in
  which the LOG thread is busy while lines are logged, and `test/transcript.cc` with `test/frozen/`, a
  copy of the implementation before this change: one scripted run must write the same bytes through both.
- Benchmarks: `bench/parts.cc`, `bench/steady.cc`, `bench/drain.cc` (with `-DLOGGER_BUILD_BENCH=ON`).

## 2026-10-31

### Changed

- A line's producer cost and timestamp cost drop. On Apple silicon, one producer
  thread pays about 235 ns per `L_INFO` with the LOG thread idle (310 ns before),
  a line goes from call to written in about 460 ns (670 before), and four
  producers pay about 360 ns each (610 before). Three changes:
  - The LOG thread used to be signalled for every async line. An async line due
    within one wheel tick (1 ms) is now scheduled 1 ms after the call, so the LOG
    thread sleeps to that time and the lines queued meanwhile need no signal. A
    hard crash within that millisecond loses the line; it used to lose it within
    the signal latency.
  - The default timestamp used to run `localtime_r` and about eight `snprintf`
    calls per line on the LOG thread. It now caches the broken-down local time
    per thread per whole second, keyed on the `TZ` value (macOS re-reads `TZ` in
    every `localtime_r`), and writes digits as integers. Years outside 0..9999,
    negative sub-second parts, and epochs past ten digits keep `snprintf`. Output
    text is byte-identical: a differential test compares the formatter with a
    frozen copy of the old `snprintf` code over 460k random and DST-edge times.
  - `do_log` reads the steady clock once instead of three times.
- The scheduler pin moves to the commit that skips its own signal while its
  thread is awake, then to `36afada`, which fixes a task that could wait out the
  worker's 30 s idle target after an `add` that raced the worker's sleep.
- `Logging::add` checks the scheduler's `add` result. On a refusal (after
  `finish()` began, or a delay past the 24 h horizon) it undoes `scheduled` and
  `pending` and handles the line by kind, as ARCHITECTURE.md describes.

### Fixed

- The 1 ms shift skipped `clears` lines, so a `clears` line due 0.3 ms out was
  written before an immediate line logged earlier. The shift is
  `max(due, now + 1 ms)` for every scheduled line. `test/order.cc` checks about
  2.7 M ordered pairs over 40 rounds of 400 mixed lines; the old code fails in
  round 0.
- The timestamp test's far-future epoch cases overflowed a nanosecond
  `system_clock` (past about 9.2e9 seconds), which UBSan reports. They run only
  where the clock is coarser, and the test polls instead of sleeping fixed times.

## 2026-10-28

### Added

- Standalone extraction from Xapiand, with bounded backpressure
  (`LogConfig::max_pending`), the bounded two-set `once` filter, the
  `thread_local` stacked indent, hooks for exceptions, backtraces, thread names,
  and timestamps, and the iTerm2 integration in the core. `Logging::finish()`
  restores the terminal as Xapiand's logger did.
