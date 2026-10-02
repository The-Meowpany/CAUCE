from __future__ import annotations

import time

from fastapi import APIRouter, Header, HTTPException, Request

from .analytics import (
    difference_in_differences,
    haversine_m,
    summary_stats,
)
from .calibration import (
    apply_value,
    calibration_for,
    calibration_summary,
    is_identity,
)
from .config import settings
from .db import query, transaction
from .ratelimit import check_rate
from .security import require_bearer_token

router = APIRouter(prefix="/v1")


@router.post("/sites")
def create_site(
    payload: dict,
    request: Request,
    authorization: str | None = Header(default=None),
) -> dict:
    check_rate(request)
    require_bearer_token(authorization, settings.api_token)
    site_id = payload.get("site_id")
    if not isinstance(site_id, str) or not site_id:
        raise HTTPException(status_code=422, detail="missing_site_id")
    is_control = _control_flag(payload.get("control"))
    with transaction() as conn:
        existing = conn.execute(
            "SELECT 1 FROM sites WHERE site_id=?", (site_id,)
        ).fetchone()
        if existing:
            raise HTTPException(status_code=409, detail="site_exists")
        conn.execute(
            "INSERT INTO sites(site_id, name, notes, is_control) VALUES(?,?,?,?)",
            (site_id, payload.get("name"), payload.get("notes"), is_control),
        )
    return {"status": "created", "site_id": site_id, "is_control": bool(is_control)}


def _control_flag(value) -> int:
    if value is None:
        return 0
    if isinstance(value, bool):
        return 1 if value else 0
    if isinstance(value, int) and value in (0, 1):
        return value
    raise HTTPException(status_code=422, detail="invalid_control_flag")


@router.put("/sites/{site_id}/control")
def set_site_control(
    site_id: str,
    payload: dict,
    request: Request,
    authorization: str | None = Header(default=None),
) -> dict:
    check_rate(request)
    require_bearer_token(authorization, settings.api_token)
    flag = _control_flag(payload.get("control"))
    with transaction() as conn:
        cur = conn.execute(
            "UPDATE sites SET is_control=? WHERE site_id=?", (flag, site_id)
        )
        if cur.rowcount == 0:
            raise HTTPException(status_code=404, detail="site_not_found")
    return {"status": "updated", "site_id": site_id, "is_control": bool(flag)}


@router.get("/sites")
def list_sites(
    request: Request,
    authorization: str | None = Header(default=None),
) -> dict:
    check_rate(request)
    require_bearer_token(authorization, settings.api_token)
    rows = query(
        """SELECT s.site_id, s.name, s.notes, s.lat, s.lon, s.is_control,
                  (SELECT COUNT(*) FROM nodes n WHERE n.site_id=s.site_id) AS node_count,
                  (SELECT COUNT(*) FROM interventions i WHERE i.site_id=s.site_id) AS intervention_count
           FROM sites s ORDER BY s.site_id"""
    )
    return {"sites": [dict(r) for r in rows]}



@router.put("/sites/{site_id}/location")
def set_site_location(
    site_id: str,
    payload: dict,
    request: Request,
    authorization: str | None = Header(default=None),
) -> dict:
    check_rate(request)
    require_bearer_token(authorization, settings.api_token)
    lat = payload.get("lat")
    lon = payload.get("lon")
    if not isinstance(lat, (int, float)) or not -90.0 <= lat <= 90.0:
        raise HTTPException(status_code=422, detail="invalid_lat")
    if not isinstance(lon, (int, float)) or not -180.0 <= lon <= 180.0:
        raise HTTPException(status_code=422, detail="invalid_lon")
    with transaction() as conn:
        cur = conn.execute("UPDATE sites SET lat=?, lon=? WHERE site_id=?",
                           (float(lat), float(lon), site_id))
        if cur.rowcount == 0:
            raise HTTPException(status_code=404, detail="site_not_found")
    return {"status": "located", "site_id": site_id,
            "lat": float(lat), "lon": float(lon)}


@router.put("/nodes/{node_id}/site")
def assign_node_site(
    node_id: str,
    payload: dict,
    request: Request,
    authorization: str | None = Header(default=None),
) -> dict:
    check_rate(request)
    require_bearer_token(authorization, settings.api_token)
    site_id = payload.get("site_id")
    if not isinstance(site_id, str) or not site_id:
        raise HTTPException(status_code=422, detail="missing_site_id")
    with transaction() as conn:
        node = conn.execute(
            "SELECT 1 FROM nodes WHERE node_id=?", (node_id,)
        ).fetchone()
        if not node:
            raise HTTPException(status_code=404, detail="node_not_found")
        site = conn.execute(
            "SELECT 1 FROM sites WHERE site_id=?", (site_id,)
        ).fetchone()
        if not site:
            raise HTTPException(status_code=404, detail="site_not_found")
        conn.execute("UPDATE nodes SET site_id=? WHERE node_id=?", (site_id, node_id))
    return {"status": "assigned", "node_id": node_id, "site_id": site_id}


