"""Paired A/B of the logger's producer cost: logger_bench binaries, interleaved per round.

logger_bench (bench/lg.cc) times what a producer pays for one L_INFO call (burst, back to back,
end-to-end; one and four producers). Build every binary from the same lg.cc against the logger
commits compared. Each round runs every arm once, in an order rotated per round. Per arm and metric,
the statistic is the median over rounds of arm/before (D77), with a distribution-free 95% interval;
the same binary as both arms gives the A/A band. With several --after arms it is a bisect.

    logger_ab.py --before BIN --after BIN [--after BIN ...] --output DIR [--rounds 30]
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import statistics
import subprocess
from pathlib import Path

LINE = re.compile(r"^(L_INFO [a-z -]+, \d producers?)\s+median\s+([0-9.]+)\s+min\s+([0-9.]+)")


def run(binary: Path) -> dict[str, float]:
    """Returns each metric's median (ns per call) from one logger_bench run."""
    text = subprocess.run(
        [str(binary), "logger"], check=True, capture_output=True, text=True, timeout=300
    ).stdout
    if "0 different" not in text:
        raise RuntimeError(f"same-text check failed:\n{text}")
    rows = {m[1]: float(m[2]) for line in text.splitlines() if (m := LINE.match(line))}
    if len(rows) != 6:
        raise RuntimeError(f"expected six metrics, got {len(rows)}:\n{text}")
    return rows


def interval(values: list[float]) -> tuple[float, float]:
    """A distribution-free 95% interval for the median: order statistics from the binomial."""
    ordered = sorted(values)
    n = len(ordered)
    k = 0
    while sum(math.comb(n, i) for i in range(k + 1)) / 2**n <= 0.025:
        k += 1
    return (ordered[k - 1], ordered[n - k]) if k else (ordered[0], ordered[-1])


def main() -> None:
    parser = argparse.ArgumentParser(description=(__doc__ or "").splitlines()[0])
    parser.add_argument("--before", type=Path, required=True)
    parser.add_argument("--after", type=Path, action="append", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--rounds", type=int, default=30)
    args = parser.parse_args()
    arms = [("before", args.before.resolve())]
    if len(args.after) == 1:
        arms.append(("after", args.after[0].resolve()))
    else:
        arms += [(path.name, path.resolve()) for path in args.after]
    if len({name for name, _ in arms}) != len(arms):
        raise SystemExit("arm names (binary file names) must differ")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    rounds: list[dict[str, dict[str, float]]] = []
    for index in range(args.rounds):
        shift = index % len(arms)
        order = arms[shift:] + arms[:shift]
        rounds.append({name: run(binary) for name, binary in order})
        print(json.dumps(rounds[-1]), flush=True)
    summary: dict[str, dict[str, dict[str, float]]] = {}
    for name, _ in arms[1:]:
        summary[name] = {}
        for metric in rounds[0]["before"]:
            ratios = [r[name][metric] / r["before"][metric] for r in rounds]
            low, high = interval(ratios)
            summary[name][metric] = {
                "median_before_ns": statistics.median(r["before"][metric] for r in rounds),
                "median_after_ns": statistics.median(r[name][metric] for r in rounds),
                "median_ratio": statistics.median(ratios),
                "ratio_low": low,
                "ratio_high": high,
            }
    report = {
        "rounds": args.rounds,
        "sha256": {name: hashlib.sha256(path.read_bytes()).hexdigest() for name, path in arms},
        "summary": summary,
        "samples": rounds,
    }
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    for name, rows in summary.items():
        if len(summary) > 1:
            print(f"== {name}")
        for metric, row in rows.items():
            print(
                f"{metric:45s} {row['median_before_ns']:8.1f} -> {row['median_after_ns']:8.1f} ns"
                f"  ratio {row['median_ratio']:.3f} [{row['ratio_low']:.3f}, {row['ratio_high']:.3f}]"
            )


if __name__ == "__main__":
    main()
