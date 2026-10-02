from __future__ import annotations

import base64
import hashlib
import hmac
import time

from fastapi import APIRouter, Header, HTTPException, Request

from .alerts import evaluate_heat_rules
from .analytics import summary_stats
from .calibration import (
    apply_value,
    calibration_for,
    calibration_summary,
    is_identity,
    transform_stats,
)
from .config import settings
from .db import engine, query, transaction
from .ratelimit import check_rate
from .retention import purge_older_than, read_state
from .security import require_bearer_token

router = APIRouter(prefix="/v1")

def _check_rate(request: Request) -> None:
    check_rate(request)


CURSOR_PREFIX = "v1:"


def encode_cursor(timestamp_utc_ms: int, sequence: int) -> str:
    raw = f"{timestamp_utc_ms}:{sequence}".encode()
    return CURSOR_PREFIX + base64.urlsafe_b64encode(raw).decode().rstrip("=")


def decode_cursor(cursor: str | None) -> tuple[int, int] | None:
    if not cursor:
        return None
    if not isinstance(cursor, str) or not cursor.startswith(CURSOR_PREFIX):
        raise HTTPException(status_code=422, detail="invalid_cursor")
    body = cursor[len(CURSOR_PREFIX):]
    padding = "=" * (-len(body) % 4)
    try:
        decoded = base64.urlsafe_b64decode(body + padding).decode()
        ts_text, seq_text = decoded.split(":", 1)
        ts, seq = int(ts_text), int(seq_text)
    except (ValueError, UnicodeDecodeError) as exc:
        raise HTTPException(status_code=422, detail="invalid_cursor") from exc
    if ts < 0 or seq < 0:
        raise HTTPException(status_code=422, detail="invalid_cursor")
    return ts, seq


def page_cursor(rows, limit: int) -> str | None:
    """A short page means the caller reached the end, so no cursor."""
    if not rows or len(rows) < limit:
        return None
    last = rows[-1]
    return encode_cursor(last["timestamp_utc_ms"], last["sequence"])


NODE_CURSOR_PREFIX = "n1:"


def decode_node_cursor(cursor: str | None) -> str | None:
    if not cursor:
        return None
    if not isinstance(cursor, str) or not cursor.startswith(NODE_CURSOR_PREFIX):
        raise HTTPException(status_code=422, detail="invalid_cursor")
    body = cursor[len(NODE_CURSOR_PREFIX):]
    padding = "=" * (-len(body) % 4)
    try:
        node_id = base64.urlsafe_b64decode(body + padding).decode()
    except (ValueError, UnicodeDecodeError) as exc:
        raise HTTPException(status_code=422, detail="invalid_cursor") from exc
    if not node_id:
        raise HTTPException(status_code=422, detail="invalid_cursor")
    return node_id


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


@router.post("/provision")
def provision_node(
    payload: dict,
    request: Request,
    authorization: str | None = Header(default=None),
) -> dict:
    _check_rate(request)
    require_bearer_token(authorization, settings.api_token)
    node_id = payload.get("node_id")
    device_key = payload.get("device_key")
    if not isinstance(node_id, str) or not node_id:
        raise HTTPException(status_code=422, detail="missing_node_id")
    if not isinstance(device_key, str) or len(device_key) < 16:
        raise HTTPException(status_code=422, detail="weak_device_key")
    with transaction() as conn:
        conn.execute(
            """INSERT INTO nodes(node_id, device_key, first_seen_utc_ms, last_seen_utc_ms)
               VALUES(?,?,?,?)
               ON CONFLICT(node_id) DO UPDATE SET device_key=excluded.device_key""",
            (node_id, device_key, int(time.time() * 1000), int(time.time() * 1000)),
        )
    return {"status": "provisioned", "node_id": node_id}


