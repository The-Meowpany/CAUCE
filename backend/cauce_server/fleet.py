from __future__ import annotations

import hashlib
import hmac
import json
import time

from fastapi import APIRouter, Header, HTTPException, Request

from .config import settings
from .db import query, transaction
from .ratelimit import check_rate
from .security import require_admin_write, require_bearer_token

router = APIRouter(prefix="/v1")

DIAG_BUNDLES_KEPT_PER_NODE = 5
STALE_AFTER_MS = 15 * 60_000
OLD_FIRMWARE_AFTER = 3
STORAGE_WARN_PCT = 85.0
FAILURE_WARN_COUNT = 1
BUNDLE_MAX_BYTES = 16384
CLOCK_SKEW_WARN_MS = 5 * 60_000


def _int_or_none(value) -> int | None:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    return int(value)


def _float_or_none(value) -> float | None:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    return float(value)


@router.post("/nodes/{node_id}/diagnostics")
async def ingest_diagnostics(
    node_id: str,
    request: Request,
    authorization: str | None = Header(default=None),
    x_cauce_node: str | None = Header(default=None),
    x_cauce_signature: str | None = Header(default=None),
) -> dict:
    check_rate(request)
    raw_body = await request.body()
    if len(raw_body) > BUNDLE_MAX_BYTES:
        raise HTTPException(status_code=413, detail="bundle_too_large")
    try:
        bundle = json.loads(raw_body.decode("utf-8"))
    except Exception as exc:
        raise HTTPException(status_code=400, detail="invalid_json") from exc
    if not isinstance(bundle, dict):
        raise HTTPException(status_code=422, detail="invalid_bundle")

    rows = query("SELECT device_key FROM nodes WHERE node_id=?", (node_id,))
    device_key = rows[0]["device_key"] if rows else None
    if device_key:
        expected = hmac.new(device_key.encode(), raw_body, hashlib.sha256).hexdigest()
        provided = x_cauce_signature.strip().lower() if x_cauce_signature else ""
        header_ok = bool(x_cauce_node) and x_cauce_node == node_id
        if not header_ok or not provided or not hmac.compare_digest(
            provided, expected
        ):
            raise HTTPException(status_code=401, detail="invalid_signature")
    else:
        require_admin_write(authorization)

    health = bundle.get("health")
    if not isinstance(health, dict):
        raise HTTPException(status_code=422, detail="missing_health")

    now_ms = int(time.time() * 1000)
    with transaction() as conn:
        conn.execute(
            """INSERT INTO node_diagnostics
               (node_id, received_at_utc_ms, firmware_version, uptime_ms,
                node_state, net_state, rssi_dbm, clock_valid, battery_v,
                storage_bytes, stored_count, invalid_count, read_failures,
                storage_failures, corrupted_frames, last_success_utc_ms,
                bundle_json)
               VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)""",
            (
                node_id,
                now_ms,
                health.get("firmware"),
                _int_or_none(health.get("uptime_ms")),
                health.get("node_state"),
                health.get("net_state"),
                _int_or_none(health.get("rssi_dbm")),
                1 if health.get("utc_time_valid") else 0,
                _float_or_none(health.get("battery_v")),
                _int_or_none(health.get("storage_bytes")),
                _int_or_none(health.get("stored")),
                _int_or_none(health.get("invalid")),
                _int_or_none(health.get("read_failures")),
                _int_or_none(health.get("storage_failures")),
                _int_or_none(health.get("corrupted_frames")),
                _int_or_none(health.get("last_success_utc_ms")),
                raw_body.decode("utf-8"),
            ),
        )
        conn.execute(
            """DELETE FROM node_diagnostics WHERE node_id=? AND diag_id NOT IN
               (SELECT diag_id FROM node_diagnostics
                WHERE node_id=? ORDER BY diag_id DESC LIMIT ?)""",
            (node_id, node_id, DIAG_BUNDLES_KEPT_PER_NODE),
        )
    return {"status": "stored", "node_id": node_id,
            "received_at_utc_ms": now_ms}


@router.get("/nodes/{node_id}/diagnostics")
def latest_diagnostics(
    node_id: str,
    request: Request,
    authorization: str | None = Header(default=None),
) -> dict:
    check_rate(request)
    require_bearer_token(authorization, settings.api_token)
    rows = query(
        """SELECT diag_id, received_at_utc_ms, firmware_version, uptime_ms,
                  node_state, net_state, rssi_dbm, clock_valid, battery_v,
                  storage_bytes, stored_count, invalid_count, read_failures,
                  storage_failures, corrupted_frames, last_success_utc_ms
           FROM node_diagnostics WHERE node_id=? ORDER BY diag_id DESC LIMIT 1""",
        (node_id,),
    )
    if not rows:
        raise HTTPException(status_code=404, detail="no_diagnostics")
    return {"node_id": node_id, "latest": dict(rows[0])}


