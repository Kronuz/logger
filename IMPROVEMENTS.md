# Improvements to consider

These surfaced while reading and porting Xapiand's logger. A few are port
artifacts already fixed in this extraction; most are genuine design weaknesses
worth fixing now that the logger is its own library rather than carried code.

## Production risks

### No backpressure: the wheel can grow without bound

Every async and deferred line becomes a `Logging` task on the stash wheel, held
until the LOG thread drains it. If the consumer cannot keep up, a slow disk, a
blocked syslog socket, or just a logging storm, the wheel grows without any cap.
Memory balloons, and nothing tells you it is happening. For a logging subsystem
that is a real way to take down the process it was meant to help debug.

The fix is a bounded mode: a high-water mark on pending lines, past which new
async lines are dropped (or coalesced) instead of queued, with a periodic
"dropped N log lines" summary so the loss is visible rather than silent. Severe
inline lines never queue, so they are never affected. This trades completeness
under overload for survival under overload, which is the right trade for logs.

### One slow sink stalls all logging

The single LOG thread does the actual writes, serially, to every handler. That
single-consumer design is what gives ordered, un-interleaved output, but it also
means one blocking sink freezes the whole channel. A full disk or a syslog
socket that stops draining stalls the LOG thread, and every async and deferred
line backs up behind it, including the deferred "this op is slow" warnings the
system leans on most exactly when things are going wrong.

The fix is to bound sink writes in time (non-blocking or timed writes, accounting
for what could not be written) or to isolate a slow or remote sink on its own
thread so its stalls do not block the fast local ones. The trade-off is a
per-sink queue and the loss of strict cross-sink ordering, so this should be
opt-in per sink, not the default.

### The `once` dedup filter grows forever

`L_*_ONCE` suppresses repeats with a Bloom filter that only ever grows. Over a
long-running process it saturates, its false-positive rate climbs, and a
first-time "once" line gets silently swallowed because the filter wrongly
believes it has seen it. A once-only alert that is supposed to fire exactly once
can be eaten after enough unrelated once-logs have gone by. Silent suppression of
a message you explicitly asked to see once is the worst failure mode for this
feature.

The fix is to bound it: two alternating filters reset on a timer, a counting
filter with a ceiling, or a small capped LRU set. Occasional re-emission is a far
better failure than permanent silent loss. The `ONCE_PER_MINUTE` variant should
key off a proper per-call-site rate limiter rather than sharing this same global
filter.

## Contention and correctness

### Per-thread indent uses a global lock and map — DONE (Phase 4)

The original tracked stacked-indent depth in an `unordered_map<thread::id,
unsigned>` under a shared mutex, touched on every stacked log construct and
destruct, all contention and map churn for something inherently per-thread. This
extraction made it `thread_local` (`log_indent()` + the `LogIndent` RAII guard):
a plain per-thread counter, captured into each line at creation, no map, no lock.

### Handlers and config are unsynchronized globals

`handlers` (a vector) and `config` are read by the LOG thread and the inline path
while a setup path can still be mutating them. Adding or removing a handler after
logging has started is a data race. In practice they are set once at init, but
nothing enforces it. Either freeze handler installation after the first log, or
hold the handler list behind an atomic `shared_ptr<const>` for safe hot-swap.

### The cancel/swap dance is subtle and deserves more tests

The "fast op stays silent, slow op logs once and then its completion" behavior
rests on the interplay of `clears`, the one-shot `clear()` CAS result,
`atom_cleaned_at`, and double-clean safety. It is correct, and the smoke test
covers the main cases, but a small change could quietly break the
silent-on-fast-op invariant. Worth a test that lands a user `clear()` exactly as
the scheduler fires, plus written invariants.

## Already addressed in this extraction

- **Per-line regex for color is gone.** The original ran a compiled `std::regex`
  over every emitted line to collapse color escapes to the terminal's depth. This
  extraction replaced it with a linear `strip_colors`. The full-fidelity path
  should resolve the palette once at config time and never re-parse per line.
- **The Xapian/iTerm2/backtrace coupling becomes hooks.** Rather than special-case
  `Xapian::Error` and shell out to traceback in the core, those reach the core
  through hooks (Phase 3), so the library carries none of it.

## Noted while wiring the macros

- **Trace-stub macro clash.** stash and scheduler bundle no-op `L_*` stubs
  (`L_EXC`, `L_NOTHING`, `L_DEBUG_HOOK`, `L_CALL`) so they build without a logger.
  The logger's real `L_EXC` collides with the stub; the fix here is a local
  `#undef L_EXC` before the real definition (which is also how Xapiand wires it,
  so the scheduler's destructor `L_EXC` becomes a real log there too). Cleaner
  upstream would be to guard those stubs with `#ifndef`, though include order
  (logger.h pulls scheduler.h before defining its macros) means the consumer
  still has to undef. Low priority; documented so it is not a surprise.

## Where these land

The two bounded-mode items (backpressure, once filter) are natural Phase 3 work,
alongside the hooks, since they are config and policy. The slow-sink isolation is
a later, opt-in sink feature. The `thread_local` indent and the handler-list
safety are small, do them with the macro/feature work in Phase 4.