@router.post("/sync")
async def sync_batch(
    request: Request,
    authorization: str | None = Header(default=None),
    x_cauce_node: str | None = Header(default=None),
    x_cauce_signature: str | None = Header(default=None),
) -> dict:
    import json as _json

    _check_rate(request)
    raw_body = await request.body()
    try:
        payload = _json.loads(raw_body.decode("utf-8"))
    except Exception as exc:
        raise HTTPException(status_code=400, detail="invalid_json") from exc
    if not isinstance(payload, dict):
        raise HTTPException(status_code=422, detail="invalid_payload")

    node_id_claim = payload.get("node_id")
    device_key = None
    if isinstance(node_id_claim, str) and node_id_claim:
        rows_ = query("SELECT device_key FROM nodes WHERE node_id=?",
                      (node_id_claim,))
        if rows_ and rows_[0]["device_key"]:
            device_key = rows_[0]["device_key"]
        import logging
        logging.info(f"sync: node_id={node_id_claim}, device_key present={bool(device_key)}")

    if device_key:
        # Provisioned node: HMAC over the raw body is mandatory, and the
        # identity header must match the payload's node_id.
        expected_sig = hmac.new(device_key.encode(), raw_body,
                                hashlib.sha256).hexdigest()
        provided_sig = x_cauce_signature.strip().lower() if x_cauce_signature else ""
        header_ok = bool(x_cauce_node) and x_cauce_node == node_id_claim
        if not header_ok or not provided_sig or not hmac.compare_digest(
            provided_sig, expected_sig
        ):
            import logging
            logging.warning(f"sync: invalid signature for {node_id_claim}")
            raise HTTPException(status_code=401, detail="invalid_signature")
    elif settings.sync_require_auth:
        raise HTTPException(status_code=503, detail="sync_not_provisioned")
    else:
        require_bearer_token(authorization, settings.sync_token)

    if payload.get("protocol_version") != settings.protocol_version:
        raise HTTPException(status_code=422, detail="unsupported_protocol_version")
    node_id = payload.get("node_id")
    if not isinstance(node_id, str) or not node_id:
        raise HTTPException(status_code=422, detail="missing_node_id")
    transport = payload.get("transport", "wifi")
    if transport not in ("wifi", "lora"):
        raise HTTPException(status_code=422, detail="unsupported_transport")
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
        v = rec.get("value")
        if v is not None and isinstance(v, float) and (v != v or v in (float("inf"), float("-inf"))):
            raise HTTPException(status_code=422, detail="invalid_value_non_finite")

    now_ms = int(time.time() * 1000)
    with transaction() as conn:
        conn.execute(
            """INSERT INTO nodes(node_id, first_seen_utc_ms, last_seen_utc_ms)
               VALUES(?,?,?)
               ON CONFLICT(node_id) DO UPDATE SET last_seen_utc_ms=excluded.last_seen_utc_ms""",
            (node_id, now_ms, now_ms),
        )
        inserted_max = None
        inserted_rows: list[dict] = []
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
            if cur.rowcount == 1:
                inserted_rows.append(rec)
                if inserted_max is None or rec["sequence"] > inserted_max:
                    inserted_max = rec["sequence"]

        for rec in inserted_rows:
            if rec["value"] is None:
                continue
            hour_ts = (rec["timestamp_utc_ms"] // 3600000) * 3600000
            conn.execute(
                """INSERT INTO agg_hourly(node_id,variable,hour_ts,cnt,sum,sumsq,min_v,max_v)
                   VALUES(?,?,?,?,?,?,?,?)
                   ON CONFLICT(node_id,variable,hour_ts) DO UPDATE SET
                     cnt = cnt + excluded.cnt,
                     sum = sum + excluded.sum,
                     sumsq = sumsq + excluded.sumsq,
                     min_v = MIN(min_v, excluded.min_v),
                     max_v = MAX(max_v, excluded.max_v)""",
                (
                    node_id,
                    rec["variable"],
                    hour_ts,
                    1,
                    rec["value"],
                    (rec["value"] or 0.0) ** 2 if rec["value"] is not None else 0.0,
                    rec["value"],
                    rec["value"],
                ),
            )

        conn.execute(
            """DELETE FROM sync_batches WHERE batch_id NOT IN
               (SELECT batch_id FROM sync_batches ORDER BY batch_id DESC LIMIT 5000)"""
        )

        acked = _acknowledged_sequence(conn, node_id, measurements) or inserted_max
        if acked is not None:
            conn.execute(
                """INSERT INTO sync_batches(node_id,batch_size,first_sequence,last_sequence,received_at_utc_ms,transport)
                   VALUES(?,?,?,?,?,?)""",
                (
                    node_id,
                    len(measurements),
                    min((r["sequence"] for r in measurements), default=None),
                    max((r["sequence"] for r in measurements), default=None),
                    now_ms,
                    transport,
                ),
            )

    try:
        evaluate_heat_rules(node_id)
    except Exception:
        pass

    return {
        "acknowledged_sequence": acked if acked is not None else 0,
        "received": len(measurements),
    }


@router.get("/ota/manifest")
def ota_manifest(
    request: Request,
    node_id: str | None = None,
) -> dict:
    _check_rate(request)
    path = settings.ota_releases_path
    if not path:
        raise HTTPException(status_code=404, detail="ota_not_configured")
    try:
        with open(path, encoding="utf-8") as fh:
            import json as _json
            release = _json.load(fh)
    except (OSError, ValueError) as exc:
        raise HTTPException(status_code=503, detail="ota_manifest_unreadable") from exc
    version = release.get("version")
    sha256 = release.get("sha256")
    url = release.get("url")
    total_size = release.get("total_size")
    if (not isinstance(version, str) or not version
            or not isinstance(sha256, str) or not sha256
            or not isinstance(url, str) or not url
            or not isinstance(total_size, int) or total_size <= 0):
        raise HTTPException(status_code=503, detail="ota_manifest_invalid")
    signature = None
    if node_id:
        rows = query("SELECT device_key FROM nodes WHERE node_id=?", (node_id,))
        if rows and rows[0]["device_key"]:
            canonical = f"{version}|{url}|{total_size}"
            signature = hmac.new(rows[0]["device_key"].encode(),
                                 canonical.encode(),
                                 hashlib.sha256).hexdigest()
    return {"version": version, "sha256": sha256, "url": url,
            "total_size": total_size, "hmac": signature}


@router.get("/nodes")
def list_nodes(
    request: Request,
    limit: int = 100,
    offset: int = 0,
    cursor: str | None = None,
    authorization: str | None = Header(default=None),
) -> dict:
    _check_rate(request)
    require_bearer_token(authorization, settings.api_token)
    limit = max(1, min(limit, 1000))
    offset = max(0, offset)
    after = decode_node_cursor(cursor)
    where = ""
    params: list = []
    if after is not None:
        where = " WHERE node_id>?"
        params.append(after)
    total = query(f"SELECT COUNT(*) AS c FROM nodes{where}",
                  tuple(params))[0]["c"]
    sql = ("SELECT node_id, site_id, firmware_version,"
           " first_seen_utc_ms, last_seen_utc_ms,"
           " (SELECT COUNT(*) FROM measurements m WHERE m.node_id=n.node_id)"
           "   AS measurement_count,"
           " (SELECT MAX(timestamp_utc_ms) FROM measurements m"
           "   WHERE m.node_id=n.node_id) AS last_measurement_utc_ms"
           " FROM nodes n" + where + " ORDER BY node_id LIMIT ? OFFSET ?")
    if cursor:
        offset = 0
    params.extend([limit, offset])
    rows = query(sql, tuple(params))
    next_cursor = None
    if rows and len(rows) == limit:
        next_cursor = NODE_CURSOR_PREFIX + base64.urlsafe_b64encode(
            rows[-1]["node_id"].encode()).decode().rstrip("=")
    return {"total": total, "limit": limit, "offset": offset,
            "next_cursor": next_cursor,
            "nodes": [dict(r) for r in rows]}


@router.get("/nodes/{node_id}")
def get_node(node_id: str, request: Request, authorization: str | None = Header(default=None)) -> dict:
    _check_rate(request)
    require_bearer_token(authorization, settings.api_token)
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
    offset: int = 0,
    cursor: str | None = None,
    authorization: str | None = Header(default=None),
) -> dict:
    _check_rate(request)
    require_bearer_token(authorization, settings.api_token)
    limit = max(1, min(limit, 10000))
    offset = max(0, offset)
    after = decode_cursor(cursor)
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
    if after is not None:
        sql += " AND (timestamp_utc_ms>? OR (timestamp_utc_ms=? AND sequence>?))"
        params.extend([after[0], after[0], after[1]])
    total = query(f"SELECT COUNT(*) AS c FROM ({sql})",
                  tuple(params))[0]["c"]
    sql += " ORDER BY timestamp_utc_ms, sequence LIMIT ? OFFSET ?"
    if cursor:
        offset = 0
    params.extend([limit, offset])
    rows = query(sql, tuple(params))
    return {"node_id": node_id, "total": total, "limit": limit,
            "offset": offset,
            "next_cursor": page_cursor(rows, limit),
            "measurements": [dict(r) for r in rows]}


