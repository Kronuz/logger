# A producer's burst cost

What a producer pays per `L_INFO` call in a burst rose 1.48 times (1 producer) and 1.75 times
(4 producers) from logger:f61d8df to logger:907d27f. The cause is a wake rule `a07f3fe` brought in
with the wheel; the fix keeps the wheel and workpool and restores the f61d8df cost.

## Method

`bench/lg.cc` (identical across f61d8df..907d27f) built per commit with `bench/ab_build.sh`;
`bench/logger_ab.py` runs every arm once per round in a rotated order and reports, per arm and
row, the median over rounds of arm / f61d8df with a distribution-free 95% interval (Detent D77).
Host: Intel i9-9980HK, macOS 15 (Darwin 24.6), Apple clang, Release, `exclusive` jobs.

## Bisect

The commits between that change the library (`logger.cc`, `logger.h`, `CMakeLists.txt`), and the
fix, in one job of 20 interleaved rounds (261116-001349-19ec7c). Median ratio to f61d8df [interval]:

| arm | burst, 1 | back to back, 1 | end to end, 1 | burst, 4 | back to back, 4 | end to end, 4 |
|---|---|---|---|---|---|---|
| a07f3fe wheel | **1.446** [1.359-1.578] | 1.036 [0.980-1.121] | 0.960 [0.925-0.990] | **1.847** [1.768-1.966] | 1.129 [1.061-1.180] | 1.004 [0.963-1.027] |
| b121f2e | 1.549 [1.303-1.729] | 1.091 [0.999-1.132] | 0.957 [0.936-0.984] | 1.940 [1.635-2.378] | 1.151 [1.088-1.179] | 0.999 [0.969-1.027] |
| 3bdf664 | 1.476 [1.386-1.620] | 1.052 [1.006-1.151] | 0.980 [0.917-1.003] | 1.905 [1.642-2.345] | 1.081 [1.045-1.132] | 1.018 [0.954-1.048] |
| 4b5b847 | 1.545 [1.335-1.634] | 1.062 [1.015-1.096] | 0.961 [0.916-1.007] | 1.811 [1.723-1.952] | 1.127 [1.034-1.160] | 0.989 [0.961-1.021] |
| 907d27f workpool | 1.410 [1.381-1.686] | 1.101 [1.029-1.161] | 0.964 [0.924-0.989] | 1.822 [1.643-1.932] | 1.115 [1.034-1.167] | 1.006 [0.985-1.027] |
| fix | 0.980 [0.954-1.032] | 0.965 [0.925-1.033] | 0.980 [0.959-1.037] | 1.043 [0.990-1.056] | 0.979 [0.936-1.022] | 1.008 [0.988-1.029] |

The whole step is at `a07f3fe`; every later commit, the move to workpool included, is flat
against it within its interval.

## Mechanism

A producer that publishes a line while the LOG thread sleeps calls `wake_for(due)`. `a07f3fe`
ported the wheel A/B's change 3 (`docs/perf/wheel-ab.md`): wake the LOG thread when the line is
due within `kWakeSlackNs` (8 ms) of its wake, so a deferred line is filed in the wheel before it
is due. The rule applied to every line, routine ones included.

In a burst, the first line (nothing pending) is due at once and wakes the LOG thread; it writes
that line and sleeps until the next queued line is due, its call plus the 1 ms batching tick.
Every later line of the burst is due a little after that wake, so `due - 8 ms < worker_wake`
held for each: each line found the LOG thread sleeping again and woke it, paying a mutex and a
notify on the producer, while the woken LOG thread passed over the producer's queue and found
nothing due. At f61d8df the test was `due < worker_wake`, which no batched line met.

Back to back the producer rarely finds the LOG thread asleep, so the rows moved less (1.04-1.13);
end to end the extra wakes wrote lines slightly sooner (0.96 at 1 producer).

## Fix

`push_task` keeps the slack for deferred records (not `ordered`); routine lines and queue-order
records wake the LOG thread only when due before its wake, as at f61d8df. A queued line is
written, not filed, so it needs no slack. The deferred lines' wheel, their filing rules and their
lateness are unchanged.

`test/wake.cc` counts the producers' notifies of the LOG thread (`logger_detail::worker_notifies`)
over 1,000 batched lines 10 us apart and requires fewer than 50; `test/wake_mutant.sh` restores
the slack for routine lines and requires the test to fail.

The confirming 30-round A/B against f61d8df and its A/A are in the campaign journal
(logger-burst-cost).
