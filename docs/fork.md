# A fork child and the logger

`fork()` copies the process with one thread, the one that called it. The LOG thread is not in the
child, but everything it left in memory is: its mutex (`LogWorker::mtx`), possibly locked, its
condition variable (`LogWorker::cv`), possibly with the LOG thread counted as a waiter, and
`worker_state`, possibly `kSleeping`.

## The reset

When the LOG thread starts, `LogWorker::get()` registers a `pthread_atfork` child handler,
`LogWorker::after_fork_child`, which constructs `mtx` and `cv` anew in place. After it, a line logged
in the child that finds `kSleeping` set wakes nothing (there is no waiter) and returns, and
`Logging::finish()` signals nothing and returns; neither can wait on a mutex the parent's LOG
thread held at the fork.

The handler resets rather than locks around the fork (a `prepare` handler that takes `mtx` and a
`parent`/`child` pair that releases it) because:

1. `mtx` guards no data. The LOG thread's state is atomics; `mtx` exists only to pair with `cv` for
   the sleep, so a fresh mutex in the child loses no invariant that a lock held over the fork would
   keep.
2. `cv` needs the reset anyway. The LOG thread spends most of its time waiting on it, so at most
   forks the child's copy counts a waiter that does not exist; glibc's condition variable keeps
   per-group waiter counts that a signal can wait on.
3. A `prepare` lock makes every fork in the host wait for the LOG thread to leave its sleep setup,
   and deadlocks a fork from a thread that holds `mtx`.

The handler only stores the objects' initial bytes (`std::mutex` and, on libc++ and libstdc++,
`std::condition_variable` are built from the static initializers), with no libc call, so it is safe
in the child of a threaded process, where only async-signal-safe calls are.

## What a child that keeps running gets

A child that execs needs nothing from the logger. A child that keeps running without exec gets:

- Its lines are queued and never written: no LOG thread runs in the child, and none is started.
  A line logged in the child does not block, and the child's exit does not wait (the exit-time stop
  is skipped when the process is not the one the LOG thread runs in, `docs/exit-lifetime.md`).
- `Logging::finish(0)` returns at once; `finish(n)` waits its `2n * 100` ms for a LOG thread that is
  not there and returns false.
- A line refused once `finish()` began, or a severe one, is written inline as in any process.

Logging in such a child before exec is outside POSIX's async-signal-safe set (a thread's first line
allocates its queue); it is as safe as the libc's `malloc` after a fork.

## What it does not cover

Two other mutexes have the same hazard and keep data, so a reset would not be enough: the shared
queue's `shared_ring_mutex` (held by a thread logging from another thread-local's destructor) and the
`once` filter's (held while it inserts). A child forked while another thread holds one blocks on its
first line through it. Neither is held by the LOG thread across a sleep.

## The proof

`test/exit.cc`'s `child-fork-held` (run by ctest's `logger_exit_fork`) holds the mutex at the fork
with `logger_detail::sleep_hook`, which the LOG thread calls as it goes to sleep with the mutex held
and `kSleeping` set. The child logs a line and calls `finish(0)` and must exit within 5 s; the parent
then writes all of its lines. `test/fork_mutant.sh` builds the tree without the `pthread_atfork`
registration and requires the test to fail.
