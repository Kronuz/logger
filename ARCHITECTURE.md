# Xapiand's logger: deferred, lock-free logging on the wheel

The logger is the original reason [stash](https://github.com/Kronuz/stash) exists.
It is the cleanest example of what the wheel is for: many threads arming
time-keyed work that a single thread drains, where most of the arming sits on a
hot path and a lot of it is cancelled before it ever runs.

This documents how the logger actually works, because the shape is a little
different from what you would guess. It is the design this library implements; it
mirrors the logger as it runs in Xapiand, with the host-specific parts (exception
description, backtraces, named threads) moved behind hooks.


## The three paths a log line can take

A single `L_*` call ends in one of three places, decided in `Logging::add`:

```
many worker threads                              one "LOG" thread
-------------------                              -----------------

L_EMERG / L_ALERT / L_CRIT ---- inline ------------------------------> sinks
        (caller formats and writes the line itself, then returns)      (file / stderr / syslog)

L_ERR / L_WARNING / L_INFO --> Logging task @ now ----\
                                                       >-- stash wheel --> walk --> task() --> sinks
L_DELAYED(100ms, ...)       --> Logging task @ now+d --/                  (LOG thread)  (decorate + write)
                                     |
                                     +-- clear() / unlog() can cancel or swap it before it fires
```

1. **Immediate, inline.** The calling thread renders the line and writes it to
   the sinks itself, then returns. No scheduler, no stash, no hand-off. This is
   the path for the three most severe levels (`EMERG`, `ALERT`, `CRIT`) and for
   `print`. The point is a guarantee: if the process is about to die, the line is
   on its way to disk before the caller moves on.

2. **Async, "now."** The calling thread renders the message into its own reusable
   buffer, copies it into a `Logging` task keyed 1 ms after the call (the wheel's
   tick), and posts the task to the scheduler. The single LOG thread files the task
   into the wheel, and on its next pass does the decoration and the write. This is
   the path for `ERR` and everything less
   severe (`WARNING`, `NOTICE`, `INFO`, `DEBUG`), set by `priority >= ASYNC_LOG_LEVEL`.
   The caller never touches the output fd, so a worker thread spewing logs never
   blocks on I/O or on a shared log lock.

3. **Deferred, future.** Same as async, but keyed at `now + delay`. The LOG thread
   fires it when the delay elapses, unless someone cancels it first. This is the
   "log slow operations only" path: arm a "this took too long" line 100 ms out,
   and cancel it when the operation finishes in time. Most of the time it finishes
   in time, so the line is cancelled before it is ever emitted.

The thing to notice: paths 2 and 3 both go through the same stash-backed
scheduler. **There is no separate queue for "regular" logs.** The wheel is the one
channel from the many producer threads to the single consumer.

If the scheduler refuses a line (its `add` returns false: after `finish()` began,
or a delay past the wheel's 24 h horizon), `Logging::add` first undoes `scheduled`
and the `pending` increment, so the backpressure count stays exact, then acts by
kind:

- **Already due** (an async line with no delay): the calling thread writes it
  inline, as path 1 would, since it would have been written anyway.
- **Severe** (priority above `NOTICE`, what backpressure never drops) and not a
  watchdog: written inline now, even if it was deferred.
- **A future watchdog** (`clears`, the slow-operation line): it stays armed off the
  wheel. At its cancel point, when the handle is dropped or `clear()`ed as the
  work completes, it is written (late) only if its deadline has passed, exactly
  when the wheel would already have written it, so only slow work shows;
  otherwise it is discarded like any cancelled watchdog. Its swap follows the same
  rules as on the wheel: kept on a handle drop, cancelled by `clear()`.
  A watchdog keeps this rule whatever its priority: writing it early would log
  every fast operation.
- **A routine future line that does not clear** (`L_DELAYED(false, ...)` or
  `do_log` with `clears == false`, `NOTICE` or below): it has no cancel point to
  decide at, so it is lost. Neither Xapiand nor Detent emits one.

## There is no logging thread separate from the scheduler

It is tempting to picture a logging thread on one side, the scheduler on the
other, and a queue moving lines between them. That is not the shape. The
scheduler's consumer thread *is* the logging thread.

`Logging` is both the log record and a `ScheduledTask`. The scheduler the logger
uses is the plain single-consumer `Scheduler<Logging, logging>`, not the
thread-pooled variant. Its one thread runs the loop: sleep until the next due
time, walk the stash wheel, and for each due task that has not been cancelled,
call the task's `operator()` directly. That `operator()` is the log emission. It
builds the decorated line (timestamp, thread name, source location, traceback, an
age annotation) and fans it out to the installed sinks, each of which does the
actual `write`.

So "how does a line get from the scheduler to the logging thread" has no answer,
because they are the same thread. The wheel moves lines from the producers (every
worker thread) to that one consumer; the consumer does the write. One consumer
also means the output is serialized for free: no interleaved half-written lines,
and no lock around the fd.

## The sinks

Three `Logger` handlers behind a common interface: a file (`StreamLogger`), stderr
(`StderrLogger`), and syslog (`SysLog`). `handlers` is a vector; every emitted
line goes to all of them. Each makes its own coloring decision and its own write.

## The cancel, and the swap

The deferred path returns a `Log` handle that owns the two operations that make
the pattern work:

- **`clear()`** cancels the scheduled task. It is a one-shot CAS: whoever wins,
  the caller cancelling or the LOG thread firing, the other becomes a no-op. So a
  fast operation that finishes and drops its handle cancels its own "too slow"
  warning with a single atomic, and the warning never reaches the output.
- **`unlog()`** swaps the message. You arm "operation X timed out" for the future,
  and when X finishes you replace it with "operation X took 87 ms." If X never
  finishes, the timeout line stands.

Both arm and cancel are cheap and non-blocking, which is the whole reason this is
affordable to do on every request. A heap-behind-a-mutex scheduler would put a
lock on the hot path of every armed-and-cancelled log, and the cancel is the
common case.

## What one call costs

A routine `L_INFO` does this on the calling thread and nothing else:

1. Compare the priority with the level; below it, no argument is evaluated.
2. Format into the thread's reusable buffer. `logger_detail::FormattedMsg` writes a format
   string whose fields are all `{}` over integers, `bool`, `char` and strings straight into a
   raw buffer (the bytes `std::vformat` writes), and hands anything else to `std::vformat_to` into
   the thread's reusable string. Neither allocates once the buffer has grown.
3. Read the steady clock once, and derive the wall-clock stamp from it: the offset between the
   two clocks is measured again every 20 ms. (On macOS the steady read goes straight to
   `mach_continuous_time`, after a check at start-up that it agrees with `steady_clock`.)
4. Make the `Logging` record, one allocation, and copy the message into it. The thread's id
   and indent come from one thread-local block.
5. Post it: one compare-exchange onto the scheduler's lock-free stack, and a signal only if the
   LOG thread sleeps past the line (once per sleep, however many lines arrive). A line due
   within a tick of its call is due exactly 1 ms after it, so one thread's wakeup times never
   decrease: it goes through `post_ordered`. Any other line (a deferred one) goes through `post`.
   Two threads' lines can reach the stack in a different order from their clock reads (a thread
   can be preempted between the two), so the queue may hold a line behind one due a little later,
   by up to that gap; the order the log promises is the one between lines whose logging is itself
   ordered, which holds (see `test/order.cc` and the scheduler's `test_ordered_posters`).
6. Add one to the thread's stripe of the lines-posted counters. `pending_count()` is the sum of the
   stripes less the LOG thread's own completed counter, so no cache line is written by both.
7. Return the `Log` handle. Dropping it reads the steady clock for `cleaned_at` (the age of a line
   is measured to it) and sets it once.

The LOG thread takes the whole stack with one exchange, files the lines, and fires them. A
`post_ordered` line goes into an arrival queue, not the wheel. A 1 ms wheel slot is a chain of
10-entry chunks that an insert walks from the head, so a slot holding the thousands of "now"
lines that one busy thread logs in a millisecond makes every insert cost thousands of hops: the
old producer paid that on its own thread, and a first version of the hand-off that filed
everything into the wheel moved the cost to the LOG thread, which then drained 50 000 lines at
8 us each with a 21 ms stall. The arrival queue is merged with the wheel by tick and filing
order (`BaseScheduler::fire_due` in the scheduler), which is the order the wheel alone would have
given, so output order is unchanged. See the scheduler's ARCHITECTURE.md.

What a call costs (`bench/steady.cc`, with each piece alone from `bench/parts.cc`). On an Apple M1
Pro it is about 76 ns (470 before): two reads of `mach_continuous_time` at 5.6 ns, the record's
`make_shared` at 21.5 ns, the compare-exchange that posts it at 6.5 ns, the stripe's `fetch_add` at 2 ns,
and the formatting, the message copy, the `shared_ptr` traffic and the handle's drop for the rest. Before,
six clock reads were about 100 ns, `std::vformat` into a fresh string 58 and the scheduler's `add()`
about 310 (367 ns against 57 for `post()` in the scheduler's `bench/bench_post.cc`). On an Intel
i9-9980HK it is about 225 ns (715 before), and the allocator and the clock are the difference: the record
costs 56 ns, a 60-byte message string 84 ns (the short-string size is 22 bytes), a steady-clock read 20 ns,
and two clocks, a record and the formatting with no queue at all add to 220 ns. Getting under 100 ns
there takes a pool for the records and the message stored inside the record.

## Why stash, specifically

The logger does not lean on stash for peak throughput. It leans on two things
stash gives that a locked structure does not.

The first is that **the arm never blocks**. When every worker thread logs at once
they do not serialize behind a log mutex; the insert is lock-free, so a burst of
logging from many threads stays a burst of independent cheap inserts, and no
thread's tail latency spikes because another thread is mid-log. The second is
**one consumer, ordered output**: a single drain thread emits lines in slot order
with no write lock and no interleaving. A line due within 1 ms of the call is moved to
call time + 1 ms, whatever its kind: `max(due, now + 1 ms)` is non-decreasing in both the due
time and the call time, so the first of two lines due in logging order never lands in a
later tick, and the wheel keeps insertion order within a tick (`test/order.cc`).

There is an honest wrinkle worth stating. The routine "now" logs all land near the
current instant, which is the convergent-key pattern, the one the wheel is
*slowest* at (every producer piles onto the same leaf). That is fine here. stash's
worst measured arming rate is still millions of lines a second, far above any real
logging load, and the property that matters is that the arm is non-blocking, not
that it is the fastest possible. The deferred logs, meanwhile, spread across the
future, which is exactly the pattern the wheel is built for, and the
cancel-before-fire traffic costs almost nothing.

## Where the pieces live

- `logger.h` — `LogConfig`, `LogHooks`, the `Logger` sinks, `Logging` (the task +
  static facade), the `Log` handle, and the `L_*` macros. The macros use a
  priority-guard ternary plus a variadic `format_msg`, so a filtered line
  evaluates none of its arguments and no `lazy.hh` is needed.
- `logger.cc` — the three-path `add` router, `operator()` (emit/decorate), the
  cancel/swap in `clean`, the bounded `OnceFilter`, the default hooks, and
  backpressure.
- `scheduler.h` (from [Kronuz/scheduler](https://github.com/Kronuz/scheduler),
  pulled via FetchContent) — the stash-backed `Scheduler` whose one thread is the
  LOG thread.
