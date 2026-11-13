# logger: per-thread queues, one LOG thread, and deferred lines

Many threads log; one thread, the LOG thread, writes. Each thread that logs owns a
queue, a byte ring it writes its lines into, formatted in place, and publishes
with one store; the LOG thread merges the queues in the order the lines were
logged and keeps deferred lines ("this is taking too long") until they are due or
cancelled. A routine line costs its thread no lock, no allocation and no
read-modify-write.

It mirrors the logger as it runs in Xapiand, with the host-specific parts
(exception description, backtraces, named threads) moved behind hooks.


## The three paths a log line can take

A single `L_*` call ends in one of three places, decided in `Logging::log_in` and
`Logging::add`:

```
many worker threads                                   one "LOG" thread
-------------------                                   ----------------

L_EMERG / L_ALERT / L_CRIT --- inline --------------------------------------> sinks
        (caller formats and writes the line itself, then returns)            (file / stderr / syslog)

L_ERR / L_WARNING / L_INFO --> the thread's queue: the line, formatted ---\
                                                                           >-- merge --> write
L_DELAYED(100ms, ...)      --> a Logging record; the thread's queue    ---/   (deferred lines
                               holds a reference to it                          wait in a wheel)
                                     |
                                     +-- clear() / unlog() can cancel or swap it before it fires
```

1. **Immediate, inline.** The calling thread renders the line and writes it to
   the sinks itself, then returns. This is the path for the three most severe
   levels (`EMERG`, `ALERT`, `CRIT`) and for `print`. The point is a guarantee: if
   the process is about to die, the line is on its way to disk before the caller
   moves on.

2. **Async, "now."** The calling thread formats the message straight into its
   queue, stamps it with one clock read, and publishes it. The line is due at its
   call when nothing is pending, and the LOG thread is woken for it, so an
   isolated line is written at once; while lines are pending it is due 1 ms after
   the call, so a burst is written in 1 ms batches and its lines need no signal.
   This is the path for `ERR` and everything less severe (`WARNING`, `NOTICE`,
   `INFO`, `DEBUG`), set by `priority >= ASYNC_LOG_LEVEL`. The caller never
   touches the output fd, so a worker thread spewing logs never blocks on I/O or
   on a shared log lock. A line that carries an exception, or is logged through
   `do_log`, is a `Logging` record instead, referenced from the same queue and
   written in the same order.

3. **Deferred, future.** A `Logging` record due at `now + delay`, referenced from
   the thread's queue; the LOG thread files it in its timer wheel and fires it
   when the delay elapses, unless someone cancels it first. This is the "log slow
   operations only" path: arm a "this took too long" line 100 ms out, and cancel it
   when the operation finishes in time. Most of the time it finishes in time, so
   the line is cancelled before it is ever emitted.

If the queue refuses a line (after `finish()` began, a delay of a day or more, or
no memory), `Logging::add` first undoes `scheduled`, so the pending count stays
exact, then acts by kind:

- **Already due** (an async line with no delay): the calling thread writes it
  inline, as path 1 would, since it would have been written anyway.
- **Severe** (priority above `NOTICE`, what backpressure never drops) and not a
  watchdog: written inline now, even if it was deferred.
- **A future watchdog** (`clears`, the slow-operation line): it stays armed off the
  queue. At its cancel point, when the handle is dropped or `clear()`ed as the
  work completes, it is written (late) only if its deadline has passed, exactly
  when the LOG thread would already have written it, so only slow work shows;
  otherwise it is discarded like any cancelled watchdog. Its swap follows the same
  rules as a queued one's: kept on a handle drop, cancelled by `clear()`.
  A watchdog keeps this rule whatever its priority: writing it early would log
  every fast operation.
- **A routine future line that does not clear** (`L_DELAYED(false, ...)` or
  `do_log` with `clears == false`, `NOTICE` or below): it has no cancel point to
  decide at, so it is lost. Neither Xapiand nor Detent emits one.

## The queues and the LOG thread

A thread's queue is made on its first queued line: a power-of-two buffer, 16 KiB
at first, that the thread writes records into and the LOG thread reads them from.
A record is a line (a 40-byte header and the message), a reference to a `Logging`
record, or a wrap (an 8-byte head that pads to the end of the buffer). The
producer owns the write index and publishes it with one store; it reads the LOG
thread's read index only when it runs out of room, and then wraps to the start or,
if the LOG thread has not freed enough, moves to a buffer twice as big (the LOG
thread reads the old one to its end and frees it). A queue never blocks its
producer and never drops a line; `max_pending` is the bound.

