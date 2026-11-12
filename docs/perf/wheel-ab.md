# Logger on its own queues vs on Kronuz/wheel

Whether logger's scheduling costs less on `wheel::timer_wheel` than on what it runs on today, for
each of its three patterns, decided by the rule below (Detent PLAN.md D72, condition 2: the
replacement is proven better on every current use). The harness is `bench/wheel_ab/`.

Arms (C) and (D) (`bench/wheel_ab/c`, `d`) and `wheel-port.sh` are deleted with the scheduler (D72): their numbers stay in this file and `wheel-port.md`, and the sources in git at logger:dce2538.

## The two backends

- **(A) logger as it is**, at this branch's base (logger:dce2538). A routine line
  is formatted into its thread's byte queue and published with one store; a deferred line is a
  `Logging` record (`std::make_shared`) referenced from the queue, which the LOG thread files in
  a binary heap by due time; `clear()` is the record's one-shot compare-exchange. The LOG thread
  sleeps on a condition variable until the earliest due line. Of the retired Kronuz/scheduler, logger used
  only `ScheduledTask`'s one-shot cancel and `Thread`; neither its stash wheel nor its thread
  was used.
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

- **(C) logger with a wheel inside its LOG thread**, the shape that would ship: logger exactly
  as (A), its per-thread queues carrying every line to the LOG thread, except that the LOG thread
  files deferred lines in a `wheel::timer_wheel` it owns (owner `arm`; no `arm_remote`, no inbox)
  instead of its heap. Each pass's `expire(now)` hands back the due lines; the pass sorts that
  batch by (due, call time, filing order) and merges it with the queues' lines as the heap's top
  was merged, so due order and call order within a due time hold as in (A). The wheel's entry
  lives in the `Logging` record (no pool), the record keeps itself alive while filed, a cleared
  line stays filed and is dropped when its slot comes due (the cancel flag the fire checks, as
  the heap drops a cancelled top), and the wait is until `next()`. It is
  `bench/wheel_ab/c/logger.{h,cc}`, a copy of the library with only that change, built into
  `logger_wheel_ab_c`; the library is unchanged. (C) is an arm added after the rule was committed;
  it runs under the same rule, with C in B's place, on patterns 2 and 3. In pattern 1 no line
  touches a timer in (C), so (C) is (A) there and has no rows.

(A) and (B) run in one binary, one backend per process; (C) runs the same harness against its
copy of logger.

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
`bench/lg.cc` does); then every line leaves the logger (written, or cleared and released)
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
- **Lines held** (reported, added with (C)): every 1 ms a sampler reads the lines the logger
  holds (filed, cleared or not, as `pending_count()` counts them); its peak, and its integral per
  call in milliseconds, how long a call's line stays filed. For pattern 3 this is how long
  cleared lines stay filed in the wheel against the heap.

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
  Added with (C): due order of deferred lines is counted from a clock read before and after each
  call (a line written after one whose due is surely later); (C)'s check also requires both
  orders kept, as (A) keeps them.

A rule changed after data is a Decisions row, and every row reruns under it.

## Verdict

**Logger stays on its own queues and heap for every pattern.** (B) wins patterns 2 and 3 on
producer cost but loses pattern 1 and breaks logger's ordering, so it cannot ship; (C), the shape
that would ship, loses patterns 2 and 3 to (A) at every P. Every row is an `exclusive` job on an
Intel i9-9980HK (16 threads), macOS 15.8.1, Apple clang 17, `Kronuz/wheel@50665c8`; r is the
median of six paired ratios, w the A/A band, both from `run.sh row` / `row-c`.

### (A) against (B), at logger:91924df

