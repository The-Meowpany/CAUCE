from __future__ import annotations

import time

from fastapi import APIRouter, Header, HTTPException, Request

from .config import settings
from .db import query
from .ratelimit import check_rate
from .security import require_bearer_token

router = APIRouter(prefix="/v1")

USABLE_QUALITIES = ("VALID", "CALIBRATED", "SUSPECT", "UNCALIBRATED")
DEFAULT_INTERVAL_MS = 60_000
MIN_INTERVAL_MS = 1_000
# Long enough for any real archive, short enough that a forgotten
# to_utc_ms cannot ask the database for the whole table.
MAX_WINDOW_MS = 730 * 86400_000
# Resolution guard: see _window.
MAX_BUCKETS = 100_000
MAX_GAPS_REPORTED = 20

REASON_NO_DATA = "no_data"
REASON_NOT_DELIVERED = "measured_not_delivered"
REASON_CLOCK_UNCERTAIN = "clock_uncertain"


def expected_samples(from_ms: int, to_ms: int, interval_ms: int) -> int:
    if to_ms <= from_ms:
        return 0
    return (to_ms - from_ms) // interval_ms + 1


def _placeholders(values: tuple[str, ...]) -> str:
    return ",".join("?" for _ in values)


def classify_gap(node_id: str, start_ms: int, end_ms: int) -> str:
    uncertain = query(
        """SELECT COUNT(*) AS c FROM measurements
            WHERE node_id=? AND time_uncertain=1
              AND timestamp_utc_ms>=? AND timestamp_utc_ms<=?""",
        (node_id, start_ms, end_ms),
    )[0]["c"]
    if uncertain:
        return REASON_CLOCK_UNCERTAIN
    delivered = query(
        """SELECT COUNT(*) AS c FROM sync_batches
           WHERE node_id=? AND received_at_utc_ms>=? AND received_at_utc_ms<=?""",
        (node_id, start_ms, end_ms),
    )[0]["c"]
    if delivered:
        return REASON_NOT_DELIVERED
    return REASON_NO_DATA


