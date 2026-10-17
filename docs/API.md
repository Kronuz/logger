# logger API reference

## `LogConfig` (`Logging::config`)

`log_level` (emit when `priority <= log_level`), `color` (`LogColorMode`:
`automatic` / `always` / `never`, mirroring a `--color` flag), `color_depth`
(`LogColorDepth`: `automatic` / `ansi16` / `ansi256` / `truecolor` / `stacked`,
the tier term-color's stacked escapes are collapsed to), `with_timestamp` plus
`timestamp` (`LogTimestamp`), `precision` (`LogPrecision`), and
`timestamp_gradient`, `with_threads`, `with_location`, `iterm2` (emit iTerm2 marks
/ tab tint / badge when stderr is an iTerm2 terminal), `markers` (the per-priority
severity bar), `tint` (color each message in its severity's palette color, as Xapiand's
lines read; off by default), and `max_pending` (backpressure: 0 = unbounded, otherwise routine
async lines are dropped past it with a coalesced summary).

## `LogHooks` (`Logging::hooks`)

Optional, each with a std default: `describe_exception`, `backtrace`,
`thread_name`, `timestamp`. A host injects richer versions (e.g. real backtraces,
named threads) without the core depending on them.

## Sinks (`Logger`)

`StreamLogger` (a file), `StderrLogger` (tty-aware), `SysLog`. Install with
`Logging::add_handler(...)`; every line fans out to all of them.

## Macros

`L_INFO` / `L_NOTICE` / `L_WARNING` / `L_ERR` / `L_CRIT` / `L_ALERT` / `L_EMERG`,
`L_EXC` (the in-flight exception), the `*_ONCE` and `*_ONCE_PER_MINUTE` variants,
`L_DELAYED_*` with `L_DELAYED_N_UNLOG` / `L_DELAYED_N_CLEAR`, `L_TIMED`,
`L_STACKED` (nested indent), `L_PRINT` (undecorated), and `L_DEBUG` (compiled out
unless `LOGGER_DEBUG`). A line below the level evaluates none of its arguments.

## The `Log` handle

Returned by the deferred macros. `clear()` cancels the pending line; `unlog()`
swaps its message; `age()` reports how long it lived. Dropping the handle runs
the cancel/swap.
