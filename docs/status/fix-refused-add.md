# Status: fix/refused-add

## State

Ready for review; not merged. Depends on Kronuz/scheduler `perf/lazy-rearm` (7f6b501), whose
`add` returns a `schedule_result`, and through it on stash `perf/window-epoch`.

## Done

- `Logging::add` checks the scheduler's `add` result. On a refusal (after `finish()` began, or
  a delay past the 24 h horizon) it undoes `scheduled` and `pending`, then by kind (owner's
  policy): a future `clears` watchdog stays armed and its cancel point (`clean()` or `clear()`)
  writes it, late and once, only if its deadline has passed, else discards it; a line already
  due, or a severe one (above `NOTICE`), is written inline now; a routine future line that does
  not clear is lost, as before (no cancel point; no caller in Xapiand or Detent).
- Tests, past the horizon while running and after `finish()`: a normal deferred line is not
  written at `do_log` time and fires later; refused watchdogs for fast work never appear,
  through a handle drop or `clear()`; refused ones whose work outlasts the delay appear once, at
  completion (the swap kept on a drop, cancelled by `clear()`); refused severe deferred lines
  are written at once; refused async lines, routine or not, are written; a refused routine
  deferred line is lost; `pending_count()` is exact across every refusal (0 before anything is
  scheduled).
- scheduler pinned to 7f6b501 (`schedule_result`); the policy goes by the line's kind, so every
  refusal reason takes the same path.

## Next

1. Code review with the scheduler and stash branches.
2. When they land, repin scheduler to its main commit.

## Blocked on

Nothing.