def node_coverage(
    node_id: str,
    variable: str,
    from_ms: int,
    to_ms: int,
    interval_ms: int,
) -> dict:
    """Expected-versus-received accounting for one node and variable.

    `interval_ms` is the configured sample period, which the central does not
    own, so the expected count is an assumption derived from the query.
    """
    interval_ms = max(MIN_INTERVAL_MS, min(interval_ms, 24 * 3600_000))
    total = query(
        """SELECT COUNT(*) AS received,
                   MIN(timestamp_utc_ms) AS first_ts,
                   MAX(timestamp_utc_ms) AS last_ts,
                   SUM(CASE WHEN time_uncertain=1 THEN 1 ELSE 0 END) AS uncertain,
                   SUM(CASE WHEN ts_reconstructed=1 THEN 1 ELSE 0 END) AS reconstructed
            FROM measurements
            WHERE node_id=? AND variable=? AND timestamp_utc_ms>=? AND timestamp_utc_ms<=?""",
        (node_id, variable, from_ms, to_ms),
    )[0]
    usable = query(
        f"""SELECT COUNT(*) AS c FROM measurements
            WHERE node_id=? AND variable=? AND timestamp_utc_ms>=? AND timestamp_utc_ms<=?
              AND quality IN ({_placeholders(USABLE_QUALITIES)})
              AND value IS NOT NULL""",
        (node_id, variable, from_ms, to_ms, *USABLE_QUALITIES),
    )[0]["c"]

    expected = expected_samples(from_ms, to_ms, interval_ms)
    received = total["received"] or 0
    coverage_pct = round(100.0 * received / expected, 2) if expected else None
    usable_pct = round(100.0 * usable / expected, 2) if expected else None

    gap_threshold = max(interval_ms * 2, interval_ms + MIN_INTERVAL_MS)
    gaps: list[dict] = []
    # Declared here, not inside the branch below: with no data at all the branch that
    # counts the SQL-side gaps never runs, and the synthetic gap it appends is the
    # only one there is.
    gaps_total = 0
    first_ts = total["first_ts"]
    last_ts = total["last_ts"]
    if first_ts is None or last_ts is None:
        if expected:
            gaps.append(
                {
                    "start_utc_ms": from_ms,
                    "end_utc_ms": to_ms,
                    "duration_ms": to_ms - from_ms,
                    "missing_samples": expected,
                    "reason": REASON_NO_DATA,
                }
            )
    else:
        if first_ts - from_ms >= gap_threshold:
            gaps.append(
                {
                    "start_utc_ms": from_ms,
                    "end_utc_ms": first_ts,
                    "duration_ms": first_ts - from_ms,
                    "missing_samples": max(
                        0, (first_ts - from_ms) // interval_ms
                    ),
                    "reason": classify_gap(node_id, from_ms, first_ts),
                }
            )
        rows = query(
            """WITH ordered AS (
                   SELECT timestamp_utc_ms AS ts,
                          timestamp_utc_ms - LAG(timestamp_utc_ms)
                            OVER (ORDER BY timestamp_utc_ms) AS delta
                   FROM measurements
                   WHERE node_id=? AND variable=?
                     AND timestamp_utc_ms>=? AND timestamp_utc_ms<=?)
               SELECT ts, delta, COUNT(*) OVER () AS total_gaps
               FROM ordered
               WHERE delta IS NOT NULL AND delta>?
               ORDER BY delta DESC LIMIT ?""",
            (node_id, variable, from_ms, to_ms, gap_threshold, MAX_GAPS_REPORTED),
        )
        # How many gaps exist, not just how many are returned.
        #
        # `gaps_truncated` alone says "there were more" without saying how much
        # more, so a caller cannot tell a site with 21 gaps from one with 21,000.
        # The window function is free here because LIMIT is applied after it.
        gaps_total = rows[0]["total_gaps"] if rows else 0
        for row in rows:
            gap_start = row["ts"] - row["delta"]
            gap_end = row["ts"]
            gaps.append(
                {
                    "start_utc_ms": gap_start,
                    "end_utc_ms": gap_end,
                    "duration_ms": row["delta"],
                    "missing_samples": max(0, row["delta"] // interval_ms - 1),
                    "reason": classify_gap(node_id, gap_start, gap_end),
                }
            )
        if to_ms - last_ts >= gap_threshold:
            gaps.append(
                {
                    "start_utc_ms": last_ts,
                    "end_utc_ms": to_ms,
                    "duration_ms": to_ms - last_ts,
                    "missing_samples": max(0, (to_ms - last_ts) // interval_ms),
                    "reason": classify_gap(node_id, last_ts, to_ms),
                }
            )
        # Outside the branch above on purpose: with no data the only gap is the
    # synthetic whole-window one, and it still has to be counted and still has to be
    # subject to the cap.
    gaps.sort(key=lambda g: g["duration_ms"], reverse=True)
    # Erring toward reporting more gaps than were returned rather than fewer.
    gaps_total = max(gaps_total, len(gaps))
    gaps = gaps[:MAX_GAPS_REPORTED]

    longest_gap_ms = gaps[0]["duration_ms"] if gaps else 0
    return {
        "node_id": node_id,
        "variable": variable,
        "from_utc_ms": from_ms,
        "to_utc_ms": to_ms,
        "expected_interval_ms": interval_ms,
        "expected_samples": expected,
        "received_samples": received,
        "usable_samples": usable,
        "coverage_pct": coverage_pct,
        "usable_pct": usable_pct,
        "valid_share_pct": round(100.0 * usable / received, 2) if received else None,
        "first_utc_ms": first_ts,
        "last_utc_ms": last_ts,
        "uncertain_samples": total["uncertain"] or 0,
        "reconstructed_samples": total["reconstructed"] or 0,
        "gap_count_total": gaps_total,
        "gap_count_reported": len(gaps),
        "longest_gap_ms": longest_gap_ms,
        "gaps_truncated": gaps_total > len(gaps),
        "gaps": gaps,
    }


def _window(
    from_utc_ms: int | None,
    to_utc_ms: int | None,
    interval_ms: int,
) -> tuple[int, int]:
    """Validates the requested window against both a span and a resolution cap.

    Two limits, because they guard against different things. The span cap is an
    absolute sanity bound. The bucket cap is the one that actually protects the
    derived figure: `expected_samples` divides the window by the caller's
    interval, so a two-year window at a one-second period asks for 63 million
    "expected" samples and reports a coverage percentage built on them. The
    arithmetic is correct and the number is meaningless, which is worse than a
    refusal because it looks quotable.

    The bucket cap is what lets a genuinely long window through. Two years of
    hourly data is 17,520 buckets and cheap; two years of per-second data is
    not, and neither is 400 days of per-minute data. Anyone who needs a long
    window at a fine period should be reading `agg_hourly` or `agg_daily`, which
    exist for exactly that, and the error says so.
    """
    to_ms = to_utc_ms if to_utc_ms is not None else int(time.time() * 1000)
    from_ms = from_utc_ms if from_utc_ms is not None else to_ms - 24 * 3600_000
    if to_ms <= from_ms:
        raise HTTPException(status_code=422, detail="invalid_window")
    if to_ms - from_ms > MAX_WINDOW_MS:
        raise HTTPException(status_code=422, detail="window_too_wide")
    buckets = (to_ms - from_ms) // interval_ms + 1
    if buckets > MAX_BUCKETS:
        raise HTTPException(
            status_code=422,
            detail=(
                "too_many_buckets: this window at this expected_interval_ms "
                f"implies {buckets} samples, above the {MAX_BUCKETS} limit; "
                "widen expected_interval_ms or read agg_hourly / agg_daily"
            ),
        )
    return from_ms, to_ms


def _interval(interval_ms: int | None) -> int:
    if interval_ms is None:
        return DEFAULT_INTERVAL_MS
    if not isinstance(interval_ms, int) or interval_ms < MIN_INTERVAL_MS:
        raise HTTPException(status_code=422, detail="invalid_expected_interval_ms")
    return interval_ms


@router.get("/nodes/{node_id}/coverage")
def node_coverage_endpoint(
    node_id: str,
    request: Request,
    variable: str = "air_temperature",
    from_utc_ms: int | None = None,
    to_utc_ms: int | None = None,
    expected_interval_ms: int | None = None,
    authorization: str | None = Header(default=None),
) -> dict:
    check_rate(request)
    require_bearer_token(authorization, settings.api_token)
    if not query("SELECT 1 FROM nodes WHERE node_id=?", (node_id,)):
        raise HTTPException(status_code=404, detail="node_not_found")
    interval = _interval(expected_interval_ms)
    from_ms, to_ms = _window(from_utc_ms, to_utc_ms, interval)
    result = node_coverage(node_id, variable, from_ms, to_ms, interval)
    return {
        "metric_type": "derived_coverage",
        "note": (
            "expected count assumes a constant sample period; verify it against "
            "the node configuration before quoting coverage"
        ),
        **result,
    }


@router.get("/sites/{site_id}/coverage")
def site_coverage_endpoint(
    site_id: str,
    request: Request,
    variable: str = "air_temperature",
    from_utc_ms: int | None = None,
    to_utc_ms: int | None = None,
    expected_interval_ms: int | None = None,
    authorization: str | None = Header(default=None),
) -> dict:
    check_rate(request)
    require_bearer_token(authorization, settings.api_token)
    if not query("SELECT 1 FROM sites WHERE site_id=?", (site_id,)):
        raise HTTPException(status_code=404, detail="site_not_found")
    interval = _interval(expected_interval_ms)
    from_ms, to_ms = _window(from_utc_ms, to_utc_ms, interval)
    node_rows = query(
        "SELECT node_id FROM nodes WHERE site_id=? ORDER BY node_id", (site_id,)
    )
    nodes = [node_coverage(r["node_id"], variable, from_ms, to_ms, interval)
             for r in node_rows]
    expected = sum(n["expected_samples"] for n in nodes)
    received = sum(n["received_samples"] for n in nodes)
    site_pct = round(100.0 * received / expected, 2) if expected else None
    worst = min(nodes, key=lambda n: n["coverage_pct"] or 0.0, default=None)
    return {
        "metric_type": "derived_coverage",
        "site_id": site_id,
        "variable": variable,
        "from_utc_ms": from_ms,
        "to_utc_ms": to_ms,
        "expected_interval_ms": interval,
        "node_count": len(nodes),
        "expected_samples": expected,
        "received_samples": received,
        "coverage_pct": site_pct,
        "worst_node": worst["node_id"] if worst else None,
        "worst_node_coverage_pct": worst["coverage_pct"] if worst else None,
        "longest_gap_ms": max((n["longest_gap_ms"] for n in nodes), default=0),
        "note": (
            "site coverage pools its nodes; a single silent node is visible in "
            "worst_node, not in the pooled percentage"
        ),
        "nodes": nodes,
    }
