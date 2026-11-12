# The logger's deferred lines on Kronuz/wheel against arm D

The production logger (this branch, deferred lines on a `wheel::timer_wheel` the LOG thread owns)
against arm D of `docs/perf/wheel-ab.md`, the prototype it ports (`bench/wheel_ab/d/logger.{h,cc}`
at logger:afe86c5). The bar (quest A3, D72, D77): patterns 3 and 2 at P = 1, 4 and 14, none past
1.10 by the paired statistic of `docs/perf/wheel-ab.md` "Rule for (D)".

**Verdict: no row is past 1.10. All six rows are no worse than arm D on median, p99 and heap.**
The worst point estimate is p99 at pattern 2, P = 14 (r 1.036, interval 0.897-1.173); the worst
median is pattern 2, P = 4 (r 1.013, interval 0.966-1.044).

## Which arm is which

`docs/perf/wheel-port.sh` runs `bench/wheel_ab`'s harness and `summarize.py --paired` with the
labels of `run.sh row-d` swapped, because the summary's reference is always labelled A and its
candidate D:

| label | binary | what it is | wheel pin |
|---|---|---|---|
| A (reference) | `logger_wheel_ab_d --backend D` | arm D, `bench/wheel_ab/d/logger.cc` | `Kronuz/wheel@45f98eb` |
| A2 (A/A twin) | `logger_wheel_ab_d --backend D` | the same binary as A | same |
| D (candidate) | `logger_wheel_ab --backend A` | the production logger, built from the repository root | `Kronuz/wheel@bfd72bc` |

So **r = D / A is production logger / arm D**, A2 / A is arm D against itself (the noise band w),
and the harness, workload, process-per-run order (each arm takes each place of the round's
order in rotation), warm-up and statistic are those of `wheel-ab.md`'s Arm D rows: 40 rounds a
row, one warm-up and one measured round per process, r the median over rounds of the within-round
ratio, a distribution-free 95% interval of the median, w the farther end of A2 / A's interval
from 1.00. Both arms are built in one `cmake` tree (`bench/wheel_ab/d/CMakeLists.txt`) at the
same commit, with the same compiler, on one host. Because r is the median of the paired ratios,
it is not the ratio of the printed medians.

Host: Intel Core i9-9980HK, 16 threads, macOS 15.8.1, Apple clang 17.0.0 (clang-1700.6.4.2).
Jobs ran `exclusive` at logger:11644a54c61b (the commits after it change only docs and
CHANGELOG.md), each exit 0.

## Rows

Bound: r(median ns) <= 1.10, r(p99 ns) <= 1.10, heap per call of D <= 1.10 times A's.

| row | job | r median [interval] (w) | r p99 [interval] (w) | heap B/call A / D | no worse than A |
|---|---|---|---|---|---|
| pattern 3, P = 1 | 261112-140421-86b4c3 | 0.997 [0.928-1.019] (0.089) | 1.012 [0.875-1.165] (0.176) | 267.7 / 267.7 | yes; p99 straddles 1.10 |
| pattern 3, P = 4 | 261112-140421-86b4c3 | 1.010 [0.973-1.041] (0.039) | 1.035 [0.903-1.111] (0.104) | 267.7 / 267.7 | yes; p99 straddles 1.10 |
| pattern 3, P = 14 | 261112-140421-86b4c3 | 0.992 [0.982-1.017] (0.029) | 0.979 [0.908-1.100] (0.063) | 267.7 / 267.7 | yes |
| pattern 2, P = 1 | 261112-140412-ee682f | 1.009 [0.838-1.196] (0.171) | 1.002 [0.859-1.200] (0.190) | 267.2 / 267.2 | yes; median and p99 straddle 1.10 |
| pattern 2, P = 4 | 261112-140415-a16bfe | 1.013 [0.966-1.044] (0.053) | 0.990 [0.910-1.065] (0.086) | 267.2 / 267.2 | yes |
| pattern 2, P = 14 | 261112-140418-0c17c8 | 0.987 [0.954-1.005] (0.029) | 1.036 [0.897-1.173] (0.141) | 267.2 / 267.2 | yes; p99 straddles 1.10 |