The LOG thread is one thread with its own loop. Each pass reads its clock, files
the deferred records it finds into its wheel, and writes, in order of due time
(and of call time within a due time), every queued line due by then and every
deferred line due by then. It takes only lines stamped before the pass began, so a
line is never written ahead of one logged before it that was not yet visible. When
nothing is due it records when the earliest line is due (`worker_wake`), marks
itself sleeping, looks at the queues once more, and sleeps until then. A producer
that publishes while it sleeps reads the mark; if its line is due before
`worker_wake` it wakes the LOG thread (one notify per sleep). The mark, the second
look and the producer's publish-then-read are all sequentially consistent, so
either the LOG thread sees the line or the producer sees it sleeping.

The order the log promises: one thread's lines come out in the order they were
logged, and of two lines whose logging is ordered (one call returning before the
other begins) and due in that order, the first is written first (`test/order.cc`,
`test/queue.cc`). Lines of two threads stamped within one tick of the stamp clock
(41.67 ns on Apple silicon, whose counter runs at 24 MHz) can come out in either
order.

A thread's queue outlives the thread: when the thread exits, the LOG thread writes
what is left and hands the queue to the next thread that logs. A line logged from
another thread-local's destructor after that goes through a shared queue behind a
mutex.

`Logging::finish()` stops new lines (a refused line that is due is written on the
calling thread) and gives the LOG thread up to `wait` passes, each waiting up to
100 ms, to write what comes due. Then the LOG thread takes a last look: every
queued line is written, deferred lines due by then are written, and the rest are
released. A producer that published just then sees the LOG thread closed and
writes its line itself, unless the last look got it. At exit, if `finish()` was
never called, a static's destructor does the same with one pass and joins the LOG thread for
200 ms. If the join times out (a handler is stalled), the handlers and config are about to be
destroyed, so the destructor sets `abandoned`: the LOG thread finishes the line it is in, writes
nothing more (what is queued is lost, as at any timed-out join) and parks, and the destructor waits
for that before returning.

## Deferred lines ride the LOG thread's wheel

