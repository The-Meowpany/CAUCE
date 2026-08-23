from __future__ import annotations

import time
from typing import Iterator

from fastapi import APIRouter, Header, HTTPException, Request, Response

from .analytics import compare_nodes, summary_stats
from .config import settings
from .db import query, transaction

router = APIRouter(prefix="/v1")

_RATE: dict[str, list[float]] = {}
_RATE_LOCK = None


def _check_rate(request: Request) -> None:
    key = request.client.host if request.client else "unknown"
    now = time.monotonic()
    window = [t for t in _RATE.get(key, []) if now - t < 60.0]
    if len(window) >= settings.rate_limit_per_minute:
        raise HTTPException(status_code=429, detail="rate_limited")
    window.append(now)
    _RATE[key] = window


def _require_sync_auth(authorization: str | None) -> None:
    expected = settings.sync_token
    if not expected:
        return
    if authorization != f"Bearer {expected}":
        raise HTTPException(status_code=401, detail="unauthorized")


def _require_api_auth(authorization: str | None) -> None:
    expected = settings.api_token
    if not expected:
        return
    if authorization != f"Bearer {expected}":
        raise HTTPException(status_code=401, detail="unauthorized")


def _acknowledged_sequence(conn, node_id: str, records: list[dict]) -> int | None:
    highest = None
    for rec in records:
        seq = rec["sequence"]
        row = conn.execute(
            "SELECT 1 FROM measurements WHERE node_id=? AND sequence=?",
            (node_id, seq),
        ).fetchone()
        if row is not None and (highest is None or seq > highest):
            highest = seq
    return highest


@router.post("/sync")
def sync_batch(
    payload: dict,
    request: Request,
    authorization: str | None = Header(default=None),
) -> dict:
    _check_rate(request)
    _require_sync_auth(authorization)

    if payload.get("protocol_version") != settings.protocol_version:
        raise HTTPException(status_code=422, detail="unsupported_protocol_version")
    node_id = payload.get("node_id")
    if not isinstance(node_id, str) or not node_id:
        raise HTTPException(status_code=422, detail="missing_node_id")
    measurements = payload.get("measurements")
    if not isinstance(measurements, list):
        raise HTTPException(status_code=422, detail="missing_measurements")

    required_fields = {"sequence", "timestamp_utc_ms", "variable", "quality"}
    for rec in measurements:
        missing = required_fields - rec.keys()
        if missing or not isinstance(rec.get("sequence"), int):
            raise HTTPException(
                status_code=422,
                detail=f"invalid_record_missing_{sorted(missing)[0]}",
            )

    now_ms = int(time.time() * 1000)
    with transaction() as conn:
        conn.execute(
            """INSERT INTO nodes(node_id, first_seen_utc_ms, last_seen_utc_ms)
               VALUES(?,?,?)
               ON CONFLICT(node_id) DO UPDATE SET last_seen_utc_ms=excluded.last_seen_utc_ms""",
            (node_id, now_ms, now_ms),
        )
        inserted_max = None
        for rec in measurements:
            cur = conn.execute(
                """INSERT OR IGNORE INTO measurements
                   (node_id, sequence, sensor_id, timestamp_utc_ms, variable,
                    value, unit, quality, reason_bits, time_uncertain)
                   VALUES(?,?,?,?,?,?,?,?,?,?)""",
                (
                    node_id,
                    rec["sequence"],
                    rec.get("sensor_id"),
                    rec["timestamp_utc_ms"],
                    rec["variable"],
                    rec.get("value"),
                    rec.get("unit"),
                    rec["quality"],
                    int(rec.get("reason_bits") or 0),
                    1 if rec.get("time_uncertain") else 0,
                ),
            )
            if cur.rowcount == 1 and (
                inserted_max is None or rec["sequence"] > inserted_max
            ):
                inserted_max = rec["sequence"]

        acked = _acknowledged_sequence(conn, node_id, measurements) or inserted_max
        if acked is not None:
            conn.execute(
                """INSERT INTO sync_batches(node_id,batch_size,first_sequence,last_sequence,received_at_utc_ms)
                   VALUES(?,?,?,?,?)""",
                (
                    node_id,
                    len(measurements),
                    min((r["sequence"] for r in measurements), default=None),
                    max((r["sequence"] for r in measurements), default=None),
                    now_ms,
                ),
            )

    return {
        "acknowledged_sequence": acked if acked is not None else 0,
        "received": len(measurements),
    }


