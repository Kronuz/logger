# Xapiand's logger: deferred, lock-free logging on the wheel

The logger is the original reason [stash](https://github.com/Kronuz/stash) exists.
It is the cleanest example of what the wheel is for: many threads arming
time-keyed work that a single thread drains, where most of the arming sits on a
hot path and a lot of it is cancelled before it ever runs.

This documents how the logger actually works, because the shape is a little
different from what you would guess. It is also the design basis for pulling the
logger out into its own Kronuz library (see [EXTRACTION.md](EXTRACTION.md)).

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

2. **Async, "now."** The calling thread renders the message to a string, wraps it
   in a `Logging` task keyed at the current instant, and hands it to the
   scheduler. The single LOG thread picks it up on its next pass and does the
   decoration and the write. This is the path for `ERR` and everything less
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

## Why stash, specifically

The logger does not lean on stash for peak throughput. It leans on two things
stash gives that a locked structure does not.

The first is that **the arm never blocks**. When every worker thread logs at once
they do not serialize behind a log mutex; the insert is lock-free, so a burst of
logging from many threads stays a burst of independent cheap inserts, and no
thread's tail latency spikes because another thread is mid-log. The second is
**one consumer, ordered output**: a single drain thread emits lines in slot order
with no write lock and no interleaving.

There is an honest wrinkle worth stating. The routine "now" logs all land near the
current instant, which is the convergent-key pattern, the one the wheel is
*slowest* at (every producer piles onto the same leaf). That is fine here. stash's
worst measured arming rate is still millions of lines a second, far above any real
logging load, and the property that matters is that the arm is non-blocking, not
that it is the fastest possible. The deferred logs, meanwhile, spread across the
future, which is exactly the pattern the wheel is built for, and the
cancel-before-fire traffic costs almost nothing.

## Where the pieces live in Xapiand today

- `src/logger.h` / `src/logger.cc` — `Logging` (the task), the `Logger` sinks, the
  `add` router, and `operator()` (the emit/decorate).
- `src/logger_fwd.h` — the `Log` handle and the `L_*` macro surface (`L_WARNING`,
  `L_DELAYED_*`, `unlog`, the lazy argument machinery).
- `src/scheduler.h` — the stash-backed `Scheduler` whose one thread is the LOG
  thread. Already extracted as [Kronuz/scheduler](https://github.com/Kronuz/scheduler).
