# Changelog

All notable changes to `logger` are recorded here, newest first. The format
follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

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