@router.get("/nodes")
def list_nodes(request: Request, authorization: str | None = Header(default=None)) -> dict:
    _check_rate(request)
    _require_api_auth(authorization)
    rows = query(
        """SELECT node_id, site_id, firmware_version,
                  first_seen_utc_ms, last_seen_utc_ms,
                  (SELECT COUNT(*) FROM measurements m WHERE m.node_id=n.node_id) AS measurement_count,
                  (SELECT MAX(timestamp_utc_ms) FROM measurements m WHERE m.node_id=n.node_id) AS last_measurement_utc_ms
           FROM nodes n ORDER BY node_id"""
    )
    return {"nodes": [dict(r) for r in rows]}


@router.get("/nodes/{node_id}")
def get_node(node_id: str, request: Request, authorization: str | None = Header(default=None)) -> dict:
    _check_rate(request)
    _require_api_auth(authorization)
    rows = query("SELECT * FROM nodes WHERE node_id=?", (node_id,))
    if not rows:
        raise HTTPException(status_code=404, detail="node_not_found")
    node = dict(rows[0])
    latest = query(
        """SELECT * FROM measurements WHERE node_id=?
           ORDER BY sequence DESC LIMIT 1""",
        (node_id,),
    )
    node["latest_measurement"] = dict(latest[0]) if latest else None
    return node


@router.get("/nodes/{node_id}/measurements")
def node_measurements(
    node_id: str,
    request: Request,
    variable: str | None = None,
    quality: str | None = None,
    from_utc_ms: int | None = None,
    to_utc_ms: int | None = None,
    limit: int = 1000,
    authorization: str | None = Header(default=None),
) -> dict:
    _check_rate(request)
    _require_api_auth(authorization)
    limit = max(1, min(limit, 10000))
    sql = "SELECT * FROM measurements WHERE node_id=?"
    params: list = [node_id]
    if variable:
        sql += " AND variable=?"
        params.append(variable)
    if quality:
        sql += " AND quality=?"
        params.append(quality)
    if from_utc_ms is not None:
        sql += " AND timestamp_utc_ms>=?"
        params.append(from_utc_ms)
    if to_utc_ms is not None:
        sql += " AND timestamp_utc_ms<=?"
        params.append(to_utc_ms)
    sql += " ORDER BY timestamp_utc_ms LIMIT ?"
    params.append(limit)
    rows = query(sql, tuple(params))
    return {"node_id": node_id, "measurements": [dict(r) for r in rows]}


@router.get("/analytics/summary")
def analytics_summary(
    node_id: str,
    request: Request,
    variable: str,
    from_utc_ms: int | None = None,
    to_utc_ms: int | None = None,
    authorization: str | None = Header(default=None),
) -> dict:
    _check_rate(request)
    _require_api_auth(authorization)
    values = _values_for(node_id, variable, from_utc_ms, to_utc_ms)
    stats = summary_stats(values)
    return {
        "node_id": node_id,
        "variable": variable,
        "metric_type": "derived",
        "note": "estadistica descriptiva; no implica causalidad",
        **stats,
    }


@router.get("/analytics/compare")
def analytics_compare(
    node_a: str,
    request: Request,
    node_b: str,
    variable: str,
    from_utc_ms: int | None = None,
    to_utc_ms: int | None = None,
    authorization: str | None = Header(default=None),
) -> dict:
    _check_rate(request)
    _require_api_auth(authorization)
    a = summary_stats(_values_for(node_a, variable, from_utc_ms, to_utc_ms))
    b = summary_stats(_values_for(node_b, variable, from_utc_ms, to_utc_ms))
    mean_diff = None
    if a["count"] and b["count"]:
        mean_diff = round(a["mean"] - b["mean"], 3)
    return {
        "variable": variable,
        "metric_type": "derived_comparison",
        "note": "diferencias pueden reflejar ubicacion/calibracion; no causalidad",
        "node_a": {"node_id": node_a, **a},
        "node_b": {"node_id": node_b, **b},
        "mean_difference": mean_diff,
    }


