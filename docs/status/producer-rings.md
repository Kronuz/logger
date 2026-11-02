# Status: producer rings (logger#1)

Workstream: the producer's cost of one routine line (`L_INFO("value {} and {}", i, "x")`), logger#1.
Landed on `main` from `perf/logger-rings-v2` (base a82454b); the prototype was `perf/logger-rings`
(7a8bce8). Both branches are deleted.

## Done criteria

1. One producer pays at most what a synchronous fmt 10.2.1 + `std::mutex` logger with no I/O pays, on
   the Apple M4 Pro and the Intel i9-9980HK, and four producers clearly less; quill (pinned release) as
   a comparator in `bench/lg.cc` with a same-text check.
2. Every guarantee kept: per-thread order, ordered posts, `L_DELAYED_*` and `Log::clear/unlog`, `clears`,
   the isolated-line latency (bb2d16c), `max_pending` and its summary line, flush at `finish()` and at
   exit, the handlers, the API.
3. TSan-clean stress tests at 1 to 16 producers (order, delayed/cancel, finish/exit flush,
   backpressure) that fail with the key fix reverted; ring mutants (publish ordering, wrap) caught; the
   old suite green; ASan/UBSan.
4. One `quiet strict` hold per machine: START (ce27b90) vs main vs NEW with A/A, lg/latency/drain and
   fmt + mutex. README Performance and CHANGELOG; CI green on every job.

## State

Landed: 21e9642 (the queues) and fb843c0 (the wake test runs alone; beside the queue test's 16
producers on linux-x86_64-gcc its median wakeup passed 900 us), CI run 41396610451 green on every
job. The bar is met on the Apple M4 Pro (every row below fmt + mutex) and in bursts on
the i9-9980HK; one producer back to back on the i9 pays 83 ns against fmt + mutex's 62, so
logger#1 stays open with the numbers. Every row is faster than main on both machines, or within
the A/A noise (latency).

## Done

- Root cause of the prototype's heap corruption (ASan on the VM and the M1 Pro): `ring_room()` wrote
  the wrap record as `new (p) LineRecord`. `LineRecord` holds a `std::source_location`, whose
  constructor writes, so the placement new wrote past the `to_end` bytes left (8 at least) into the
  next heap chunk: 8 bytes at offset 32 with libc++, 24 at offset 32 with the VM's shim. Fix: the wrap
  is an 8-byte `RecordHead` written with memcpy (`write_wrap`).
- Regression test `test_wrap_every_offset` (`logger_queue_test wrap`): one producer, 16 lines in
  flight so its 16 KiB buffer never grows, sizes drawn from every multiple of 8 to 520, so the wrap
  falls at every offset short of a header. ASan on the M1 Pro: the fix passes; the fix reverted
  (placement new) and the old prototype 7a8bce8 both report `heap-buffer-overflow ... WRITE of size 8
  ... std::source_location::source_location`.
- Mutants (TSan on the VM, `vmmut.sh`): the publish store relaxed: data race in `logger_queue` and
  `logger_wake`; the wake removed from `ring_published`: `logger_queue` hangs (timeout) and
  `logger_wake` fails (`done_ns.load() != 0`); the wrap written as a whole header: ASan above. The
  wrap condition weakened to `free >= need` is not caught: the producer's view of the free room
  lags the LOG thread's (it publishes its read index once per pass), and `test_wrap_full`, which
  keeps the buffer within a line of full, passes with it (open).
- v2: main's due rules kept (a line due within a tick is due 1 ms after its call unless it starts a
  burst with nothing pending); the LOG thread sleeps until the earliest due line (no naps), and a
  producer wakes it only if its line is due earlier; routine-line handles keep `unlog()` (the swap is
  written when the handle drops); shared data on 128-byte lines.
- Tests in `test/queue.cc`: the wrap at every offset and near full; exactly once at 1, 2, 4, 8 and
  16 producers; order across 4 and 16 threads; due order with routine lines and records due 0 to
  3.5 ms out; watchdogs armed and dropped from 16 threads; backpressure from 16 producers; swaps
  from 8 threads; the sleep handshake; short-lived threads; exit without `finish()`; `finish()`
  racing 16 producers. Quick sizes under TSan.
- Verification at 0a79b95 (code as landed): see Verification below.
- Bench: `bench/lg.cc` runs fmt + mutex through the same burst and back-to-back harness; the pauses
  spin (a sleeping pause times the CPU's clock ramp, 5x on the M1 Pro for fmt + mutex too); the
  same-text check without timestamps; quill v13.1.0 optional; the spdlog option is gone (untested).
- Timing, one `quiet strict` hold per machine at f8243b5 (code identical to the landed commit), START
  ce27b90 and main a82454b against it, 5 rounds with A/A (CHANGELOG, README): M4 Pro one producer
  22.7 ns in bursts (main 53.3, START 254.2, fmt + mutex 28, quill 6.7), 21.0 back to back (fmt + mutex
  28.6); four producers 22.1 and 6.1 (fmt + mutex 45 and 80). i9: 59.3 and 83.6 (fmt + mutex 65 and
  62, quill 29 and 61); four producers 65.5 and 23.4 (fmt + mutex 145 and 172).

## Verification

At 0a79b95 (the landed code; the landed commit adds docs only):
- Linux VM, clang 12: ASan+UBSan ctest 11/11 (`logger_queue` 11.8 s), TSan ctest 10/10
  (`logger_queue` 10.8 s, quick sizes), no reports.
- Apple M1 Pro: Release ctest 12/12 (with `logger_bench_check`), ASan+UBSan ctest 11/11, the full
  queue test under ASan in 7 s.
- Same-text check in both timing holds: logger, quill and fmt + mutex, 100,000 messages each, none
  different.

## Next

1. logger#1 (open): the i9's back-to-back row. The stamp clock is 20 ns of it (`mach_continuous_time`;
   `rdtsc` is 6, but its conversion to steady time is not exact), and the rest is the producer's
   stores missing the cache while the LOG thread (535 ns a line there) falls behind and the queue
   grows. Smaller line headers, a faster LOG thread, or rdtsc stamps with a gate in raw ticks.
2. A test that catches the weakened wrap condition.

## Blocked on

Nothing.
