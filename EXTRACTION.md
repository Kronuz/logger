# Extracting the logger

The reusable idea is the deferred/async logging machinery on top of the Kronuz
[scheduler](https://github.com/Kronuz/scheduler): the three-path routing (inline /
async-now / deferred-future), the `Logging` task, the `Log` handle with
`clear()`/`unlog()`, the sink interface, and the once-dedup. See
[ARCHITECTURE.md](ARCHITECTURE.md) for how it fits together. This is the part
worth handing to a stranger.

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

1. New repo `Kronuz/logger`, consuming `Kronuz/scheduler` (which pulls
   `threadpool` + `stash`) plus `term-color`, `located-exception`, `datetime`,
   `hashes`, `base-x` via FetchContent.
2. Lift `Logging` + `Log` + the sink interface + the three-path `add()` with the
   already-extracted deps; stub `opts` with a `Config` and the three hooks at std
   defaults.
3. Port the macro surface (`L_*`, `L_DELAYED_*`, `unlog`) minus the Xapiand-only
   hooks; keep colors via `term-color`.
4. Replace `strings`/`io`/`repr` with `std::format`/`::write`/a tiny escape; vendor
   `bloom_filter` and `lazy`.
5. A demo and a test: arm-and-cancel, deferred-fires-if-not-cancelled, once-dedup,
   single-consumer output ordering.
6. Back-port: Xapiand consumes `Kronuz/logger` and provides the `Xapian::Error` +
   iTerm2 + named-thread adapter.

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