A deferred line waits in a `wheel::timer_wheel` from [Kronuz/wheel](https://github.com/Kronuz/wheel)
that the LOG thread owns; no other thread touches it, so it takes no lock and no atomic. `Logging`
derives from `wheel::timer_entry`, so the record is its own entry: filing it allocates nothing,
and the entry the wheel hands back is the record by a `static_cast`. The entry's deadline is the
line's due time and the record's `created_at` its call time; the record holds a `shared_ptr` to
itself while filed, because the wheel does not own what it files and a cancelled line's handle may
be gone. The one-shot cancel (`created_at`, `cleared_at` and a user cancel's flag) is the record's
own three atomics.

Each pass calls `expire(now)` and sorts the lines it hands back by (due time, call time, filing
order), then merges that batch with the queues' lines. The filing order is a 32-bit counter the
LOG thread stamps on the record, so two lines of one thread that tie on the first two keys
come out as they were logged. A line already due when the LOG thread files it skips the wheel and
is put into the batch in order, by the same key. A cancelled line stays filed until its slot comes
and is dropped then. The LOG thread sleeps until `next()`, the end of the next occupied slot.
There is no second timer structure: the queues' merge is a heap of each queue's next line,
rebuilt every pass, and holds nothing between passes.

The resolution is the wheel's: 1 ms slots. A deferred line is never written before its due time,
and is written by the end of its 1 ms slot plus the LOG thread's timed wake, so it is about 1 ms
later than a heap that wakes at the exact due time would write it (lateness p99 1.31 to 1.37 ms
against 0.07 to 0.32 ms for lines deferred 100 ms to 1 s, `docs/perf/wheel-ab.md`, "Arm D,
fixed"). That is the price of one timer mechanism across the libraries (Detent's D72 and D86):
the deferred line is a "this is taking too long" warning 100 ms or more out, where 1 ms is below
anything a reader can see, and in exchange the LOG thread uses 0.34 to 0.44 times the heap's CPU
when lines are deferred far out, producers pay the same per call (median 0.96 to 1.08 times), and
a deferred line costs 8 more heap bytes. `test/deferred.cc` holds the bound.

Two rules keep the LOG thread filing lines before they are due, so the batch rarely takes an
insert:

1. After a pass that filed lines, it sleeps at most one tick: more are likely in the queues.
2. A producer whose line is due within 8 ms of the LOG thread's wake wakes it, because a timed
   wait returns late and a line due just after it would otherwise be filed already due.

The LOG thread is a `Thread` from [Kronuz/threadpool](https://github.com/Kronuz/threadpool),
which the CMake file fetches at its own pin.

## The sinks

Three `Logger` handlers behind a common interface: a file (`StreamLogger`), stderr
(`StderrLogger`), and syslog (`SysLog`). `handlers` is a vector; every emitted
line goes to all of them. Each makes its own coloring decision and its own write.

## The cancel, and the swap

A record's `Log` handle owns the two operations that make the pattern work:

- **`clear()`** cancels the record. It is a one-shot compare-exchange: whoever
  wins, the caller cancelling or the LOG thread firing, the other becomes a no-op.
  So a fast operation that finishes and drops its handle cancels its own "too
  slow" warning with one atomic, and the warning never reaches the output. The
  LOG thread drops a cancelled record when its slot comes.
- **`unlog()`** swaps the message. You arm "operation X timed out" for the future,
  and when X finishes you replace it with "operation X took 87 ms." If X never
  finishes, the timeout line stands.

A routine line written into the queue has no record. Its handle keeps the line's
stamp and call site, so `unlog()` still swaps in a message, written as a new line
when the handle drops; `clear()` drops the swap and returns false, as it does for
a line the LOG thread has already written (a routine line cannot be cancelled once
it is queued). The handle allocates nothing unless `unlog()` is called.

## What one call costs

A routine `L_INFO` does this on the calling thread and nothing else:

1. Compare the priority with the level; below it, no argument is evaluated.
2. Format the message straight into the queue. `logger_detail::fast_format` writes a format
   string whose fields are all `{}` over integers, `bool`, `char` and strings into the queue's
   free bytes (the bytes `std::vformat` writes); anything else goes through `std::vformat_to` into
   the thread's reusable string and is copied in. Neither allocates once the buffers have grown.
3. Read the stamp clock once: on macOS the raw count of `mach_continuous_time` (the clock behind
   `steady_clock` there), which the LOG thread converts; elsewhere `steady_clock`. The wall-clock
   time is derived on the LOG thread.
4. Decide the due time. A line due within a tick is due 1 ms after its call, unless it starts a
   burst: when it is not `clears`, its thread queued no such line in the last 1 ms, and nothing is
   pending (`pending_count()` is 0, so every line logged before it has been written or
   cancelled), it is due at its call. A thread in a burst reads no counter (the 1 ms gap is
   checked first).
5. Write the line's header, add one to the queue's own `posted` count, publish the write index
   (one sequentially consistent store), and read the LOG thread's state (one load). Only if the
   LOG thread sleeps or is closing does the producer call out of line, and it signals only if
   its line is due before the LOG thread wakes.
6. Return a handle that holds the stamp and the call site; it allocates nothing.

There is no lock, no allocation and no read-modify-write on this path, and the only shared data a
producer writes is its own queue. Data written by one thread and read by another sits on its own
128 bytes (Apple silicon's cache line, and the pair Intel's adjacent-line prefetcher fetches).
What is left is the formatting, the clock and the stores: on an Intel i9-9980HK
`mach_continuous_time` is 20 ns of a call (`bench/parts.cc`), on Apple silicon a few. When the LOG
thread falls behind a producer that logs back to back, the queue grows to hold the backlog, and the
producer's stores miss the cache; README.md's *back to back* rows show it.

## Where the pieces live

- `logger.h` — `LogConfig`, `LogHooks`, the `Logger` sinks, `Logging` (the record +
  static facade), the `Log` handle, the `L_*` macros, and the producer's half of the
  queues (`queue_line`, the record layouts, the thread-local block). The macros
  use a priority-guard ternary plus a variadic `format_msg`, so a filtered line
  evaluates none of its arguments.
- `logger.cc` — the queues' out-of-line half (`ring_attach`, `ring_room`,
  `ring_published`), the LOG thread (`LogWorker`), the three-path `add` router,
  `operator()` (emit/decorate), the cancel/swap in `clean`, the bounded
  `OnceFilter`, the default hooks, and backpressure.