@router.post("/interventions")
def create_intervention(
    payload: dict,
    request: Request,
    authorization: str | None = Header(default=None),
) -> dict:
    check_rate(request)
    require_bearer_token(authorization, settings.api_token)
    site_id = payload.get("site_id")
    kind = payload.get("kind")
    start_utc_ms = payload.get("start_utc_ms")
    end_utc_ms = payload.get("end_utc_ms")

    if not isinstance(site_id, str) or not site_id:
        raise HTTPException(status_code=422, detail="missing_site_id")
    if not isinstance(kind, str) or not kind:
        raise HTTPException(status_code=422, detail="missing_kind")
    if not isinstance(start_utc_ms, int):
        raise HTTPException(status_code=422, detail="missing_start_utc_ms")
    if end_utc_ms is not None and (
        not isinstance(end_utc_ms, int) or end_utc_ms <= start_utc_ms
    ):
        raise HTTPException(status_code=422, detail="end_before_start")

    with transaction() as conn:
        site = conn.execute(
            "SELECT 1 FROM sites WHERE site_id=?", (site_id,)
        ).fetchone()
        if not site:
            raise HTTPException(status_code=404, detail="site_not_found")
        cur = conn.execute(
            """INSERT INTO interventions(site_id, kind, start_utc_ms, end_utc_ms, notes)
               VALUES(?,?,?,?,?)""",
            (site_id, kind, start_utc_ms, end_utc_ms, payload.get("notes")),
        )
        intervention_id = cur.lastrowid

    return {"status": "created", "intervention_id": intervention_id}


@router.get("/interventions")
def list_interventions(
    request: Request,
    site_id: str | None = None,
    authorization: str | None = Header(default=None),
) -> dict:
    check_rate(request)
    require_bearer_token(authorization, settings.api_token)
    sql = "SELECT * FROM interventions"
    params: list = []
    if site_id:
        sql += " WHERE site_id=?"
        params.append(site_id)
    sql += " ORDER BY start_utc_ms"
    return {"interventions": [dict(r) for r in query(sql, tuple(params))]}