@router.get("/maintenance/backup")
def download_backup(
    request: Request,
    authorization: str | None = Header(default=None),
):
    import os
    import tempfile

    from fastapi.responses import FileResponse
    from starlette.background import BackgroundTask

    _check_rate(request)
    require_bearer_token(authorization, settings.api_token)
    tmp = tempfile.NamedTemporaryFile(suffix=".sqlite", delete=False)
    tmp.close()
    try:
        engine().execute(f"VACUUM INTO '{tmp.name}'")
        engine().commit()
    except Exception as exc:
        try:
            os.remove(tmp.name)
        except OSError:
            pass
        raise HTTPException(status_code=503, detail="backup_failed") from exc

    def _cleanup() -> None:
        try:
            os.remove(tmp.name)
        except OSError:
            pass

    return FileResponse(tmp.name, media_type="application/x-sqlite3",
                        filename="cauce-backup.sqlite",
                        background=BackgroundTask(_cleanup))


@router.post("/maintenance/retention")
def run_retention(
    payload: dict,
    request: Request,
    authorization: str | None = Header(default=None),
) -> dict:
    _check_rate(request)
    require_bearer_token(authorization, settings.api_token)
    days = payload.get("older_than_days", 90)
    if not isinstance(days, int) or not 1 <= days <= 3650:
        raise HTTPException(status_code=422, detail="invalid_retention_days")
    state = purge_older_than(days)
    return {
        "status": "retained",
        "older_than_days": days,
        "deleted_measurements": state.get("deleted_measurements", 0),
        "deleted_buckets": state.get("deleted_buckets", 0),
        "vacuumed": bool(state.get("vacuumed")),
    }


