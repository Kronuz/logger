# What the logger reaches at exit

The LOG thread writes lines while the process exits. A process that returns from `main` without
`Logging::finish()` stops it from a static's destructor, the *stop* (`Finisher` in
`LogWorker::get()`), which ends the LOG thread with one pass and joins it for 200 ms, or, if the
join times out, abandons it and waits until it parks. Until the stop returns, the LOG thread drains
the queues and calls `Logging::emit`; a line refused after the stop began is written by
`Logging::emit` on the thread that logged it, which may be a static's destructor anywhere in the
process.

## The rule

The stop is made at the first queued line, so the statics made before it (every file's
namespace-scope statics, in practice) are destroyed after it, and the LOG thread no longer runs when
they go. Whatever the logger makes after the stop is destroyed before it, while the LOG thread still
runs. So everything the logger owns that `Logging::emit` or the drain reaches is one of:

1. Built at compile time with no destructor (`constexpr`).
2. Trivially destructible (atomics, integers, plain structs), so there is nothing to destroy.
3. Never destroyed: held in `Immortal<T>` (logger.cc), a union whose destructor does not destroy
   its member.

Reordering definitions does not meet the rule: the order a function-local static is made in
depends on which line comes first.

## The defect this fixes

`Logging::emit` deduplicated `once` lines through a function-local `static OnceFilter filter`, made
by the first `once` line, after the stop. At exit its destructor ran first; the LOG thread, still
draining (and then in the stop's last look, `LogWorker::close`), locked its destroyed mutex and read
its freed sets. LLVM's libc++ (22) throws `system_error: mutex lock failed` there, which ends the
process; Apple's libc++ does not, and the freed sets were read silently. Xapiand's aggregations
test aborted 20 of 20 at exit under LLVM 22 (Xapiand campaign job 261118-163254-b09a49).

`test/exit.cc` (`logger_exit_test once`, ctest `logger_exit_once`) queues `once` lines as `main`
returns, and checks each distinct line is written once, by a handler whose destructor runs after the
stop.

## Every static on the path

What `Logging::emit` reaches (decoration, `once` filter, sinks), what the drain reaches
(`LogWorker::pass`, `advance`, `fire_head`, `fire`, `close`, the wheel), and what a refused line's
inline write reaches.

| Static | Kind | Why it is safe at exit |
|---|---|---|
| `OnceFilter filter` in `Logging::emit` | function-local, made after the stop | `Immortal`: never destroyed |
| `priorities`, `tints` | namespace scope | `constexpr` `string_view`s over `constexpr` term-color escapes: no destructor |
| `kGrey60`, `kGrey94`, `kGrey162`, `kGrey230` | namespace scope | `constexpr` term-color escapes: no destructor |
| the empty color in `default_timestamp` | was a function-local `std::string` | gone: the gradient returns an empty `string_view` |
| `Cache` in `local_tm` | `thread_local` | trivially destructible (TZ kept in a 64-byte array; a longer TZ is not cached), so a line written after the thread's thread-locals are destroyed finds it |
| `shared_ring_mutex` | namespace scope | `Immortal`: never destroyed |
| `ring_list`, `shared_ring` | namespace scope | pointers; a queue (`Ring`) is never freed |
| `clock_offset` | namespace scope | atomics: trivially destructible |
| `Logging::completed_`, `Logging::dropped_` | static members | atomics: trivially destructible |
| `worker_state`, `worker_wake`, `worker_notifies` | inline (logger.h) | atomics: trivially destructible |
| `thread_state`, `scratch_slot` | `thread_local` (logger.h) | trivially destructible; their thread's exit marks them `dead`, and a later line takes the shared queue and formats into its own string |
| `MachClock` in `mach_clock()` | function-local | trivially destructible |
| `tick` in `tick_stamps()`, `tty` in `is_tty()`, `is_iterm` in `iterm2_available()` | function-local | integers: trivially destructible |
| `worker` in `LogWorker::get()` | function-local | the LOG thread's object is never deleted |
| `finisher` in `LogWorker::get()` | function-local | the stop itself |
| `n` in `LogWorker::name()` | function-local | read once, when the LOG thread starts; never at exit |
| `Logging::config`, `Logging::hooks`, `Logging::handlers` | static members, the host's | made before the stop (namespace scope in logger.cc), destroyed after it: the LOG thread has stopped or parked when they go. A line logged inline by a static destroyed after them is the host's to avoid (`Logging::finish()` before the statics tear down) |
| `seq`, `mine` in `wheel::timer_wheel::stripe()` | Kronuz/wheel | atomic and integer: trivially destructible |

term-color's `apply` and `collapse` keep no statics.

## What the stop does not cover

- What the host's hooks and handlers reach is the host's. Xapiand's `thread_name` hook calls
  workpool's `get_thread_name`, which reads the namespace-scope `thread_names` map (made before the
  stop: safe) and may return a function-local `static std::string` (made after it: destroyed first).
- After the join returns, the LOG thread's runner still calls workpool's `teardown_thread` (the
  host's `WORKPOOL_THREAD_UNREGISTER`) while the statics are destroyed.
- A line queued before logger.cc's statics are initialized (from another file's static initializer)
  makes the stop before `config`, `hooks` and `handlers`, so they are destroyed before it.
- Keeping `config`, `hooks` and `handlers` alive for good (references to `Immortal` storage, the
  same expressions in the host) would cover inline lines logged after their destruction too, at
  the cost of the handlers' destructors no longer running at exit; that is a behavior change, left
  open.
