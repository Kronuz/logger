# Changelog

All notable changes to `logger` are recorded here, newest first. The format
follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

Upgrading a pin from before 2026-10-30: [the consumer upgrade note](https://github.com/Kronuz/cpp-libraries/blob/main/docs/UPGRADING-2026-10.md#logger) lists what changes for callers, how to find affected inputs, the commits not to pin, and the speedup.

## 2026-11-01 (per-thread queues)

### Changed

- A routine line costs its thread no lock, no allocation and no read-modify-write. Each thread
  that logs owns a queue, a byte ring it formats its lines into and publishes with one store; the
  LOG thread is its own loop that merges the queues in call order (by due time, then call time,
  taking only lines stamped before each pass began) and keeps deferred lines in a heap. Before,
  each line was a `Logging` record allocated by the caller and posted to the scheduler's wheel
  with a compare-exchange, and the scheduler's thread wrote it. The stamp is one raw read of the
  stamp clock (`mach_continuous_time` on macOS), converted on the LOG thread. The due rules are
  unchanged (a line due within a tick is due 1 ms after its call unless it starts a burst with
  nothing pending), as are `L_DELAYED_*`, `clear()`/`unlog()`, `max_pending` and its summary line,
  `finish()` and the exit flush. A routine line's handle keeps its stamp and call site, so
  `unlog()` on it still writes the swap after the line when the handle drops; `clear()` on it
  returns false, as it does for a line already written. Lines of two threads stamped within one
  tick of the stamp clock (41.67 ns on Apple silicon) can come out in either order.
  Measured in one `quiet strict` hold per machine with `tools/ab_compare.py` (5 rounds, A/A on the
  first side), START (`ce27b90`) and main (`a82454b`) against this commit, ns per call (latency in
  us), the median of the runs, and the ratio (A/A):

  | Row | M4 Pro START | main | now | main / now (A/A) | i9 START | main | now | main / now (A/A) |
  |---|---:|---:|---:|---:|---:|---:|---:|---:|
  | burst, 1 producer | 254.2 | 53.3 | 22.7 | 2.34x (0.99) | 636.3 | 184.2 | 59.3 | 3.06x (0.99) |
  | back to back, 1 producer | 225.4 | 49.0 | 21.0 | 2.31x (0.97) | 590.7 | 199.9 | 83.6 | 2.39x (1.00) |
  | end-to-end, 1 producer | 624.8 | 318.2 | 271.2 | 1.14x (1.02) | 1477.4 | 652.8 | 541.4 | 1.23x (1.00) |
  | burst, 4 producers | 439.2 | 123.5 | 22.1 | 5.69x (0.99) | 731.8 | 246.7 | 65.5 | 3.65x (1.02) |
  | back to back, 4 producers | 181.7 | 140.4 | 6.10 | 22.7x (0.99) | 198.3 | 75.1 | 23.4 | 3.20x (0.98) |
  | end-to-end, 4 producers | 652.8 | 327.5 | 256.7 | 1.28x (0.99) | 1501.6 | 714.9 | 512.8 | 1.39x (1.00) |
  | `bench/drain.cc` produced | 261.7 | 62.3 | 22.9 | 2.77x (0.98) | 647.8 | 237.2 | 85.5 | 2.77x (1.01) |
  | `bench/drain.cc` drained | 709.3 | 391.8 | 288.1 | 1.36x (1.03) | 1503.8 | 642.2 | 526.5 | 1.24x (1.03) |
  | latency median (us) | 18.4 | 14.1 | 13.7 | noise (1.15) | 62.7 | 46.5 | 40.4 | noise (0.94) |
  | latency p90 (us) | 40.6 | 38.3 | 38.5 | noise (1.00) | 99.0 | 80.4 | 71.6 | noise (0.84) |

  The rows are `bench/lg.cc` as it is now (its pauses spin, and fmt + mutex runs through the same
  harness), built against each side. Against START, every row is faster on both machines
  (one producer in bursts 11.3x on the M4 Pro and 10.6x on the i9-9980HK, end-to-end 2.3x and
  2.8x, latency median 1.35x and 1.55x). Against main every row is faster, or within the A/A noise
  (the latency rows). Against fmt + mutex in the same holds, one producer pays less on the M4 Pro
  in both rows (22.7 against 28 ns in bursts) and on the i9 in bursts (59 against 62 to 68 ns), and
  more on the i9 back to back (83 against 61 to 63 ns, logger#1); four producers pay a half to a
  thirteenth.

### Fixed

- The queues' first version corrupted the heap: the wrap at the end of a buffer was written as a
  whole line header (`new (p) LineRecord`), and `LineRecord`'s `std::source_location` member's
  constructor wrote past the bytes left. The wrap is an 8-byte record head now.

### Added

- `test/queue.cc` (`logger_queue`): the queues under 1 to 16 producers; the wrap at every offset
  short of a header (`logger_queue_test wrap`; under ASan the old wrap reports a heap-buffer-overflow),
  exactly once and in order, order across threads and by due time with records due 0 to 3.5 ms out,
  watchdogs armed and dropped from 16 threads, backpressure, swaps, the sleep handshake,
  short-lived threads, exit without `finish()`, and `finish()` racing 16 producers.
- `bench/lg.cc`: quill v13.1.0 as a comparator (`-DLOGGER_BENCH_QUILL=ON`, fetched by the bench
  only), a same-text check over every logger (`logger_bench --check`, the `logger_bench_check`
  test), and fmt + mutex through the same burst and back-to-back harness. The pauses between
  bursts spin: a sleeping pause timed the core's clock ramp, five times the cost for fmt + mutex
  too on an M1 Pro.

## 2026-11-01 (isolated lines)

### Changed

- An isolated line is written at once again. Since the 2026-10-31 change below, a line due within
  1 ms of its call was due 1 ms after it, so a single `L_INFO` with nothing else queued waited for
  the next tick: about 1.26 ms on an Apple M4 Pro and 1.51 ms on an Intel i9-9980HK from the call to
  the handler, against 19.7 and 55.6 us before (`ce27b90`; the gate's numbers). Now a line that is
  not `clears`, whose thread posted no line due within a tick in the last 1 ms, and that finds
  nothing pending (`pending_count()` is 0) keeps its own due time, and the LOG thread is woken for
  it; every other line due within a tick is due 1 ms after its call, as before, so a burst is written
  in 1 ms batches with no signal per line. A thread in a burst reads no counters (the 1 ms gap is
  checked first), and the gap keeps each thread's ordered wakeups non-decreasing. Ordering is
  unchanged: a line that finds nothing pending comes after every line logged before it, and lines
  logged after it count it as pending.
  Measured with `bench/latency.cc` (one isolated `L_INFO` every 5 ms, 300 lines, call to handler)
  in one `quiet strict` hold per machine, interleaved with `tools/ab_compare.py` (5 rounds, A/A on
  START), median (90th percentile) in us:

  | Machine | START (`ce27b90`) | before (`8c21862`) | after | after / START |
  |---|---:|---:|---:|---:|
  | Apple M4 Pro | 21.7 (49.3) | 1507 (1514) | 16.8 (46.4) | 1.30x (1.12x) faster |
  | Intel Core i9-9980HK | 58.5 (96.4) | 1514 (1562) | 42.5 (80.8) | 1.28x (1.22x) faster |

  The throughput benches against `8c21862` in the same holds (the ratio before / after, with its
  A/A ratio): every `bench/lg.cc` row, `bench/drain.cc` and `bench/steady.cc` are within the A/A
  noise. On the M4 Pro the one-producer draining row reads 0.918 (A/A 0.909) and the rest 0.99 to
  1.07; on the i9 the four-producer draining row reads 0.966 (A/A 0.993, range 0.94 to 1.08) and
  the rest 0.99 to 1.05.

### Added

- `test/wake.cc`: an isolated line with nothing pending is written within 900 us (median of 15), and
  one logged while a watchdog is held waits for the 1 ms batch. With the change reverted the first
  check fails (median 1515 us on an Apple M1 Pro); without the pending check (a mutant) the second
  does (33 us).
- `bench/latency.cc` (`logger_latency`): the time from an isolated `L_INFO` to its handler, 300 lines
  5 ms apart, median and 90th percentile.

## 2026-11-01

### Tests

- `test/order.cc` pins the two exceptions to the ordering guarantee that the README
  states (audit L1). A line more severe than `LOG_ERR`, logged with `L_CRIT` while a
  routine line due 300 ms out waits in the wheel, is written on the calling thread, ahead
  of it, and the routine line is written by the LOG thread. After `finish()` has begun
  (the LOG thread is stalled inside its first write so the drain is still running), a
  line due at once is written on the calling thread, every line is written exactly once,
  the LOG thread writes its lines in the order they were logged, and the 200 lines logged
  before `finish()` are all drained. The capture records the writing thread of each line.

## 2026-10-31 (audit fixes)

### Changed

- The scheduler pin moves to 34631ec: the worker walks at once when a pass finds a line due
  (no mutex, no `sleeping` for producers to signal), files the posted stack once per such pass,
  leaves `ending` unwritten until `finish()`, `add()` fences before it reads the published wakeup
  time, and stash's leaf inserts resume at the furthest node claimed. The LOG thread files a
  backlog of overdue lines in linear time instead of quadratic.
- `cppcodec` is fetched at a commit (8019b8b, the one its `master` names) instead of at `master`.
- The README states the producer cost measured with `bench/lg.cc` on an Apple M4 Pro (one producer:
  65 to 66 ns with the LOG thread idle, 55 to 69 ns while it drains; four producers: 158 to 172 ns)
  and on an Intel i9-9980HK (387 to 402, 223 to 228 and 81 to 85 ns),
  and scopes the ordering guarantee to lines that go through the LOG thread: lines more severe than
  `LOG_ERR`, and lines due at once that are logged after `finish()` begins, are written at the call
  and can overtake queued ones (audit L1).

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
