from __future__ import annotations

import math
import statistics
from collections.abc import Iterable

EARTH_RADIUS_M = 6371008.8


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


def haversine_m(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
    phi1, phi2 = math.radians(lat1), math.radians(lat2)
    d_phi = phi2 - phi1
    d_lambda = math.radians(lon2 - lon1)
    a = (
        math.sin(d_phi / 2) ** 2
        + math.cos(phi1) * math.cos(phi2) * math.sin(d_lambda / 2) ** 2
    )
    return round(2 * EARTH_RADIUS_M * math.asin(math.sqrt(a)), 1)


def difference_in_differences(
    treated_delta: float | None, control_deltas: list[float]
) -> float | None:
    """Treated shift minus the mean shift of the untreated controls.

    Removes the component of the change that the whole area experienced, so
    weather and regional heat events do not get credited to an intervention.
    """
    if treated_delta is None or not control_deltas:
        return None
    return round(treated_delta - statistics.fmean(control_deltas), 3)