| Pattern, P | median ns A / B | r (w) | p99 r (w) | heap B/call A / B | lag p99 us A / B | LOG CPU r | verdict | job |
|---|---|---|---|---|---|---|---|---|
| 1, P=1 | 39.1 / 69.7 | 1.844 (0.103) | 2.707 (0.161) | 0 / 0 | 1,439 / 2,539 | 1.26 | loss | 261112-035007-2f8fd3 |
| 1, P=4 | 42.7 / 65.7 | 1.522 (0.016) | 1.544 (0.218) | 0 / 0 | 1,698 / 2,662 | 1.12 | loss | 261112-035011-fbe2f5 |
| 1, P=14 | 54.1 / 73.0 | 1.357 (0.004) | 2.986 (0.230) | 0.6 / 0 | 5,258 / 3,966 | 0.84 | loss | 261112-035015-2210ff |
| 1, P=1, formatted | 62.5 / 115.2 | 1.822 (0.001) | 2.314 (0.529) | 1.6 / 0 | n/a | 1.55 | loss | 261112-035041-cd1116 |
| 2, P=1 | 175.5 / 112.9 | 0.638 (0.064) | 0.705 (0.654) | 225 / 48 | 247 / 1,130 | 0.31 | win | 261112-035018-1410b6 |
| 2, P=4 | 170.2 / 117.1 | 0.688 (0.125) | 0.818 (0.098) | 225 / 48 | 112 / 1,349 | 0.28 | win | 261112-035022-b1f5a9 |
| 2, P=14 | 246.1 / 143.2 | 0.581 (0.030) | 0.543 (0.038) | 225 / 48 | 62 / 1,150 | 0.24 | win | 261112-035026-0fd817 |
| 3, P=1 | 220.8 / 72.6 | 0.346 (0.016) | 0.757 (0.283) | 215 / 0.5 | 5,164 / 5,811 | 0.46 | win | 261112-035030-378568 |
| 3, P=4 | 230.0 / 67.2 | 0.292 (0.055) | 0.615 (0.075) | 215 / 0.5 | 6,082 / 6,806 | 0.35 | win | 261112-035034-adfe40 |
| 3, P=14 | 294.9 / 76.8 | 0.257 (0.018) | 0.376 (0.041) | 215 / 0.5 | 9,693 / 10,817 | 0.36 | win | 261112-035037-bda2f7 |

- **Pattern 1: wheel loss** at every P. The wheel puts 19 to 31 ns on each immediate call (an
  `arm_remote` and a record from the pool where (A) writes its byte queue and publishes it with one
  store), and its fire lag p99 is 1.5 to 1.8 times (A)'s at P = 1 and 4. In a whole formatted call
  (62.5 ns on (A)) the wheel adds 53 ns.