def _values_for(
    node_id: str, variable: str, from_utc_ms: int | None, to_utc_ms: int | None
) -> list[float]:
    sql = """SELECT value FROM measurements
             WHERE node_id=? AND variable=?
               AND quality IN ('VALID','CALIBRATED','SUSPECT','UNCALIBRATED')
               AND value IS NOT NULL"""
    params: list = [node_id, variable]
    if from_utc_ms is not None:
        sql += " AND timestamp_utc_ms>=?"
        params.append(from_utc_ms)
    if to_utc_ms is not None:
        sql += " AND timestamp_utc_ms<=?"
        params.append(to_utc_ms)
    sql += " ORDER BY timestamp_utc_ms"
    return [r["value"] for r in query(sql, tuple(params))]


@router.get("/analytics/heat-events")
def analytics_heat_events(
    node_id: str,
    request: Request,
    variable: str = "air_temperature",
    threshold: float = 32.0,
    min_duration_min: int = 60,
    from_utc_ms: int | None = None,
    to_utc_ms: int | None = None,
    authorization: str | None = Header(default=None),
) -> dict:
    _check_rate(request)
    _require_api_auth(authorization)
    sql = """SELECT timestamp_utc_ms, value FROM measurements
             WHERE node_id=? AND variable=? AND value IS NOT NULL
               AND quality IN ('VALID','CALIBRATED','SUSPECT','UNCALIBRATED')"""
    params: list = [node_id, variable]
    if from_utc_ms is not None:
        sql += " AND timestamp_utc_ms>=?"
        params.append(from_utc_ms)
    if to_utc_ms is not None:
        sql += " AND timestamp_utc_ms<=?"
        params.append(to_utc_ms)
    sql += " ORDER BY timestamp_utc_ms"
    rows = query(sql, tuple(params))

    events: list[dict] = []
    start_ms: int | None = None
    peak = None
    prev_ts: int | None = None
    for r in rows:
        ts, v = r["timestamp_utc_ms"], r["value"]
        above = v >= threshold
        if above and start_ms is None:
            start_ms, peak = ts, v
        elif above and start_ms is not None:
            if v > (peak or v):
                peak = v
            duration_ms = ts - start_ms
            if duration_ms >= min_duration_min * 60000 and prev_ts is not None:
                events.append({
                    "start_utc_ms": start_ms,
                    "end_utc_ms": ts,
                    "duration_min": round(duration_ms / 60000),
                    "peak_value": round(peak, 2),
                })
                start_ms, peak = None, None
        elif not above and start_ms is not None:
            duration_ms = (prev_ts or start_ms) - start_ms
            if duration_ms >= min_duration_min * 60000:
                events.append({
                    "start_utc_ms": start_ms,
                    "end_utc_ms": prev_ts,
                    "duration_min": round(duration_ms / 60000),
                    "peak_value": round(peak, 2),
                })
            start_ms, peak = None, None
        prev_ts = ts

    return {
        "node_id": node_id,
        "variable": variable,
        "threshold": threshold,
        "min_duration_min": min_duration_min,
        "metric_type": "derived",
        "note": "duracion estimada entre muestras consecutivas sobre el umbral; no causalidad",
        "events": events,
    }


@router.get("/analytics/period-compare")
def analytics_period_compare(
    node_id: str,
    request: Request,
    variable: str,
    a_start: int,
    a_end: int,
    b_start: int,
    b_end: int,
    authorization: str | None = Header(default=None),
) -> dict:
    _check_rate(request)
    _require_api_auth(authorization)
    if min(a_start, a_end, b_start, b_end) < 0 or a_end <= a_start or b_end <= b_start:
        raise HTTPException(status_code=422, detail="invalid_windows")

    def stats_for(lo: int, hi: int) -> dict:
        return summary_stats(_values_for(node_id, variable, lo, hi))

    period_a = stats_for(a_start, a_end)
    period_b = stats_for(b_start, b_end)
    mean_shift = None
    if period_a["count"] and period_b["count"]:
        mean_shift = round(period_b["mean"] - period_a["mean"], 3)

    sufficient = period_a["count"] >= 30 and period_b["count"] >= 30
    return {
        "metric_type": "derived_period_comparison",
        "note": (
            "comparacion del mismo nodo en dos ventanas; no implica causalidad. "
            + ("" if sufficient else "MUESTRAS INSUFICIENTES (<30 por ventana)")
        ),
        "sufficient_sample": sufficient,
        "node_id": node_id,
        "variable": variable,
        "period_a": {"start_utc_ms": a_start, "end_utc_ms": a_end, **period_a},
        "period_b": {"start_utc_ms": b_start, "end_utc_ms": b_end, **period_b},
        "mean_shift": mean_shift,
    }