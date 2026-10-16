# logger roadmap

Open design work, most important first. Done items live in the git history:
bounded backpressure (`LogConfig::max_pending`), the bounded two-set `once` filter,
the `thread_local` stacked indent, and hooks for exceptions, backtraces, thread
names, and timestamps.

## One slow sink stalls all logging

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

## Handlers and config are unsynchronized globals

`handlers` (a vector) and `config` are read by the LOG thread and the inline path
while a setup path can still be mutating them. Adding or removing a handler after
logging has started is a data race. In practice they are set once at init, but
nothing enforces it. Either freeze handler installation after the first log, or
hold the handler list behind an atomic `shared_ptr<const>` for safe hot-swap.

## The cancel/swap dance is subtle and deserves more tests

The "fast op stays silent, slow op logs once and then its completion" behavior
rests on the interplay of `clears`, the one-shot `clear()` CAS result,
`atom_cleaned_at`, and double-clean safety. It is correct, and the smoke test
covers the main cases, but a small change could quietly break the
silent-on-fast-op invariant. Worth a test that lands a user `clear()` exactly as
the scheduler fires, plus written invariants.

## The trace-stub macro clash

stash and scheduler bundle no-op `L_*` stubs
  (`L_EXC`, `L_NOTHING`, `L_DEBUG_HOOK`, `L_CALL`) so they build without a logger.
  The logger's real `L_EXC` collides with the stub; the fix here is a local
  `#undef L_EXC` before the real definition (which is also how Xapiand wires it,
  so the scheduler's destructor `L_EXC` becomes a real log there too). Cleaner
  upstream would be to guard those stubs with `#ifndef`, though include order
  (logger.h pulls scheduler.h before defining its macros) means the consumer
  still has to undef. Low priority; documented so it is not a surprise.
