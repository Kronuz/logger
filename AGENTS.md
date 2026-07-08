# AGENTS.md

Notes for agents and contributors working in this repo. For internal design see
ARCHITECTURE.md; for usage see README.md; for the open ideas see IMPROVEMENTS.md.

## Repo map

- `logger.h` — the public API: `LogConfig`, `LogHooks`, the `Logger` sink
  interface (`StreamLogger` / `StderrLogger` / `SysLog`), `Logging` (the log
  record, which is also a `ScheduledTask`, plus the static facade), the `Log`
  handle, and the `L_*` macro surface. Also the `format_msg` / `print_msg`
  helpers and the `log_indent()` / `LogIndent` thread-local indent.
- `logger.cc` — implementations: the sinks, the three-path `add()` router,
  `operator()` decoration, `clean` / `clear` / `age`, the bounded `OnceFilter`,
  the default hook bodies, and backpressure. This file must be compiled and
  linked; the library is not header-only.
- `examples/demo.cc` — a small runnable showcase (level macros, the deferred
  slow-op line, once-dedup, stacked indent).
- `test/test.cc` — a single self-contained smoke test using a `CHECK` macro
  (see invariants below), no framework.
- `CMakeLists.txt` — FetchContents `scheduler`, builds the static library, and
  wires the test and demo when this repo is the top-level project.

## Build & run

```sh
cmake -B build && cmake --build build
ctest --test-dir build      # or: ./build/logger_test
./build/logger_demo         # the showcase, to stderr
```

The test and demo targets are only added when this repo is top-level, so a
consumer vendoring it via `FetchContent` won't build them.

Sanitizers: use Homebrew LLVM, not Apple clang (whose ASan/TSan are broken on
recent macOS):

```sh
cmake -B build-tsan -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=thread -g -O1"
cmake --build build-tsan && ./build-tsan/logger_test
```

## Conventions

- C++20. Double quotes in code. No em dashes in docs.
- MIT-licensed; keep the copyright header (Copyright (c) 2015-2019 Dubalu LLC) on
  source files.
- The core depends on `Kronuz/scheduler` and `Kronuz/term-color` (the palette plus
  the `collapse`/`apply` resolution the sink runs per line), plus `cppcodec`
  (header-only) used only in `logger.cc` to base64-encode the iTerm2 badge text.
  Everything *host-specific* (exception description, real backtraces, thread names,
  timestamp format) goes through `LogHooks`, not a new dependency. Do not pull
  `located-exception`, `Xapian::Error`, etc. into the core; a consumer injects those
  via hooks.
- Keep `Xapian::Error` and real backtraces out of the core; they reach it through
  `LogHooks` (a consumer-side adapter, see EXTRACTION.md). The iTerm2 integration
  (marks / tab tint / badge / notifications, gated by `config.iterm2`) does ship in
  the core: the extraction's open question was resolved in its favor.

## Load-bearing invariants

- **Tests use `CHECK`, not `assert`.** The CMake build can be `Release`
  (`-DNDEBUG`), which compiles `assert` out, so a test written with `assert`
  verifies nothing. `test/test.cc` defines a `CHECK` that aborts regardless. This
  already caught a wrong expectation once; keep it.
- **The cancel/swap semantics are exact.** A deferred line that `clears` and is
  cancelled *before it fires* stays silent, and its swap (`unlog`) message is
  dropped too; a line whose timeout already fired keeps its swap. This rests on
  the one-shot `clear(true)` CAS result in `clean()` (`logger.cc`): if `clear(true)`
  wins, the line had not fired, so suppress the swap. Don't "simplify" that
  branch.
- **The LOG thread is the consumer and the writer.** There is one scheduler
  thread; it drains the wheel and does the `write`s. Call `Logging::finish()` at
  shutdown before the statics tear down, or pending async lines can outlive their
  sinks.
- **`make_format_args` needs lvalues.** `format_msg` passes `args...` (not
  `std::forward`ed) into `std::make_format_args`. Do not change it to
  `std::forward<Args>(args)...`; that reintroduces the rvalue temporaries it
  rejects.
- **`L_EXC` undefs a stub.** stash and scheduler bundle a no-op `L_EXC` (and
  `L_NOTHING` / `L_DEBUG_HOOK` / `L_CALL`) so they build without a logger. The
  macro block `#undef`s `L_EXC` before defining the real one. Keep that undef.
- **`config` / `hooks` / `handlers` are read unsynchronized.** Install handlers
  and set config before logging starts. Mutating them while the LOG thread runs
  is a data race (see IMPROVEMENTS.md).
- **Indent is captured at creation, on the producer thread.** `log_indent()` is
  `thread_local`; a line records the depth when it is built (on the producer),
  not when it fires (on the LOG thread). Keep the capture in the `Logging`
  constructor.
