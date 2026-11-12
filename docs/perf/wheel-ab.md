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
- **Lines held** (reported, added with (C)): every 1 ms a sampler reads the lines the scheduler
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
`ScheduledTask`'s one-shot cancel and `Thread` from Kronuz/scheduler. A port worth a rerun
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
**no win** are the Rule's (win: r(median ns) <= 0.90 - w, r(p99) <= 1.10 + w, heap D <= 1.10 times
A; loss: r(median ns) >= 1.10 + w), and each row also says whether (D) is **no worse than (A)**
(D72's bar for one mechanism): r(median ns) <= 1.10, r(p99) <= 1.10 + w, heap D <= 1.10 times A. A
row whose interval straddles 1.10 is reported as such. Correctness gates every row as for (C):
`run.sh check-d` (every line once, none early, no cleared line written, 0 per-thread and 0
due-order inversions) and `run.sh tsan-d`. Patterns 2 and 3 at P = 1, 4 and 14; pattern 1 has no
timer in (D), so no rows.
