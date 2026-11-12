# Logger on its own queues vs on Kronuz/wheel

Whether logger's scheduling costs less on `wheel::timer_wheel` than on what it runs on today, for
each of its three patterns, decided by the rule below (Detent PLAN.md D72, condition 2: the
replacement is proven better on every current use). The harness is `bench/wheel_ab/`.

## The two backends

- **(A) logger as it is**, at this branch's base (scheduler pinned at `d57e8fc`). A routine line
  is formatted into its thread's byte queue and published with one store; a deferred line is a
  `Logging` record (`std::make_shared`) referenced from the queue, which the LOG thread files in
  a binary heap by due time; `clear()` is the record's one-shot compare-exchange. The LOG thread
  sleeps on a condition variable until the earliest due line. Of Kronuz/scheduler, logger uses
  only `ScheduledTask`'s one-shot cancel and `Thread`; neither the scheduler's stash wheel nor
  its thread is used (ARCHITECTURE.md).
- **(B) a prototype on the wheel**, `wheel::timer_wheel` at `Kronuz/wheel@50665c8`, owned by a
  LOG thread of the prototype's own. A line is a 128-byte record (the 32-byte `timer_entry`,
  state, and the message inline) taken from the logging thread's record pool and handed over
  with `arm_remote(deadline)`; the LOG thread's `expire(now)` writes it and returns the record
  to its pool. The prototype keeps (A)'s policy where it is policy, not mechanism: the same due
  rule for a routine line (due at its call when it starts a burst and nothing is pending, else
  1 ms after it), the same sleep handshake (state word, wake time, sequentially consistent
  fences, `std::condition_variable`), and the same fan-out to the sinks (`Logging::log`).
  A worker cannot cancel a wheel entry, so the slow-op clear is a flag the fire checks: one
  `fetch_or` on the record's state by the worker, and the record stays filed until its deadline,
  when the fire discards it. No new wheel API.

Both run the same workload in the same binary, one backend per process.

## Workload

Each run is one process: one backend, one pattern, P producer threads (P = 1, 4, 14 on a host
with 16 hardware threads). The message is a fixed 16 bytes (the line's 8-byte id and 8 fixed
bytes), copied, not formatted, into a sink that only counts it; the formatted row formats
`"value {} and {}"` with an int and `"x"` through logger's `fast_format` on both backends.
Decoration on the LOG thread is off (`with_timestamp = false`) on both.

1. **Immediate**: an `L_INFO` (B: a record armed at its due time).
2. **Deferred, fires**: `L_DELAYED(false, d, LOG_INFO, ...)` with d uniform in [100 ms, 1 s)
   from a hash of the line's id; never cleared.
3. **Slow-op, usually cleared**: `L_DELAYED(true, 200 ms, LOG_WARNING, ...)`; for 99 of 100 ids
   (by a hash of the id) the arming worker drops the handle at once, which cancels the line;
   the other 1% keep their handle until their line was written. One call is the arm plus the
   clear.

A round: each producer runs 10 bursts of 1,024 calls, spinning 5 ms between bursts (as
`bench/lg.cc` does); then every line leaves the scheduler (written, or cleared and released)
before the next round. A run is one warm-up round, discarded, then 8 measured rounds (3 for
pattern 2, whose rounds last a second). A run prints its start, warm-up, timed part and
teardown.

## Metrics

Per run, over its measured rounds:

- **Producer ns per call, median and p99**: each producer times blocks of 64 calls with
  `steady_clock`; a block's time / 64 is one sample; median and p99 over every block of every
  producer.
- **Heap bytes per call**: bytes requested from `operator new` on every thread (a counting
  replacement in the harness binary, one uncontended per-thread counter) / calls. Allocations
  per call alongside.
- **Fire lag p99**: for each written line, the sink's `steady_clock` reading minus the line's
  deadline (its block's start plus its delay; an upper bound on the true lag by at most the
  block's 64 calls). Microseconds.
- **LOG-thread CPU per call**: the LOG thread's user plus system time (`thread_info`) / calls.

## Rule

Committed before any measured run. Each row (a pattern at a P, and the formatted row) is one
`exclusive` job at a named commit, macOS: first one A/A pair (A, A), then six A/B pairs in
alternating order (AB, BA, AB, BA, AB, BA). For each metric, pair i gives the ratio
r_i = B_i / A_i; r is the median of the six. The A/A band of a metric is w = |A'/A'' - 1| from
the A/A pair.

- **Wheel win** at (pattern, P): r(median ns) <= 0.90 - w(median ns), and r(p99 ns) <= 1.10 +
  w(p99 ns), and heap bytes per call of B <= 1.10 times A's (a heap per call under 1 byte counts
  as 0 on either side).
- **Wheel loss**: r(median ns) >= 1.10 + w(median ns).
- Otherwise **no win**.
- A pattern is a wheel win when it is a win at P = 1, 4 and 14 (D72: better on every current
  use); a loss at any P keeps logger off the wheel for that pattern until a wheel change and a
  rerun under this rule.
- Fire lag p99 and LOG-thread CPU per call are reported for every row and called out wherever
  B is worse than 1.10 times A's beyond their band; they do not decide.
- Correctness comes first and gates every row: a check run per pattern, P and backend writes
  every line exactly once, none before its deadline (the sink's reading against a clock read
  before the call), and no cleared line; (B)'s check also passes under ThreadSanitizer.
  Per-thread order of immediate lines is counted and reported for both backends.

A rule changed after data is a Decisions row, and every row reruns under it.

## Verdict

Pending the rows.