Pattern 3's three rows are one job (P = 1, 4, 14); a pattern 2 row takes about 5 minutes, so each
is its own job (an exclusive job is capped at 10 minutes). Allocations per call are identical
(1.000 in pattern 2, 1.010 in pattern 3) and the held-line counts are identical in every row.

The point estimates are inside the bound on all twelve (median, p99) cells. The p99 intervals
and the pattern 2 P = 1 median interval reach past 1.10 in five rows, as `wheel-ab.md` found for
arm D against the heap (D77 reports a straddle and does not gate on it). Arm D against itself
(A2 / A) spans 0.919-1.176, 0.896-1.099, 0.937-1.013, 0.885-1.190, 0.914-1.037 and 0.898-1.141
on p99 in the rows' order above, so at P = 1 and in pattern 2 the p99 noise alone is as wide as
the interval; at pattern 3 P = 4 and 14 the candidate's interval is wider than the twin's, and its
point estimates there are 1.035 and 0.979.

## Outside the rule

Reported because D72 is about the heap and the wheel's resolution. r = production / arm D, with
the interval; A2 / A in the last column.

| row | lag p99 us A / D (r) | lag A2/A r | LOG CPU ns/call A / D | LOG CPU r [interval] | A2/A r |
|---|---|---|---|---|---|
| pattern 3, P = 1 | 8,879 / 9,385 (1.042 [0.918-1.152]) | 1.031 | 281.8 / 287.2 | 0.978 [0.923-1.036] | 1.031 |
| pattern 3, P = 4 | 10,886 / 10,639 (1.001 [0.922-1.096]) | 0.979 | 269.5 / 282.5 | 0.995 [0.909-1.061] | 0.961 |
| pattern 3, P = 14 | 13,782 / 14,264 (1.045 [0.970-1.168]) | 0.994 | 273.4 / 281.5 | 1.024 [0.979-1.099] | 1.047 |
| pattern 2, P = 1 | 1,365 / 1,370 (1.005 [0.992-1.017]) | 1.003 | 2,604.6 / 2,628.2 | 1.158 [0.919-1.478] | 0.902 |
| pattern 2, P = 4 | 1,122 / 1,124 (1.000 [0.991-1.012]) | 1.000 | 924.6 / 851.2 | 0.995 [0.912-1.019] | 0.983 |
| pattern 2, P = 14 | 1,105 / 1,109 (1.001 [0.987-1.016]) | 1.000 | 789.1 / 794.8 | 1.022 [0.799-1.200] | 1.069 |

The fire lag and the LOG thread's CPU of the production logger are those of arm D within noise
on every row: the port keeps the wheel's 1 ms slot (pattern 2's lag p99 is about 1.1 to 1.4 ms,
the tick plus the LOG thread's wake) and the LOG thread's per-call cost. Pattern 2 P = 1 LOG CPU
has the widest interval (0.919-1.478), and arm D against itself spans 0.734-1.403 there.

## Correctness beside the timing

`logger_deferred` (once, in order with its peers, never early, within one tick plus the LOG
thread's wake) and `logger_deferred_stress` pass in the suite (261112-140315-039e6c) and under TSan
(261112-140332-797330), at logger:be2106cd8e43.

## Reproduce

```sh
docs/perf/wheel-port.sh 3 40        # pattern 3 at P = 1, 4, 14
docs/perf/wheel-port.sh 2 40 1      # pattern 2 at P = 1 (then 4, 14)
```

Exit 1 when a row's median or p99 r is past 1.10 or its heap past 1.10 times arm D's. Run it in
`exclusive` jobs; the raw per-run lines and each summary are in `.scratch/wab-d/port-PATTERN-P.log`.
