"""The rule of docs/perf/wheel-ab.md over one row's runs (run.sh row, row-c), or a smoke's checks.

A row's runs are, in order, the A/A pair and then six pairs AX XA AX XA AX XA, X being B or C;
pair i gives r_i = X_i / A_i, r is their median, and the A/A band is |A'/A'' - 1|.
"""

from __future__ import annotations

import json
import statistics
import sys
from typing import Any

METRICS = (
    "median_ns",
    "p99_ns",
    "heap_bytes_per_call",
    "allocs_per_call",
    "lag_p99_us",
    "log_cpu_ns_per_call",
    "held_peak",
    "held_ms_per_call",
)


def runs(path: str) -> list[dict[str, Any]]:
    with open(path) as f:
        return [json.loads(line) for line in f if line.startswith("{")]


def ratio(b: float, a: float) -> float:
    return b / a if a > 0 else float("inf") if b > 0 else 1.0


def smoke(rs: list[dict[str, Any]]) -> int:
    expect = [(b, p) for b in "AB" for p in (1, 2, 3)]
    got = [(r["backend"], r["pattern"]) for r in rs]
    bad = got != expect
    for r in rs:
        lagged = r["lag_lines"] > 0 or r["formatted"]
        if r["producers"] != 4 or r["calls"] <= 0 or r["median_ns"] <= 0:
            bad = True
        if r["log_cpu_ns_per_call"] <= 0 or not lagged:
            bad = True
    print(f"smoke {'FAILED' if bad else 'ok'}: {len(rs)} runs {got}")
    return 1 if bad else 0


def row(rs: list[dict[str, Any]]) -> int:
    other = rs[3]["backend"] if len(rs) > 3 else "B"
    order = "AAA" + other * 2 + "AA" + other * 2 + "AA" + other * 2 + "A"
    if "".join(r["backend"] for r in rs) != order:
        print(f"FAIL: runs {''.join(r['backend'] for r in rs)}, expected {order}")
        return 1
    aa = rs[0:2]
    pairs = [(rs[i], rs[i + 1]) for i in range(2, 14, 2)]
    pairs = [(x, y) if x["backend"] == "A" else (y, x) for x, y in pairs]
    first = rs[0]
    name = f"pattern {first['pattern']}, P = {first['producers']}"
    if first["formatted"]:
        name += ", formatted"
    print(f"row {name}")
    print(
        f"{'metric':<22}{'A median':>12}{other + ' median':>12}{'r = ' + other + '/A':>10}{'A/A w':>9}  ratios"
    )
    res: dict[str, tuple[float, float, float, float]] = {}
    for m in METRICS:
        a = statistics.median(p[0].get(m, 0.0) for p in pairs)
        b = statistics.median(p[1].get(m, 0.0) for p in pairs)
        rv = [ratio(p[1].get(m, 0.0), p[0].get(m, 0.0)) for p in pairs]
        r = statistics.median(rv)
        w = abs(ratio(aa[1].get(m, 0.0), aa[0].get(m, 0.0)) - 1)
        res[m] = (a, b, r, w)
        shown = " ".join(f"{x:.3f}" for x in rv)
        print(f"{m:<22}{a:>12.3f}{b:>12.3f}{r:>10.3f}{w:>9.3f}  {shown}")
    r_med, w_med = res["median_ns"][2], res["median_ns"][3]
    r_p99, w_p99 = res["p99_ns"][2], res["p99_ns"][3]
    heap_a, heap_b = res["heap_bytes_per_call"][0], res["heap_bytes_per_call"][1]
    heap_a = 0.0 if heap_a < 1 else heap_a
    heap_b = 0.0 if heap_b < 1 else heap_b
    median_ok = r_med <= 0.90 - w_med
    p99_ok = r_p99 <= 1.10 + w_p99
    heap_ok = heap_b <= 1.10 * heap_a
    if median_ok and p99_ok and heap_ok:
        verdict = "wheel win"
    elif r_med >= 1.10 + w_med:
        verdict = "wheel loss"
    else:
        verdict = "no win"
    print(
        f"verdict {name}: {verdict} (median r {r_med:.3f} vs {0.90 - w_med:.3f}: {median_ok}; "
        f"p99 r {r_p99:.3f} vs {1.10 + w_p99:.3f}: {p99_ok}; heap {other} {heap_b:.1f} vs 1.10 x A {heap_a:.1f}: {heap_ok})"
    )
    for m in ("lag_p99_us", "log_cpu_ns_per_call"):
        _, _, r, w = res[m]
        if r > 1.10 + w:
            print(
                f"note: {other}'s {m} is {r:.2f}x A's, beyond the 1.10 + {w:.3f} band"
            )
    return 0


def main(argv: list[str]) -> int:
    if len(argv) == 3 and argv[1] == "--smoke":
        return smoke(runs(argv[2]))
    if len(argv) == 2:
        return row(runs(argv[1]))
    print("usage: summarize.py [--smoke] RUNS", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
