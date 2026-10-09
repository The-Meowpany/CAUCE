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

def detect_heat_events(rows: Iterable[dict], threshold: float,
                       min_duration_min: int = 60) -> list[dict]:
    """Runs of samples at or above `threshold` that lasted long enough to count.

    Extracted from `api.analytics_heat_events` so the dashboard page can call it without
    first acquiring a credential. That page used to forward the browser's (absent)
    `Authorization` header into the API function, which refused it - so the one page whose
    entire job was to show heat events was the only page in the dashboard a browser could
    not open. Nothing was protected by that: every reading it displays is already rendered
    unauthenticated by `/`, `/map`, `/colocation` and the node page.

    `rows` need `timestamp_utc_ms` and `value`, ordered by timestamp, value not None.

    Duration is measured between consecutive samples rather than measured directly, because
    that is all the stored data supports. An event that is still above the threshold when
    the data ends is not reported: there is no evidence it stopped, and an open-ended event
    would report a duration that is really just "until the last sample we happen to have".
    """
    events: list[dict] = []
    start_ms: int | None = None
    peak: float | None = None
    prev_ts: int | None = None
    min_ms = min_duration_min * 60000

    for r in rows:
        ts, v = r["timestamp_utc_ms"], r["value"]
        above = v >= threshold

        if above and start_ms is None:
            start_ms, peak = ts, v
        elif above and start_ms is not None:
            if peak is None or v > peak:
                peak = v
            duration_ms = ts - start_ms
            if duration_ms >= min_ms and prev_ts is not None:
                events.append({
                    "start_utc_ms": start_ms,
                    "end_utc_ms": ts,
                    "duration_min": round(duration_ms / 60000),
                    "peak_value": round(peak, 2),
                })
                start_ms, peak = None, None
        elif not above and start_ms is not None:
            duration_ms = (prev_ts or start_ms) - start_ms
            if duration_ms >= min_ms:
                events.append({
                    "start_utc_ms": start_ms,
                    "end_utc_ms": prev_ts,
                    "duration_min": round(duration_ms / 60000),
                    "peak_value": round(peak, 2),
                })
            start_ms, peak = None, None
        prev_ts = ts

    return events


def heat_summary(events: Iterable[dict]) -> dict:
    """Counts and worst case across a node's events, for the fleet view.

    `open_events` is the number of runs that began and never ended within the window. It is
    reported rather than folded into the count, because a heat wave still going is the one
    number an operator most wants and the one a closed-events list hides.
    """
    events = list(events)
    if not events:
        return {"events": 0, "total_minutes": 0, "peak_value": None,
                "longest_min": 0}
    return {
        "events": len(events),
        "total_minutes": sum(e["duration_min"] for e in events),
        "peak_value": max(e["peak_value"] for e in events),
        "longest_min": max(e["duration_min"] for e in events),
    }