@router.get("/maintenance/retention")
def retention_state(
    request: Request,
    authorization: str | None = Header(default=None),
) -> dict:
    _check_rate(request)
    require_bearer_token(authorization, settings.api_token)
    state = read_state()
    return {
        "enabled": settings.retention_enabled,
        "retention_days": settings.retention_days,
        "retention_interval_h": settings.retention_interval_h,
        "vacuum_interval_h": settings.vacuum_interval_h,
        **state,
    }


@router.get("/analytics/summary")
def analytics_summary(
    node_id: str,
    request: Request,
    variable: str,
    from_utc_ms: int | None = None,
    to_utc_ms: int | None = None,
    granularity: str = "auto",
    authorization: str | None = Header(default=None),
) -> dict:
    _check_rate(request)
    require_bearer_token(authorization, settings.api_token)
    if granularity not in ("auto", "raw", "hourly"):
        raise HTTPException(status_code=422, detail="invalid_granularity")
    calibration = calibration_for(node_id, variable)

    span_ms = None
    if from_utc_ms is not None and to_utc_ms is not None:
        span_ms = to_utc_ms - from_utc_ms
    elif to_utc_ms is not None:
        span_ms = to_utc_ms - (from_utc_ms or 0)

    use_hourly = granularity == "hourly" or (
        granularity == "auto" and span_ms is not None
        and span_ms >= HOURLY_MIN_SPAN_MS
        and _hourly_coverage_ok(node_id, variable, from_utc_ms, to_utc_ms)
    )
    if use_hourly:
        raw_hourly = _hourly_stats(node_id, variable, from_utc_ms, to_utc_ms)
        calibrated_hourly = transform_stats(raw_hourly, calibration)
        return {
            "node_id": node_id,
            "variable": variable,
            "metric_type": "derived",
            "source": "materialized_hourly",
            "granularity": "hourly",
            "note": (
                "descriptive statistics only; does not imply causality. "
                "hourly buckets, not raw rows; extremes inside an hour are lost. "
                "use granularity=raw for exact min/max"
            ),
            "calibration": calibration_summary(calibration),
            **({"calibrated": calibrated_hourly} if calibration else {}),
            **raw_hourly,
        }

    values = _values_for(node_id, variable, from_utc_ms, to_utc_ms)
    stats = summary_stats(values)
    calibrated = transform_stats(stats, calibration)
    return {
        "node_id": node_id,
        "variable": variable,
        "metric_type": "derived",
        "granularity": "raw",
        "note": (
            "descriptive statistics only; does not imply causality. "
            + ("raw rows are untouched; calibrated values are derived"
               if calibration else "")
        ),
        "calibration": calibration_summary(calibration),
        **({"calibrated": calibrated} if calibration else {}),
        **stats,
    }


