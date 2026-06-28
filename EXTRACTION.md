# Extracting the logger

The reusable idea is the deferred/async logging machinery on top of the Kronuz
[scheduler](https://github.com/Kronuz/scheduler): the three-path routing (inline /
async-now / deferred-future), the `Logging` task, the `Log` handle with
`clear()`/`unlog()`, the sink interface, and the once-dedup. See
[ARCHITECTURE.md](ARCHITECTURE.md) for how it fits together. This is the part
worth handing to a stranger.

## Status

Phases 1 through 5 are implemented and TSan-clean (see the git history): the sinks
and immediate path, the three-path router, the cancel/swap deferred mechanism,
hook-driven decoration, the bounded `once` filter, backpressure, the full `L_*`
macro surface, a demo, and a test. **Phase 6 (the Xapiand back-port + adapter) is
the remaining work.**

The implementation went further on dependencies than the analysis below assumed:
the core depends on **`scheduler` only**. The richer libs the tables call out
(`located-exception`, `datetime`, `term-color`, real backtraces, named threads)
are not pulled in; they reach the core through `LogHooks` with std defaults, so a
consumer re-injects them. Colors use a small built-in palette, and `once` uses a
bounded two-set filter rather than a vendored bloom, so neither `bloom_filter`
nor `lazy.hh` was vendored. The tables below remain as the dependency analysis
that informed those choices.

## Good news: most of the heavy dependencies are already Kronuz libs

`extract-and-modernize` already consumes these via FetchContent, and the logger
uses every one of them. They are drop-in:

| logger needs | already a lib |
| --- | --- |
| the wheel / `Scheduler` | `Kronuz/scheduler` (pulls `threadpool` + `stash`) |
| `ThreadPolicyType` | `Kronuz/threadpool` |
| `CLEAR_COLOR`, `rgb()`, the color toolkit | `Kronuz/term-color` (the per-level `*_COL` palette is Xapiand-local; bring it or redefine) |
| `BaseException` | `Kronuz/located-exception` (has `BaseException`; **not** `traceback`/`BACKTRACE`, which stay behind the backtrace hook) |
| timestamp formatting | `Kronuz/datetime` |
| `fnv1ah32` (hook hashing) | `Kronuz/hashes` |
| Base64 (iTerm2 badge only) | `Kronuz/base-x` |

So the logger is not the eight-header decoupling the old backlog note feared. The
backbone is already standalone.

## The tethers that remain (the actual work)

Only a handful, and only one is real:

| tether | used for | move |
| --- | --- | --- |
| `opts.h` (global `opts`) | timestamp style, threads/location toggles, iterm2, log level | **the real work**: replace the global with a `Logger::Config` struct |
| `xapian.h` (`Xapian::Error`) | one `catch` arm in `operator()` | drop from core; lives in a Xapiand adapter |
| `thread.hh` (`get_thread_name`) | the `(thread)` column | a `thread_name(id)->string` hook, default `std::thread::id` |
| `io.hh` (`io::write`) | the sink write | `::write` / `std::fwrite` |
| `strings.hh` (`format`, `from_delta`) | message format + age annotation | `std::format` + a ~15-line duration formatter |
| `repr.hh` (`repr`) | the format-error fallback only | inline a minimal escape, or drop the fallback |
| `bloom_filter.hh` | the `once` dedup | vendor (small) or make the once-feature optional |
| `lazy.hh` (`LAZY`) | lazy arg eval in the macros | vendor (tiny, header-only) |

## The decoupling boundary

One config plus a few optional hooks, all with std defaults:

- **`Logger::Config`** — the format toggles (timestamp style, threads, location,
  colors), the async-level cutoff, and the list of sinks. Replaces every read of
  the global `opts`.
- **Hooks** — `describe_exception(eptr) -> string`, `backtrace() -> frames`,
  `thread_name(id) -> string`. The core ships std defaults; Xapiand supplies the
  ones that special-case `Xapian::Error`, real backtraces, and named threads.

Drop the `Xapian::Error` arm and the iTerm2 escapes from the core. They become a
thin Xapiand-side adapter.

## Steps

1. **Done.** New repo `Kronuz/logger`, consuming `Kronuz/scheduler` only (which
   pulls `threadpool` + `stash`). The other libs were not needed: their roles
   became hooks.
2. **Done.** Lifted `Logging` + `Log` + the sink interface + the three-path
   `add()`; replaced the global `opts` with `LogConfig` and added `LogHooks` at
   std defaults.
3. **Done.** Ported the macro surface (`L_*`, `L_DELAYED_*`, `unlog`). A
   priority-guard ternary plus a variadic `format_msg` replaced the `LAZY` +
   arg-counting machinery, so no `lazy.hh`. Colors use a small built-in palette,
   not `term-color`.
4. **Done.** `std::format` / `::write` / a linear `strip_colors`. The `once`
   filter is a bounded two-set structure, so no `bloom_filter` vendoring.
5. **Done.** A demo and a test (arm-and-cancel, deferred-fires, once-dedup,
   single-consumer ordering, the macros), passing under Release and TSan.
6. **Remaining.** Back-port: Xapiand consumes `Kronuz/logger` and provides the
   `Xapian::Error` + iTerm2 + named-thread + real-backtrace adapter.

## Size

Medium, not large. The core is small and the backbone is already extracted; the
only non-mechanical piece is turning the global `opts` into a `Config` and putting
the exception/backtrace/thread-name specifics behind three hooks. Bigger than the
leaf libs were, smaller than the old note implied.

## Open checks before coding

- The `traceback()` / `BACKTRACE()` machinery is *not* in `located-exception` (only
  `BaseException` is). It stays in Xapiand and reaches the core through the
  `backtrace()` hook. Confirmed.
- Decide whether `once` (bloom dedup) and the iTerm2 integration ship in the core
  or as optional add-ons.
- Decide the sink interface's coloring story: keep per-sink color decisions, or
  resolve color once and pass plain bytes to the sinks.
