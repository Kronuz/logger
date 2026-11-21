# Source layout

What each source file holds, and why the large ones stay whole. The rules for cutting a file are
cpp-libraries' `docs/SPLITTING.md`.

| File | Holds |
|---|---|
| `logger.h` | The public header and the umbrella: config, hooks, sinks, the per-thread queue records, `Logging`, `Log` and the `L_*` macros. Includes `logger_format.h`. |
| `logger_format.h` | The message formatter: a thread's scratch buffers (`ScratchSlot`), the fast path for plain `{}` formats (`fast_format*`), and `FormattedMsg`. All in `logger_detail`. |
| `logger.cc` | The implementation: clocks, the queues and the LOG thread (`LogWorker`), `Logging`, `Log`, the sinks, color rendering and the iTerm2 integration. |
| `logger_timestamp.cc` | `logger_detail::default_timestamp`: the three timestamp styles, the grey gradient and the per-thread cached local time. |

## Kept whole

`logger.cc` (about 2,050 lines) stays one translation unit:

- `LogWorker` is one class, 550 lines of in-class members, defined in the TU-local part of
  `logger_detail` together with the rings it reads. `Logging`'s out-of-line members reach it
  (`get`, `join`, `push_task`), so a piece holding the worker would need the class in an internal
  header with its members cut out of the class body.
- The statics that decide the exit order (`Logging::config`, `hooks`, `handlers`, the `torn_down`
  flag and the `Teardown` object defined right after them, `docs/exit-lifetime.md`) must be
  initialized, and destroyed, in one order that only one translation unit guarantees.
- The sinks and the color rendering (about 140 lines) share `write_all`, `is_tty` and `render` with
  the LOG thread, the iTerm2 code and `Logging`; cutting them out costs an internal header and
  external linkage for the three helpers, for a piece that does not stand alone.
- The clocks (`steady_now`, `stamp_time`, `system_at`, about 115 lines) share `mach_clock` with the
  worker's start.

`logger.h` (about 770 lines) keeps the queue records, `Logging`, `Log` and the macros together:
the inline `queue_line` and the macros use all of them and are one contract, the header users
include.