@router.get("/analytics/before-after")
def analytics_before_after(
    intervention_id: int,
    node_id: str,
    variable: str,
    request: Request,
    before_window_days: int | None = None,
    authorization: str | None = Header(default=None),
) -> dict:
    check_rate(request)
    require_bearer_token(authorization, settings.api_token)
    if before_window_days is not None and not 1 <= before_window_days <= 3650:
        raise HTTPException(status_code=422, detail="invalid_before_window_days")

    rows = query(
        "SELECT * FROM interventions WHERE intervention_id=?",
        (intervention_id,),
    )
    if not rows:
        raise HTTPException(status_code=404, detail="intervention_not_found")
    iv = rows[0]
    start = iv["start_utc_ms"]
    end = iv["end_utc_ms"] or int(time.time() * 1000)
    before_from = 0
    if before_window_days is not None and start > 0:
        before_from = max(0, start - before_window_days * 86400000)

    def values(lo: int, hi: int) -> list[float]:
        raw = query(
            """SELECT value FROM measurements
               WHERE node_id=? AND variable=? AND timestamp_utc_ms>=? AND timestamp_utc_ms<=?
                 AND quality IN ('VALID','CALIBRATED','SUSPECT','UNCALIBRATED')
                 AND value IS NOT NULL""",
            (node_id, variable, lo, hi),
        )
        return [r["value"] for r in raw]

    calibration = calibration_for(node_id, variable)

    def calibrated(lo: int, hi: int) -> list[float]:
        return [apply_value(v, calibration) for v in values(lo, hi)]

    before = summary_stats(values(before_from, start - 1) if start > 0 else [])
    after = summary_stats(values(start, end))

    mean_shift = None
    if before["count"] and after["count"]:
        mean_shift = round(after["mean"] - before["mean"], 3)

    calibrated_shift = None
    if start > 0:
        cal_before = summary_stats(calibrated(before_from, start - 1))
        cal_after = summary_stats(calibrated(start, end))
        if cal_before["count"] and cal_after["count"]:
            calibrated_shift = round(cal_after["mean"] - cal_before["mean"], 3)

    sufficient = before["count"] >= 30 and after["count"] >= 30

    controls = _control_group(iv["site_id"], variable, before_from, start, end, start)
    control_deltas = [c["mean_shift"] for c in controls if c["included"]]
    did = difference_in_differences(mean_shift, control_deltas)
    did_calibrated = difference_in_differences(calibrated_shift, control_deltas)

    notes = [
        "descriptive comparison; does not imply causality.",
        "sufficient samples" if sufficient
        else "INSUFFICIENT SAMPLES (<30 per period): interpret with caution",
    ]
    if did is None:
        notes.append(
            "no control group available: difference-in-differences not computed, "
            "so regional weather is not subtracted from the shift"
        )
    else:
        notes.append(
            f"difference-in-differences over {len(control_deltas)} control node(s); "
            "controls are unmatched, so check distance and land cover yourself"
        )
    if calibration and not is_identity(calibration["scale"], calibration["offset"]):
        notes.append("calibrated shift reported separately; raw rows are untouched")

    return {
        "metric_type": "derived_before_after",
        "note": " ".join(notes),
        "sufficient_sample": sufficient,
        "intervention": {
            "id": intervention_id,
            "site_id": iv["site_id"],
            "kind": iv["kind"],
            "start_utc_ms": start,
            "end_utc_ms": iv["end_utc_ms"],
        },
        "node_id": node_id,
        "variable": variable,
        "calibration": calibration_summary(calibration),
        "windows": {
            "before_start_utc_ms": before_from,
            "before_end_utc_ms": start - 1,
            "after_start_utc_ms": start,
            "after_end_utc_ms": end,
        },
        "before": before,
        "after": after,
        "mean_shift": mean_shift,
        "mean_shift_calibrated": calibrated_shift,
        "control_group": {
            "control_node_count": len(control_deltas),
            "excluded_node_count": len(controls) - len(control_deltas),
            "control_mean_shift": (
                round(sum(control_deltas) / len(control_deltas), 3)
                if control_deltas else None
            ),
            "difference_in_differences": did,
            "difference_in_differences_calibrated": did_calibrated,
            "controls": controls,
        },
    }


CONTROL_MIN_SAMPLES = 30


def _control_group(
    site_id: str,
    variable: str,
    before_from: int,
    start: int,
    end: int,
    start_ms: int,
) -> list[dict]:
    treated = query("SELECT lat, lon FROM sites WHERE site_id=?", (site_id,))[0]
    rows = query(
        """SELECT n.node_id, s.site_id, s.lat, s.lon
           FROM nodes n JOIN sites s ON s.site_id=n.site_id
           WHERE s.is_control=1 AND s.site_id<>? ORDER BY s.site_id, n.node_id""",
        (site_id,),
    )
    group: list[dict] = []
    for row in rows:
        before = summary_stats(
            _window_values(row["node_id"], variable, before_from, start_ms - 1)
        )
        after = summary_stats(
            _window_values(row["node_id"], variable, start_ms, end)
        )
        shift = None
        if before["count"] and after["count"]:
            shift = round(after["mean"] - before["mean"], 3)
        entry = {
            "node_id": row["node_id"],
            "site_id": row["site_id"],
            "lat": row["lat"],
            "lon": row["lon"],
            "distance_m": (
                haversine_m(treated["lat"], treated["lon"], row["lat"], row["lon"])
                if treated["lat"] is not None and row["lat"] is not None else None
            ),
            "before_count": before["count"],
            "after_count": after["count"],
            "before_mean": before["mean"],
            "after_mean": after["mean"],
            "mean_shift": shift,
            "included": shift is not None
            and before["count"] >= CONTROL_MIN_SAMPLES
            and after["count"] >= CONTROL_MIN_SAMPLES,
        }
        if not entry["included"]:
            entry["excluded_reason"] = "insufficient_samples"
        group.append(entry)
    return group


def _window_values(node_id: str, variable: str, lo: int, hi: int) -> list[float]:
    if hi < lo:
        return []
    rows = query(
        """SELECT value FROM measurements
           WHERE node_id=? AND variable=? AND timestamp_utc_ms>=? AND timestamp_utc_ms<=?
             AND quality IN ('VALID','CALIBRATED','SUSPECT','UNCALIBRATED')
             AND value IS NOT NULL""",
        (node_id, variable, lo, hi),
    )
    return [r["value"] for r in rows]

