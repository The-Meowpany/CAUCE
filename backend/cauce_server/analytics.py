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
    """Great-circle distance in metres.

    Longitudes are normalised into [-180, 180) before the difference is taken. Without that,
    two points either side of the antimeridian - (0, 179) and (0, -179), 222 km apart -
    produce a longitude difference of 358 degrees, so the formula returns the distance the
    long way round: 39,875 km. Nothing raises and the number is finite, which is the worst
    shape of error, because a site group split across the date line would be reported as
    thousands of kilometres from each other.

    The fix is one line and the alternative is trusting that no deployment ever has a site
    on both sides of it.
    """
    # Modulo rather than add-and-subtract, so an input already outside the range (some
    # callers pass 181 or -200 straight from a CSV) is handled as well as a value that only
    # appears out of range after subtraction.
    lon1 = (lon1 + 180.0) % 360.0 - 180.0
    lon2 = (lon2 + 180.0) % 360.0 - 180.0
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