- **Patterns 2 and 3: wheel win** by the rule at every P, with p99 and heap far inside their
  bounds. Called out beyond their bands: fire lag p99 in pattern 2 is 4.5, 12 and 19 times (A)'s
  (1.1 to 1.35 ms against 0.06 to 0.25 ms, the wheel's 1 ms slot), and 1.16 times at pattern 3,
  P = 1.
- **(B) does not keep logger's order**, which the rule reports but does not gate on (B): its check
  counts 23,574 to 109,949 per-thread inversions of immediate lines, and 19,201 / 101,159 / 390,867
  due-order inversions of 30,720 / 122,880 / 430,080 deferred lines at P = 1 / 4 / 14
  (261112-040502-859c44). Lines from one thread land in different stripes' order, and a slot's
  lines come back in filing order, not due order. Shipping (B) changes what users read.
- **What (B) needed that the wheel lacks**: a caller-owned 128-byte record per line, kept alive
  until it fires (a per-thread pool and a Treiber stack to return records), because the wheel
  holds only an intrusive entry; a cancel flag the fire checks, because a worker cannot cancel an
  entry on the owner's wheel (no remote cancel), so a cleared line stays filed until its deadline;
  a `seq_cst` fence after `arm_remote` (its CAS is release) for the sleep handshake; and any
  ordering within a slot or across producers, which it does not have. Its pattern 2 and 3 wins come
  from that pooled 48-byte path against (A)'s `make_shared` record per deferred line, not from the
  wheel's filing: (C) below files in the same wheel and loses.

### (A) against (C), at logger:ef0bb6b

| Pattern, P | median ns A / C | r (w) | p99 r (w) | heap B/call A / C | lag p99 us A / C | LOG CPU r | held ms/call A / C | verdict | job |
|---|---|---|---|---|---|---|---|---|---|
| 2, P=1 | 190.0 / 287.2 | 1.546 (0.145) | 1.270 (0.176) | 225 / 305 | 287 / 1,370 | 0.46 | 551 / 552 | loss | 261112-040534-165b69 |
| 2, P=4 | 186.9 / 240.3 | 1.258 (0.186) | 1.130 (0.459) | 225 / 305 | 110 / 1,331 | 0.37 | 550 / 551 | no win | 261112-040540-dc3982 |
| 2, P=14 | 257.0 / 338.0 | 1.326 (0.003) | 1.238 (0.096) | 225 / 305 | 63 / 1,288 | 0.45 | 551 / 552 | loss | 261112-040546-2390a9 |
| 3, P=1 | 229.3 / 287.3 | 1.245 (0.044) | 1.631 (0.036) | 215 / 295 | 9,634 / 10,271 | 1.27 | 201 / 203 | loss | 261112-040552-6a1eea |
| 3, P=4 | 224.4 / 286.8 | 1.300 (0.022) | 1.435 (0.120) | 215 / 297 | 10,102 / 25,902 | 2.11 | 202 / 207 | loss | 261112-040557-766cf0 |
| 3, P=14 | 299.2 / 371.3 | 1.239 (0.039) | 1.414 (0.078) | 215 / 295 | 14,897 / 306,468 | 5.92 | 206 / 398 | loss | 261112-040603-2ef310 |

- **Pattern 2: wheel loss** (a loss at P = 1 and 14; at P = 4, r = 1.258 sits just under the
  loss line 1.286). The heap bound fails at every P: 305 bytes per line against a cap of 248.
  The LOG thread spends 0.37 to 0.46 of (A)'s CPU per line (no heap sift), but fire lag p99 is
  4.7 to 20 times (A)'s: `next()` is the end of the due line's 1 ms slot.
- **Pattern 3: wheel loss** at every P, p99 1.41 to 1.63 times and heap 1.37 times (A)'s. At
  P = 4 and 14 the LOG thread falls behind: fire lag p99 2.6 and 21 times (A)'s (306 ms at
  P = 14), LOG CPU 2.1 and 5.9 times, and a call's line stays filed 398 ms against 206 ms. Both
  hold the same lines (peak 143,360 at P = 14), so it is the per-line cost of keeping 143,000
  filed lines in the wheel, not their number: the heap keeps its keys in one 40-byte-per-line
  vector and touches a record once, when it pops; the wheel links through the records themselves
  (about 40 MB of them at P = 14, past this host's 16 MB L3) and touches each again when its
  level cascades and when it expires. That attribution is from the numbers, not a profile.
- **Order is kept**: 0 per-thread and 0 due-order inversions at every pattern and P
  (261112-040508-910d14), and TSan clean (261112-040521-982cc9).
- **What (C) needed that the wheel lacks**: a sorted batch per pass, because `expire()` hands a
  slot's lines back unsorted and a line already due when the LOG thread files it must bypass the
  wheel to keep due order; a self-reference (`shared_ptr`) per filed record, because the wheel
  does not own what it files; the entry inside the record with an owner pointer and the sort key
  (+80 bytes per record, which is the whole heap failure); and a cancel flag the fire checks, as
  in (B), so a cleared line stays filed until its slot.
- **Why (C)'s producer pays more though its producer code is (A)'s**: the only change on the
  producer's side is the record's size, `sizeof(Logging)` 184 to 264, which takes the
  `make_shared` block from 200 to 280 bytes, past the 256 bytes macOS serves from its nano
allocator. With the nano allocator off for both (`MallocNanoZone=0`, diagnostics, not verdict
rows), (A) rises to (C)'s cost and most of the median gap closes: pattern 3, P = 1 goes from
r 1.245 to 1.048 (w 0.079; A 272 ns, C 275 ns; 261112-055705-1b993a), pattern 2, P = 1 from
1.546 to 1.112 (w 0.038; A 241 ns, C 279 ns; 261112-055709-999bf4). So the 256-byte boundary is
most of (C)'s producer median gap, all of it in pattern 3; pattern 3's p99 (1.50 times) and
about 11% of pattern 2's median remain. To stay in the nano allocator, `sizeof(Logging)` must
stay at or below 240 bytes (the block adds 16), at most 56 bytes over (A)'s 184; the heap bound
is tighter: at most 22 bytes over.

### The wheel at 45f98eb

These rows ran at `Kronuz/wheel@50665c8`. From there to `45f98eb` the wheel reshaped `arm`'s
inlining, made an owner `arm` or `cancel` of an unarmed entry with a `next` (one queued behind
another `arm_remote` push) file every stripe, and changed only the destructor's firing. (C) never
calls `arm_remote`: its inbox is always empty and a new record's entry has no `next`, so the
change adds one load of `e.next` to an `arm` that writes that line anyway, and the drain never
runs. (C)'s deciding metrics (producer median, p99, heap) run no wheel code at all; only its LOG
CPU and fire lag could move, by nanoseconds per line. (B) runs 1 to 14 producers, each on its own
stripe below 64, and its LOG thread never calls owner `arm`, so neither row set needs a rerun for
the verdict. Not rerun at 45f98eb; this is from the diff.

### For the next decision

(C) is not no worse than (A): it loses both deferred patterns at every P on producer median
(+24% to +55%), p99, heap (+80 bytes per line) and fire lag (+1 ms), and in pattern 3 its LOG
thread falls behind from P = 4. By D72, logger keeps its deferred-line heap, and with it
`ScheduledTask`'s one-shot cancel and `Thread` from the retired Kronuz/scheduler. A port worth a rerun
needs a record within 10% of (A)'s (at most 22 bytes more): the entry and the self-reference
replacing the `ScheduledTask` base, which such a port drops (its post hand-off union and times),
instead of sitting beside it, the record reached from its entry by offset rather than an owner
pointer, and the sort key read from the record's existing fields. That would answer the heap
bound; pattern 3's LOG-thread cost at P = 14 and the 1 ms of fire lag are
the wheel's own and would remain.

## Arm D

(D) is (C)'s shape and order guarantees on a record at most 22 bytes over (A)'s, the size the heap
bound allows (1.10 times 225 bytes per deferred line). It is `bench/wheel_ab/d/logger.{h,cc}`, a
copy of the library with only that change, built apart from the library's CMakeLists by
`bench/wheel_ab/d/CMakeLists.txt` (`run.sh build-d`) against `Kronuz/wheel@45f98eb`, into
`logger_wheel_ab_d`; the library is unchanged. Everything outside the record is (C): the LOG thread
files deferred lines in a `wheel::timer_wheel` it owns, each pass's `expire(now)` hands back the
due lines, the pass sorts that batch and merges it with the queues' lines, a line already due when
filed bypasses the wheel into the batch, a cleared line stays filed until its slot, and the wait
is until `next()`.

### Design: where (C)'s 80 bytes go

`sizeof(Logging)` is 184 bytes in (A), of which `ScheduledTask` is 56 and `Logging`'s own fields
128. (C) added 80 beside all of it. (D) takes 64 of the 80 out of the places that already hold
them, and the last 16 is the one thing the wheel needs that the record lacks.

| (C)'s field | bytes | (D) |
|---|---|---|
| `timer_entry` (next, pprev, deadline, fire) | 32 | the entry replaces `ScheduledTask`'s wakeup time (8) and its post hand-off union (24): both are scheduler machinery, unused by logger, which never `post()`s. `Logging` derives from `timer_entry` instead of `ScheduledTask`, so the 32 bytes the entry needs are the 32 the base gave up. |
| owner pointer (entry to record) | 8 | none. The entry is `Logging`'s base, so the record is a `static_cast` from the entry the wheel hands back: no pointer, no offset arithmetic. |
| sort key: due | 8 | none. It is the entry's `deadline`, which `arm` already sets to the due time. |
| sort key: call time | 8 | none. It is the record's `atom_created_at` (every record that reaches the wheel is made with created_at equal to its call time, as `do_log` and `do_log_in` pass `now` twice). |
| sort key: filing order | 8 | none. The tie-break past (due, call time) is dropped: the key (due, created) is equal only for two lines logged in the same nanosecond, which no thread does (calls are at least a clock read apart) and across threads has no program order to keep. The check counts inversions to prove it. |
| self-reference (`shared_ptr`) | 16 | kept: a filed record is owned by the wheel, which does not own what it files, and a cleared line's handle may be gone (pattern 3 drops 99 in 100 at once). It sits where (C) had it. |

The one-shot (`atom_created_at`, `atom_cleared_at`, `atom_cancelled`) is `ScheduledTask`'s, moved
into `Logging` unchanged (16 bytes of atomics; the cancel flag fills the padding after
`Logging`'s three bools); `operator bool`, `cancelled()`, the claim and the run-unless-cancelled
check are the same four lines. The batch the pass sorts holds (due, created, record pointer)
instead of (due, created, filing order, `shared_ptr`): 24 bytes per due line against (C)'s 40, with
the `shared_ptr` moved out of the record only when the line is written.

| record | `sizeof(Logging)` | `make_shared` block | requested over (A) | `operator new` bytes per deferred line |
|---|---|---|---|---|
| (A) | 184 | 200 | 0 | 225 (pattern 2) |
| (C) | 264 | 280 | +80 | 305 |
| (D) | 192 | 208 | +8 | about 233, 1.04 times (A)'s |

`sizeof` from `run.sh sizeof` (261112-062455-24c582, `Apple clang 17`, x86-64). (D)'s block of
208 bytes stays inside the 256 bytes macOS's nano allocator serves (the line (C) crossed), and its
heap per line is expected at 1.04 times (A)'s, inside the 1.10 bound; the measured heap is in the
rows. The 8 bytes over (A) are the self-reference's 16 less the 8 saved where the entry and the one-shot
(48 bytes) replace `ScheduledTask` (56, with the cancel flag and `post_ordered` padded to 8).

### Rule for (D)

The Rule above, with the statistic of D77 in place of the six-pair median. A row (a pattern at a P)
is `run.sh row-d PATTERN P`, one `exclusive` job at a named commit: 40 rounds, each running one
process of (A), one of its A/A twin (A2, the same binary) and one of (D), in an order rotated each
round, each process one warm-up round (discarded) and one measured round. Per metric, r is the
median over rounds of D / A taken within the round, with a distribution-free 95% interval from
order statistics; w is the farther end of the interval of A2 / A from 1.00. **Win**, **loss** and
**no win** are the Rule's, with the p99 bound at 1.10 itself (win: r(median ns) <= 0.90 - w,
r(p99) <= 1.10, heap D <= 1.10 times A; loss: r(median ns) >= 1.10 + w), and each row also says
whether (D) is **no worse than (A)** (D72's bar for one mechanism): r(median ns) <= 1.10, r(p99) <=
1.10, heap D <= 1.10 times A. The p99's w and interval are reported beside its r, not added to the
bound. A metric whose interval straddles 1.10 is reported as straddling (D77). Correctness gates every row as for (C):
`run.sh check-d` (every line once, none early, no cleared line written, 0 per-thread and 0
due-order inversions) and `run.sh tsan-d`. Patterns 2 and 3 at P = 1, 4 and 14; pattern 1 has no
timer in (D), so no rows.

## Verdict D

At logger:8094113, `exclusive`, 40 rounds a row, `summarize.py --paired`. Correctness first:
`check-d` 261112-071531-bd8c2e and `tsan-d` 261112-071533-689a79 pass (every line once, none
early, no cleared line written, 0 per-thread and 0 due-order inversions, TSan clean).

r is D / A per round, median over 40, with the 95% interval and the A/A band w beside it. The
bound is r <= 1.10 for median and p99; heap D 267.7 against 1.10 x A 259.7 holds on every row
(1.031 times A, as expected). No row is a win and no row is a loss (the median r never reaches
1.10 + w).

| row | jobs | r median [interval] | r p99 [interval] (w) | no worse than A |
|---|---|---|---|---|
| pattern 3, P = 1 | 261112-071159-b0cda2 | 0.978 [0.898-1.042] | **1.469** [1.149-1.582] (0.217) | **no**: p99 |
| pattern 3, P = 4 | 261112-071201-563e40 | 0.995 [0.969-1.025] | 1.050 [0.890-1.186] (0.131) | yes; p99 straddles 1.10 |
| pattern 3, P = 14 | 261112-071203-028190 | 0.994 [0.979-1.021] | 0.967 [0.894-1.191] (0.132) | yes; p99 straddles 1.10 |
| pattern 2, P = 1 | 261112-071206-e00583 | 1.000 [0.956-1.130] | **1.133** [0.982-1.424] (0.293) | **no**: p99, and the median straddles 1.10 |
| pattern 2, P = 4 | 261112-071208-a53db3 | 1.003 [0.965-1.037] | 0.933 [0.866-1.073] (0.180) | yes |
| pattern 2, P = 14 | 261112-071210-60b58a | 1.018 [1.004-1.044] | 1.028 [0.895-1.093] (0.145) | yes |

Median cost is equal to (A) on every row (r 0.98 to 1.02): the record is small enough that the
wheel costs the producers nothing, which is what (C) lacked (r 1.245 median, heap 1.372 at
pattern 3, P = 1). The two failures are p99 at P = 1. Pattern 3 P = 1 is a clear loss of the
bound (the whole interval is above 1.10); pattern 2 P = 1 has an interval that straddles it, a
point estimate 0.033 over, with w 0.293 wider than the effect, so it is not distinguishable from
(A)'s own noise there.

Outside the Rule, and reported because D72 is about replacing the heap: the wheel's expiry
granularity shows in the lines' lateness and in the LOG thread's CPU. Lateness p99 (`lag_p99_us`)
is 1.1 to 1.4 ms with (D) against 0.07 to 0.28 ms in (A) at pattern 2 (r 4.9, 12.8, 19.6), at
pattern 3 P = 14 6.5 times (13 ms to 85 ms), and 1.1 to 1.4 times at P = 1 and 4. LOG CPU per
call is 2.6 times (A)'s at pattern 3 P = 14 and 1.26 times at P = 4, and 0.30 to 0.39 times at
pattern 2 (the wheel does less work than the heap there). None is a Rule metric.

**For the lead:** (D) is no worse than (A) on 4 of 6 deferred rows and loses on two, both at
P = 1: pattern 3 P = 1 p99 at 1.469 (interval 1.149-1.582, past the 1.10 bound) and pattern 2
P = 1 p99 at 1.133 (interval 0.982-1.424, straddling it; w 0.293). The medians are equal on all
six (0.98 to 1.02), and heap is 1.031 times (A) throughout. D72's one mechanism is therefore
not proven on p99 at P = 1; the owner decides whether to accept those p99 losses (and the
lateness above) or keep logger's heap.

## Arm D, fixed

**(D) at logger:afe86c5 is no worse than (A) on all six deferred rows by the Rule for (D).** Producer
median r is 0.960 to 1.084 and p99 r 0.966 to 1.099; heap is 1.031 times (A) on every row. No row is
a win and no row is a loss. Outside the Rule, (D) still writes pattern 2's lines about 1 ms later
than (A), the wheel's 1 ms slot. All jobs ran at afe86c5 on the same host and toolchain as above.

### Cause

1. **Pattern 3, P = 14: LOG CPU 2.6 times, lateness 6.5 times.** In a `sample` profile of (D)
   (`run.sh profile-d 3 14 D`, 261112-081713-5f3fc7), 521 of the LOG thread's 1,061 active samples
   are the `memmove` of `std::vector<Deferred>::insert` in `file_deferred`. In (A)'s profile
   (261112-081716-e81894) no such cost appears, and its `pop_heap` is small. The insert is the
   bypass, a line already due when the LOG thread files it, put into the sorted batch in order.
   Each insert is linear, so a pass's inserts are quadratic. The counters (`run.sh stats-d`,
   261112-082031-adcffd) show 114,942 of 716,800 lines bypassed, a mean of 4.9 ms past due, a batch
   of up to 29K, and one pass that expired 117,760 lines.
2. **Why lines are due at filing.** Producers queue lines in their rings. The LOG thread files a
   line only when it wakes, and it sleeps until the next filed deadline. A trace at P = 1
   (261112-100143-5b66af) shows the thread slept 198 ms with nothing filed recently and woke
   4.77 ms past its target. Every line due in those 4.77 ms was already due when filed. (A) has
   the same wake rule and files just as late, but its heap takes a due line in O(log n).
3. **Pattern 3, P = 1: p99 1.469.** The P = 1 profile (261112-082038-4353a0) has only about 27
   active LOG samples in 2,683, too few to show the cost. The cause at P = 1 comes from the
   counters (6,588 of 51,200 lines bypassed, mean 4.9 ms late, a batch of up to 2,047;
   261112-082033-7add00) and from the experiment below. Replacing the batch with a heap (change 1)
   left p99 at 1.438. Making the LOG thread file promptly (change 2) brought p99 to 1.031. The
   loss comes from the late filing burst itself, not from the batch's data structure. The profile
   does not show how that burst raises the producer's p99 blocks.
4. **Pattern 2, P = 1: p99 1.133** was noise. On that row (D) bypasses 42 lines with a largest
   pass of 27 (261112-082035-21c273). Three reruns gave p99 r 0.990 (change 1), 1.010 (change 2)
   and 1.099 (final), and every interval straddles 1.10.

### Changes, each rerun on the losing rows

| change | commit | p3 P=1 median / p99 | p3 P=14 median / p99, lag, LOG CPU | p2 P=1 median / p99 | bypassed p3 P=14 / P=1 |
|---|---|---|---|---|---|
| none (Verdict D) | 8094113 | 0.978 / **1.469** | 0.994 / 0.967, 6.53, 2.60 | 1.000 / 1.133 | 114,942 / 6,588 |
| 1: the due batch is a binary heap | 6c29783 | 0.988 / **1.438** [1.208-1.564] | 0.998 / 1.034, 1.17, 1.18 | 1.003 / 0.990 | 54,524 / n/a |
| 2: after a pass that filed lines, sleep at most one tick (with 1) | e7a4f3f | 1.019 / 1.031 [0.880-1.167] | 0.998 / 1.025, 1.26, 1.14 | 0.963 / 1.010 | 28,672 / 5,120 |
| 3: a push due within 8 ms of the sleep's end wakes the LOG thread (with 1, 2) | 3dcc62e | 0.975 / 0.886 [0.804-1.211] | 0.990 / 0.972, 1.12, 1.17 | n/a | 0 / 0 |
| final: 2 + 3, change 1 reverted, the sorted batch | afe86c5 | 0.960 / 0.966 | 0.992 / 0.983, 1.12, 1.07 | 1.084 / 1.099 | 0 / 0 |

Jobs, rows in table order: 261112-082231-ac506e, -082234-9559fb, -082236-9714e6; 261112-082706-07b590,
-082708-f1c86e, -082711-f34b89; 261112-101944-86316f, -101946-73c9a9; and the final rows below.
Counters: 261112-082724-b5f70b (change 1 alone), -082652-65999f and -082655-1d3288 (2),
-101939-91ce7a and -101942-4b0fb6 (3), -111538-27efbf and -111541-4c2f8a (final).

After change 3 no line is bypassed, so the batch never takes an insert. Change 1's heap no longer
pays for itself, and the final (D) drops it: the wheel's expired batch is sorted once per pass, as
in Verdict D. Changes 2 and 3 alter only the LOG thread's sleep, so a port ships them with the
wheel. Their cost is in the rows: the LOG thread wakes once per tick while lines arrive, and a
producer whose line is due near the sleep's end notifies it.

### Final rows

At logger:afe86c5, `exclusive`, 40 rounds a row, `summarize.py --paired`. Correctness:
`check-d` 261112-111533-94c654 and `tsan-d` 261112-111535-1f3660 pass (every line once, none early,
no cleared line written, 0 per-thread and 0 due-order inversions, TSan clean).

| row | job | r median [interval] (w) | r p99 [interval] (w) | lag p99 ms A / D (r) | LOG CPU r [interval] | no worse than A |
|---|---|---|---|---|---|---|
| pattern 3, P = 1 | 261112-111543-8bd01a | 0.960 [0.927-1.018] (0.078) | 0.966 [0.910-1.091] (0.116) | 9.03 / 9.65 (1.098) | 1.007 [0.872-1.118] | yes |
| pattern 3, P = 4 | 261112-111549-a6d0a9 | 0.979 [0.931-1.045] (0.066) | 0.995 [0.934-1.060] (0.135) | 9.45 / 10.74 (1.147) | 0.984 [0.927-1.131] | yes |
| pattern 3, P = 14 | 261112-111546-ef2712 | 0.992 [0.975-1.011] (0.018) | 0.983 [0.920-1.022] (0.075) | 12.08 / 14.11 (1.117) | 1.069 [1.021-1.110] | yes |
| pattern 2, P = 1 | 261112-111551-87f1d9 | 1.084 [0.997-1.182] (0.087) | 1.099 [0.847-1.261] (0.123) | 0.32 / 1.37 (4.30) | 0.441 [0.335-0.578] | yes; median and p99 straddle 1.10 |
| pattern 2, P = 4 | 261112-111554-1986e4 | 1.034 [0.960-1.085] (0.067) | 0.997 [0.910-1.064] (0.090) | 0.11 / 1.34 (11.9) | 0.340 [0.287-0.398] | yes |
| pattern 2, P = 14 | 261112-111556-0924db | 1.011 [0.978-1.070] (0.022) | 1.016 [0.812-1.257] (0.188) | 0.07 / 1.31 (18.8) | 0.340 [0.253-0.376] | yes; p99 straddles 1.10 |

Heap per call is 267.7 against (A)'s 259.7 bytes in pattern 3 and 267.2 against 259.2 in pattern 2.
That is +8 bytes per call, 1.031 times (A), interval [1.031-1.031], on every row.

- **Where (D) loses, outside the Rule.** Pattern 2's lateness p99 is 1.31 to 1.37 ms against 0.07
  to 0.32 ms, which adds 1.0 to 1.24 ms to lines deferred 100 ms to 1 s. The cause is `next()`:
  it is the end of the next occupied 1 ms slot (`wheel.hh`), so a line waits for the end of its
  slot. Pattern 3's lateness is 1.10 to 1.15 times (A)'s (+0.6 to +2.0 ms on 9 to 14 ms), and every
  interval lies above 1.0. LOG CPU at pattern 3, P = 14 is 1.069 [1.021-1.110] (+20 ns a call,
  down from 2.6 times). At pattern 2 (D)'s LOG thread uses 0.34 to 0.44 of (A)'s CPU.
- **The one row to watch: pattern 2, P = 1.** Its median r of 1.084 has an interval of
  [0.997-1.182]. That interval straddles 1.10 and almost excludes 1.00, so it could hide a cost
  of up to about 8%. Earlier code measured 1.000 [0.956-1.130] on this row (Verdict D), 1.003
  [0.965-1.060] (change 1) and 0.963 [0.939-1.065] (change 2). Pattern 2 bypasses almost nothing,
  so the filing path is nearly the same in all four runs. Their point estimates span 0.96 to 1.08,
  against A/A bands w of 0.05 to 0.10. The honest reading is between equal and a few percent
  worse. It is not a loss, whose bar is 1.10 + w = 1.187.
- **`sizeof(Logging)` 192 against 184** (+8 bytes, 261112-062455-24c582). The `make_shared` block
  is 208 against 200 bytes, both in macOS's nano allocator (256 bytes and under). The cost is the
  measured +8 heap bytes per deferred line (1.031 times), plus 8 bytes for each line held:
  1.1 MB at pattern 3, P = 14's peak of 143,360 held lines. Producer time shows no cost from the
  size, with median r 0.96 to 1.03 on five rows.
- **What Kronuz/wheel would need: nothing for the Rule.** (D) sorts each pass's expired batch
  itself: 24-byte keys, up to 17,408 lines a pass at pattern 3, P = 14 and 1,024 at P = 1. With
  that sort, LOG CPU is 0.98 to 1.07 times (A)'s, so a sorted `expire` in the wheel would move the
  sort without a measured saving. The bypass that cost 2.6 times came from logger's sleep policy,
  not from the wheel, and the fix is in logger. The one wheel-owned cost is the 1 ms of
  lateness. Matching (A)'s 0.07 to 0.32 ms would take a wheel whose `next()` and `expire(now)`
  work to the entry's deadline instead of its slot's end, a change to the wheel's resolution
  contract. This is unmeasured and not proposed here.

**For the lead:** (D), the wheel, matches the heap on every deferred row under the Rule.
Medians are 0.96 to 1.08, with only pattern 2, P = 1 straddling 1.10 at 1.084 [0.997-1.182]. p99
is 0.97 to 1.10, with pattern 2 at P = 1 and P = 14 straddling. Heap is 1.031 times (A), from
`sizeof(Logging)` 192 against 184. The fix lives in logger's LOG thread: it files promptly after
filing (one-tick sleep) and wakes for a line due within 8 ms of its sleep's end. It needs no wheel
change. The price of one mechanism (D72) is lateness: deferred lines are written about 1 ms later
in pattern 2 (1.3 ms against 0.07 to 0.32 ms p99) and 1.10 to 1.15 times later in pattern 3. The
LOG thread also costs 7% more CPU at pattern 3, P = 14 and 56 to 66% less at pattern 2. The owner
decides whether that lateness is acceptable: wheel, or keep the heap.