def _flags(
    last_seen: int | None,
    last_measurement: int | None,
    diagnostic: dict | None,
    storage_capacity_bytes: int | None,
    now_ms: int,
) -> list[str]:
    flags: list[str] = []
    if last_seen is None:
        flags.append("never_synced")
    else:
        age = now_ms - int(last_seen)
        if age > STALE_AFTER_MS:
            flags.append("stale")
        if age > 24 * 3600_000:
            flags.append("offline_24h")
    if last_measurement is not None and now_ms - int(last_measurement) > 3600_000:
        flags.append("no_measurements_1h")
    if diagnostic:
        if not diagnostic["clock_valid"]:
            flags.append("clock_unset")
        if diagnostic["corrupted_frames"]:
            flags.append("corrupted_frames")
        if diagnostic["storage_failures"]:
            flags.append("storage_failures")
        if diagnostic["read_failures"]:
            flags.append("read_failures")
        stored = diagnostic["stored_count"]
        if storage_capacity_bytes and stored is not None:
            pct = 100.0 * stored / storage_capacity_bytes
            if pct >= STORAGE_WARN_PCT:
                flags.append("storage_nearly_full")
        if diagnostic["battery_v"] and diagnostic["battery_v"] < 3.4:
            flags.append("battery_low")
    elif last_seen is not None:
        flags.append("no_diagnostics")
    return flags


@router.get("/fleet")
def fleet(
    request: Request,
    storage_capacity_bytes: int | None = None,
    stale_after_min: int | None = None,
    authorization: str | None = Header(default=None),
) -> dict:
    check_rate(request)
    require_bearer_token(authorization, settings.api_token)
    return fleet_snapshot(storage_capacity_bytes)


def fleet_snapshot(storage_capacity_bytes: int | None = None) -> dict:
    now_ms = int(time.time() * 1000)
    rows = query(
        """SELECT n.node_id, n.site_id, n.firmware_version,
                  n.last_seen_utc_ms,
                  (SELECT MAX(timestamp_utc_ms) FROM measurements m
                   WHERE m.node_id=n.node_id) AS last_measurement_utc_ms,
                  (SELECT COUNT(*) FROM measurements m
                   WHERE m.node_id=n.node_id) AS measurement_count,
                  (SELECT d.received_at_utc_ms FROM node_diagnostics d
                   WHERE d.node_id=n.node_id ORDER BY d.diag_id DESC LIMIT 1)
                    AS last_diagnostic_utc_ms
           FROM nodes n ORDER BY n.node_id"""
    )
    nodes: list[dict] = []
    versions: dict[str, int] = {}
    for row in rows:
        diag_rows = query(
            """SELECT received_at_utc_ms, firmware_version, uptime_ms,
                      node_state, net_state, rssi_dbm, clock_valid, battery_v,
                      storage_bytes, stored_count, invalid_count, read_failures,
                      storage_failures, corrupted_frames, last_success_utc_ms
               FROM node_diagnostics WHERE node_id=?
               ORDER BY diag_id DESC LIMIT 1""",
            (row["node_id"],),
        )
        diagnostic = dict(diag_rows[0]) if diag_rows else None
        version = row["firmware_version"] or (
            diagnostic["firmware_version"] if diagnostic else None
        )
        if version:
            versions[version] = versions.get(version, 0) + 1
        entry = {
            "node_id": row["node_id"],
            "site_id": row["site_id"],
            "firmware_version": version,
            "last_seen_utc_ms": row["last_seen_utc_ms"],
            "last_measurement_utc_ms": row["last_measurement_utc_ms"],
            "measurement_count": row["measurement_count"],
            "last_diagnostic_utc_ms": row["last_diagnostic_utc_ms"],
            "diagnostic": diagnostic,
            "storage_pct": None,
        }
        if diagnostic and storage_capacity_bytes and diagnostic["stored_count"]:
            entry["storage_pct"] = round(
                100.0 * diagnostic["stored_count"] / storage_capacity_bytes, 1
            )
        entry["flags"] = _flags(
            row["last_seen_utc_ms"],
            row["last_measurement_utc_ms"],
            diagnostic,
            storage_capacity_bytes,
            now_ms,
        )
        entry["needs_visit"] = any(
            f in entry["flags"]
            for f in (
                "offline_24h",
                "clock_unset",
                "storage_nearly_full",
                "storage_failures",
                "corrupted_frames",
                "battery_low",
            )
        )
        nodes.append(entry)

    fleet_versions = sorted(versions.items(), key=lambda kv: kv[0])
    return {
        "metric_type": "derived_fleet",
        "generated_utc_ms": now_ms,
        "node_count": len(nodes),
        "visits_needed": sum(1 for n in nodes if n["needs_visit"]),
        "firmware_spread": [
            {"version": v, "node_count": c} for v, c in fleet_versions
        ],
        "firmware_uniform": len(fleet_versions) <= 1,
        "note": (
            "flags come from sync timestamps and the last field diagnostics "
            "bundle; absence of a bundle is itself a flag"
        ),
        "nodes": nodes,
    }