HOUR_MS = 3600000
# Below a week the raw rows are cheap to read and give exact extremes, so
# `auto` stays on raw. Above it the hourly buckets are the cheaper answer and
# the response says which one it used.
HOURLY_MIN_SPAN_MS = 7 * 24 * HOUR_MS


def _hourly_stats(node_id: str, variable: str, from_utc_ms: int | None,
                  to_utc_ms: int | None) -> dict:
    sql = ("SELECT hour_ts, cnt, sum, sumsq, min_v, max_v FROM agg_hourly"
           " WHERE node_id=? AND variable=?")
    params: list = [node_id, variable]
    if from_utc_ms is not None:
        sql += " AND hour_ts>=?"
        params.append((from_utc_ms // HOUR_MS) * HOUR_MS)
    if to_utc_ms is not None:
        sql += " AND hour_ts<=?"
        params.append((to_utc_ms // HOUR_MS) * HOUR_MS)
    sql += " ORDER BY hour_ts"
    return _agg_stats(query(sql, tuple(params)))


def _hourly_coverage_ok(node_id: str, variable: str, from_utc_ms: int | None,
                        to_utc_ms: int | None) -> bool:
    """Only trust the hourly view when the buckets are actually populated."""
    rows = query(
        "SELECT COUNT(*) AS buckets, COALESCE(SUM(cnt),0) AS samples"
        " FROM agg_hourly WHERE node_id=? AND variable=?",
        (node_id, variable),
    )[0]
    if rows["buckets"] == 0:
        return False
    raw = query(
        "SELECT COUNT(*) AS c FROM measurements WHERE node_id=? AND variable=?",
        (node_id, variable),
    )[0]["c"]
    return raw == 0 or rows["samples"] >= raw * 0.9


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
    require_bearer_token(authorization, settings.api_token)
    raw_a = summary_stats(_values_for(node_a, variable, from_utc_ms, to_utc_ms))
    raw_b = summary_stats(_values_for(node_b, variable, from_utc_ms, to_utc_ms))
    cal_a = calibration_for(node_a, variable)
    cal_b = calibration_for(node_b, variable)
    a = transform_stats(raw_a, cal_a)
    b = transform_stats(raw_b, cal_b)
    mean_diff = None
    if a["count"] and b["count"]:
        mean_diff = round(a["mean"] - b["mean"], 3)
    raw_diff = None
    if raw_a["count"] and raw_b["count"]:
        raw_diff = round(raw_a["mean"] - raw_b["mean"], 3)
    return {
        "variable": variable,
        "metric_type": "derived_comparison",
        "note": ("differences may reflect placement or calibration; not causality. "
                 "means are calibrated when the site has a calibration record"),
        "node_a": {"node_id": node_a, "calibration": calibration_summary(cal_a),
                   **a},
        "node_b": {"node_id": node_b, "calibration": calibration_summary(cal_b),
                   **b},
        "mean_difference": mean_diff,
        "mean_difference_raw": raw_diff,
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
    require_bearer_token(authorization, settings.api_token)
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
    calibration = calibration_for(node_id, variable)
    if calibration and not is_identity(calibration["scale"],
                                      calibration["offset"]):
        rows = [{"timestamp_utc_ms": r["timestamp_utc_ms"],
                 "value": apply_value(r["value"], calibration)} for r in rows]

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
        "note": "duration estimated between consecutive above-threshold samples; not causality",
        "calibration": calibration_summary(calibration),
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
    require_bearer_token(authorization, settings.api_token)
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
    calibration = calibration_for(node_id, variable)
    cal_a = transform_stats(period_a, calibration)
    cal_b = transform_stats(period_b, calibration)
    cal_shift = None
    if cal_a["count"] and cal_b["count"]:
        cal_shift = round(cal_b["mean"] - cal_a["mean"], 3)
    return {
        "metric_type": "derived_period_comparison",
        "note": (
            "same-node comparison across two windows; does not imply causality. "
            + ("" if sufficient else "INSUFFICIENT SAMPLES (<30 per window)")
        ),
        "sufficient_sample": sufficient,
        "node_id": node_id,
        "variable": variable,
        "calibration": calibration_summary(calibration),
        "period_a": {"start_utc_ms": a_start, "end_utc_ms": a_end, **period_a},
        "period_b": {"start_utc_ms": b_start, "end_utc_ms": b_end, **period_b},
        "mean_shift": mean_shift,
        "mean_shift_calibrated": cal_shift,
    }


def _agg_stats(rows) -> dict:
    import math
    cnt = sum(r["cnt"] for r in rows)
    if cnt == 0:
        return {"count": 0}
    total_sum = sum(r["sum"] for r in rows)
    total_sumsq = sum(r["sumsq"] for r in rows)
    mean = total_sum / cnt
    variance = max(0.0, total_sumsq / cnt - mean * mean)
    mins = [r["min_v"] for r in rows if r["min_v"] is not None]
    maxs = [r["max_v"] for r in rows if r["max_v"] is not None]
    return {
        "count": cnt,
        "min": round(min(mins), 3) if mins else None,
        "max": round(max(maxs), 3) if maxs else None,
        "mean": round(mean, 3),
        "stddev_pop": round(math.sqrt(variance), 3),
        "buckets": len(rows),
        "granularity": "hourly",
    }


@router.get("/analytics/summary-fast")
def analytics_summary_fast(
    node_id: str,
    request: Request,
    variable: str,
    from_utc_ms: int | None = None,
    to_utc_ms: int | None = None,
    authorization: str | None = Header(default=None),
) -> dict:
    """Reads precomputed hourly aggregates; O(buckets) instead of O(records)."""
    _check_rate(request)
    require_bearer_token(authorization, settings.api_token)
    sql = """SELECT hour_ts, cnt, sum, sumsq, min_v, max_v FROM agg_hourly
             WHERE node_id=? AND variable=?"""
    params: list = [node_id, variable]
    if from_utc_ms is not None:
        first_hour = (from_utc_ms // 3600000) * 3600000
        sql += " AND hour_ts>=?"
        params.append(first_hour)
    if to_utc_ms is not None:
        last_hour = (to_utc_ms // 3600000) * 3600000
        sql += " AND hour_ts<=?"
        params.append(last_hour)
    sql += " ORDER BY hour_ts"
    rows = query(sql, tuple(params))
    calibration = calibration_for(node_id, variable)
    raw_stats = _agg_stats(rows)
    calibrated = transform_stats(raw_stats, calibration)
    return {
        "node_id": node_id,
        "variable": variable,
        "metric_type": "derived",
        "source": "materialized_hourly",
        "note": ("descriptive statistics only; does not imply causality"
                 if raw_stats.get("count") else "no data in range"),
        "calibration": calibration_summary(calibration),
        **({"calibrated": calibrated} if calibration else {}),
        **raw_stats,
    }


@router.post("/nodes/{node_id}/time-reconstruct")
def time_reconstruct(
    node_id: str,
    request: Request,
    authorization: str | None = Header(default=None),
) -> dict:
    """Backfills timestamps of leading time_uncertain records using the first
    anchored sample and the median interval between consecutive anchored
    samples. Marks reconstructed rows and never touches anchored ones."""
    _check_rate(request)
    require_bearer_token(authorization, settings.api_token)

    with transaction() as conn:
        rows = conn.execute(
            """SELECT sequence, timestamp_utc_ms FROM measurements
               WHERE node_id=? ORDER BY sequence""",
            (node_id,),
        ).fetchall()
        if not rows:
            raise HTTPException(status_code=404, detail="node_not_found")

        anchored = [(r["sequence"], r["timestamp_utc_ms"]) for r in rows
                    if r["timestamp_utc_ms"]]
        if len(anchored) < 2:
            raise HTTPException(status_code=422, detail="no_time_anchor")

        intervals = [b[1] - a[1] for a, b in zip(anchored, anchored[1:], strict=False)
                     if b[1] > a[1]]
        intervals.sort()
        step = intervals[len(intervals) // 2]
        if step <= 0:
            raise HTTPException(status_code=422, detail="invalid_intervals")

        anchor_seq, anchor_ts = anchored[0]
        fixed = 0

        # walk backwards from the first anchor over preceding uncertain rows
        pending = [(s, ts) for s, ts in anchored]
        first_seq, first_ts = pending[0]
        cur = conn.execute(
            """SELECT sequence FROM measurements
               WHERE node_id=? AND sequence<? AND timestamp_utc_ms=0
               ORDER BY sequence DESC""",
            (node_id, first_seq),
        ).fetchall()
        already = conn.execute(
            "SELECT COUNT(*) AS c FROM measurements WHERE node_id=? AND ts_reconstructed=1",
            (node_id,),
        ).fetchone()["c"]
        if already > 0:
            raise HTTPException(status_code=409,
                                detail="already_reconstructed_run_once_only")
        uncertain_seqs = [r["sequence"] for r in cur]
        for seq in uncertain_seqs:
            distance = anchor_seq - seq
            if distance <= 0:
                continue
            new_ts = first_ts - distance * step
            if new_ts <= 0:
                continue
            conn.execute(
                """UPDATE measurements SET timestamp_utc_ms=?, ts_reconstructed=1
                   WHERE node_id=? AND sequence=? AND timestamp_utc_ms=0""",
                (new_ts, node_id, seq),
            )
            fixed += 1

    return {
        "status": "reconstructed",
        "node_id": node_id,
        "records_fixed": fixed,
        "anchor_sequence": anchor_seq,
        "assumed_interval_ms": step,
        "note": ("timestamps inferred backwards from first anchor using median "
                 "interval; margin of error grows with distance from anchor"),
    }

