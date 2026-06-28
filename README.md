# logger

Deferred, lock-free logging built on the Kronuz
[scheduler](https://github.com/Kronuz/scheduler) and
[stash](https://github.com/Kronuz/stash) wheel. Extracted from
[Xapiand](https://github.com/Kronuz/Xapiand), where it is the original reason the
wheel exists.

The idea in one line: many threads arm log lines without ever blocking each other,
a single thread drains and writes them in order, and a scheduled "this was slow"
line can be cancelled with one atomic when the operation finishes in time, so the
common case logs nothing.

**Status: planning.** This directory currently holds the design, not the code:

- [ARCHITECTURE.md](ARCHITECTURE.md) — how Xapiand's logger works today: the three
  paths a line can take (inline / async-now / deferred-future), why the scheduler's
  consumer thread *is* the logging thread, and why stash is the right backing store.
- [EXTRACTION.md](EXTRACTION.md) — the plan to lift it into this library: which
  dependencies are already Kronuz libs (most of them), which tethers remain (mainly
  the global `opts`), and the decoupling boundary (a `Config` plus three hooks).
