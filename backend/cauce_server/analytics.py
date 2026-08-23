from __future__ import annotations

import statistics
from collections.abc import Iterable


def summary_stats(values: Iterable[float]) -> dict:
    data = sorted(values)
    n = len(data)

    def pct(p: float) -> float | None:
        if n == 0:
            return None
        if n == 1:
            return round(data[0], 3)
        pos = p * (n - 1)
        lo = int(pos)
        hi = min(lo + 1, n - 1)
        frac = pos - lo
        return round(data[lo] * (1 - frac) + data[hi] * frac, 3)

    return {
        "count": n,
        "min": round(data[0], 3) if n else None,
        "max": round(data[-1], 3) if n else None,
        "mean": round(statistics.fmean(data), 3) if n else None,
        "median": pct(0.5),
        "stddev": round(statistics.stdev(data), 3) if n >= 2 else None,
        "p05": pct(0.05),
        "p25": pct(0.25),
        "p75": pct(0.75),
        "p95": pct(0.95),
    }


def compare_nodes(stats_a: dict, stats_b: dict) -> float | None:
    if not stats_a["count"] or not stats_b["count"]:
        return None
    return round(stats_a["mean"] - stats_b["mean"], 3)
