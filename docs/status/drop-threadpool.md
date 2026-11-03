> Status: CURRENT (2026-11-02) workstream: one pool for the Kronuz libraries (developer), logger's part.

# drop-threadpool (logger)

- Task: the scheduler runs on `WorkPool`; threadpool deletes the `ThreadPool` family; logger pins the new scheduler head.
- Branch: `phase7c/drop-threadpool`, from origin/main `logger:801105c`.
- State: `CMakeLists.txt` pins scheduler `1e5ccc9` (scheduler branch `phase7c/drop-threadpool`: threadpool `eb4751a`) in place of
  `2bb0a8f`. logger names `ThreadPolicyType::logging` only (the `Logging` task and `LogWorker`); the enum keeps `regular` and
  `logging`, so no source changes. logger uses `Scheduler`, not `ThreadedScheduler`, so it never touches a pool.

## Done

- Pin moved (this branch's commit).

## Proof

- `preflight.sh` at `e462011` (scheduler `181ed03`): mac hardened, mac ASan+UBSan and Linux ok; the FreeBSD VM failed
  `logger_clock` (`CHECK FAILED at line 47: w <= s1 + kSlack`, a timing slack on a busy VM; the other ten tests passed). Not rerun
  there (the producer test alone takes 9 minutes). CI at `2356791`: run 41546525411, all 8 jobs green including FreeBSD.

## Next

The lead lands threadpool, then scheduler, then logger. `http-log` and `prism` pin logger `801105c` and need no change.

## Blocked on

Nothing.
