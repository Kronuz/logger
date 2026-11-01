# Status: perf/logger-producer

Workstream: logger producer path. Branch `perf/logger-producer` (base `main` 5299eb3), with `perf/scheduler-producer` in the scheduler
(base `main` 5fa44d1); both are merged into `main`.

## Done criteria

1. One `L_INFO` under steady load costs 100 ns or less on the producer (405 ns before on the M4;
   the synchronous fmt-style logger with a no-op sink is 29 ns), on arm64 and the x86 laptop.
2. Output bytes, ordering, `finish()` drain and the inline path for severe lines unchanged: a
   differential test against a frozen copy of the old implementation, an ordering oracle, TSan
   and a stress run on the Linux VM.
3. Scheduler and logger landed with CI green; the logger's scheduler pin names the scheduler
   commit.

## State

Criterion 1 is met on arm64 and not on the x86 laptop. Criteria 2 and 3: see Done.

| ns per `L_INFO`, one producer | before | after | comparator |
|---|---|---|---|
| Apple M1 Pro, `bench/steady.cc`, 3 runs each, twice | 469 to 477 | 73.5 to 77.0 | 35 |
| Apple M1 Pro, `bench/lg.cc`, median (min), two runs | 505 to 508 (477) | 95 to 113 (80) | 35 |
| Intel i9-9980HK, `bench/steady.cc`, 3 runs each, `quiet strict` | 714 to 717 | 224 to 225 | 55 |
| Intel i9-9980HK, `bench/lg.cc`, median (min) | 708 (691) | 235 (230) | 55 |

Four producers (`bench/lg.cc`, median per call): M1 Pro 307 to 131 (min 85), laptop 287 to 84.
The M4 Pro was not measured: its `quiet` queue never cleared. The M1 Pro has no `quiet`; its load
was 1 to 2 during the runs, and the A/A spread of `bench/steady.cc` is under 2%.

The x86 laptop's cost is allocation and clocks: `bench/parts.cc` there prices the record
(`make_shared`) at 56 ns, a 60-byte string at 84 ns, a steady-clock read at 20 ns (two per call),
and the sum of two clocks, a record and the formatting, with no queue at all, at 220 ns. Reaching
100 ns there needs a record pool and the message stored in the record (no allocation per line),
which is a change to the record's lifetime across threads and should be designed and reviewed on
its own.

## Done

- Profile of the old producer path (M1 Pro, `sample`): the wheel insert with its chain walk, a
  heap box per insert, the shared `pending` counter, six clock reads and a `std::vformat` into a
  fresh string.
- Scheduler (`perf/scheduler-producer`, 5ad60fc, c01a9e3, 2c7345f): `post()` and `post_ordered()`
  hand a task to the LOG thread with one compare-exchange; the LOG thread files them, and merges
  the arrival queue with the wheel by filing tick.
- Logger (`perf/logger-producer`): the per-thread formatter and its fast path, the derived
  wall-clock stamp, one thread-local block, striped counters, `post`/`post_ordered` routing.
- Differential test: `test/transcript.cc` against `test/frozen` (5299eb3): identical bytes. Order:
  `test/order.cc` including rounds with a stalled LOG thread (fails against the scheduler before the
  filing-tick fix). `test/producer.cc`, `test/format.cc`, `test/clock.cc`, scheduler `test/post.cc`.
- Sanitizers: CI ASan/UBSan and TSan green on both repos (the transcript test is skipped under
  TSan: the frozen library's relaxed counters race with the test's configuration writes by
  construction). On the Linux VM (clang 12, `setarch -R`): logger order, format, producer and
  timestamp under TSan with no report; UBSan and the plain build on the M1 Pro.
- Independent architect review (`docs/reviews/2026-10-31-logger-producer-handoff.md` in cpp-libraries): APPROVE WITH
  CONDITIONS; LPH-R1 to R7 folded in (close on every exit, no task code under the mutex,
  `pending_count()` read order, seq_cst wakeup load, bad_alloc in the arrival queue, a test for
  each, the cross-poster ordering contract documented and tested), then a re-check: R1 to R6
  closed; R7 closed by the `peep_sleep_stale` test (it fails on the review's `no_has_posted`
  mutant: the look at the stack before the sleep is needed, not an optimisation); R10 (the
  `producers` test took 30 s under TSan because 200000 tasks due within 2 ms walk one slot's
  chain; its due times now spread over 64 ms) and R11 (doc names) done.

## Behavior differences (owner decisions)

- The wall-clock stamp is derived: exact to the drift over 20 ms (within 2 us of `system_clock`).
- A refusal while the LOG thread files a line does not reach the poster (see CHANGELOG).
- `Logging::age()` prefers `cleaned_at` over `cleared_at`, so a handle dropped at once hides the
  " ~Nms" annotation of a late line. Unchanged, reported: it looks unintended.
- Lines of unrelated threads can leave due order by the gap between a clock read and the push.

## Next

1. Record pool and inline message storage, to bring the x86 laptop under 100 ns (design first).
2. A `quiet strict` run on the M4 Pro when its queue allows.
3. Re-run the VM's ASan build (the first attempt had a flag bug).

## Blocked on

Nothing owned by this workstream.
