from __future__ import annotations

import math
import time

from fastapi import APIRouter, Header, HTTPException, Request

from .db import query, transaction
from .ratelimit import check_rate
from .security import require_scope, require_site_access

router = APIRouter(prefix="/v1")

IDENTITY_SCALE = 1.0
IDENTITY_OFFSET = 0.0
MAX_ABS_SCALE = 100.0
MAX_ABS_OFFSET = 1000.0
METHODS = ("unspecified", "co-location-relative",
           "single-point-reference")
STATUSES = ("applied", "provisional", "retired", "rejected")
# How the uncertainty figure was arrived at. Recording the kind matters as much
# as the number: a datasheet tolerance and a co-location spread are not the same
# claim, and reporting one as the other would be dishonest.
UNCERTAINTY_KINDS = ("sensor_datasheet", "co_location_spread",
                     "repeatability", "estimated", "unknown")
MAX_UNCERTAINTY = 1000.0
MAINTENANCE_KINDS = ("install", "calibration", "sensor_replacement",
                     "maintenance", "relocation", "note")


def is_identity(scale: float, offset: float) -> bool:
    return scale == IDENTITY_SCALE and offset == IDENTITY_OFFSET


def site_calibrations(site_id: str | None) -> dict[str, dict]:
    if not site_id:
        return {}
    rows = query(
        "SELECT * FROM calibration WHERE site_id=? AND status<>'retired'"
        " ORDER BY variable",
        (site_id,),
    )
    return {r["variable"]: dict(r) for r in rows}


def node_site(node_id: str) -> str | None:
    rows = query("SELECT site_id FROM nodes WHERE node_id=?", (node_id,))
    return rows[0]["site_id"] if rows and rows[0]["site_id"] else None


def calibration_for(node_id: str, variable: str) -> dict | None:
    return site_calibrations(node_site(node_id)).get(variable)


def node_calibration_map() -> dict[str, dict[str, dict]]:
    """Every node's calibrations in one query, for bulk transforms such as
    CSV export where a per-row lookup would dominate the runtime."""
    rows = query(
        """SELECT n.node_id, c.variable, c.scale, c.offset, c.method, c.status,
                  c.calibration_date, c.calibration_reference,
                  c.uncertainty, c.uncertainty_kind
           FROM nodes n JOIN calibration c ON c.site_id = n.site_id
           WHERE c.status<>'retired'"""
    )
    out: dict[str, dict[str, dict]] = {}
    for r in rows:
        out.setdefault(r["node_id"], {})[r["variable"]] = dict(r)
    return out


def apply_value(value: float | None, calibration: dict | None) -> float | None:
    if value is None or not calibration:
        return value
    return round(value * calibration["scale"] + calibration["offset"], 6)


def transform_stats(stats: dict, calibration: dict | None) -> dict:
    """Exact under a linear map: location statistics shift, dispersion scales.

    Aggregated hourly buckets can therefore be corrected without re-reading
    the raw records.
    """
    if not calibration or is_identity(calibration["scale"],
                                      calibration["offset"]):
        return stats
    scale = calibration["scale"]
    offset = calibration["offset"]
    out = dict(stats)
    for key in ("min", "max", "mean", "median", "p05", "p25", "p75", "p95"):
        if out.get(key) is not None:
            out[key] = round(out[key] * scale + offset, 6)
    if scale < 0:
        out["min"], out["max"] = out["max"], out["min"]
    if out.get("stddev") is not None:
        out["stddev"] = round(out["stddev"] * abs(scale), 6)
    if out.get("stddev_pop") is not None:
        out["stddev_pop"] = round(out["stddev_pop"] * abs(scale), 6)
    return out


def calibrated_uncertainty(calibration: dict | None) -> float | None:
    """Absolute uncertainty of a calibrated value, in the variable's unit.

    A correction expressed as a scale moves the uncertainty with it: dividing
    the signal by two halves the error the scale introduces. An offset is an
    addition, so its uncertainty passes through unchanged. When no uncertainty
    was ever characterised this returns None rather than 0, because "nobody
    measured it" and "it is exact" must not look the same.
    """
    if not calibration:
        return None
    recorded = calibration.get("uncertainty")
    if recorded is None:
        return None
    return round(float(recorded) * abs(float(calibration["scale"])), 6)


def calibration_summary(calibration: dict | None) -> dict:
    if not calibration:
        return {"applied": False, "identity": True, "uncertainty": None}
    uncertainty = calibrated_uncertainty(calibration)
    return {
        "applied": True,
        "identity": is_identity(calibration["scale"], calibration["offset"]),
        "scale": calibration["scale"],
        "offset": calibration["offset"],
        "method": calibration["method"],
        "status": calibration["status"],
        "calibration_date": calibration["calibration_date"],
        "calibration_reference": calibration["calibration_reference"],
        # None here is a statement: nobody characterised this. It is not the
        # same as zero, and the docs depend on the difference staying visible.
        "uncertainty": uncertainty,
        "uncertainty_kind": calibration.get("uncertainty_kind"),
    }


def _number(payload: dict, key: str, default: float | None,
            low: float, high: float) -> float:
    value = payload.get(key, default)
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise HTTPException(status_code=422, detail=f"invalid_{key}")
    value = float(value)
    if not math.isfinite(value) or value < low or value > high:
        raise HTTPException(status_code=422, detail=f"invalid_{key}")
    return value


def _optional_number(payload: dict, key: str, low: float,
                    high: float) -> float | None:
    """A number allowed to be absent, but which must be sane when present."""
    value = payload.get(key)
    if value is None:
        return None
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise HTTPException(status_code=422, detail=f"invalid_{key}")
    value = float(value)
    if not math.isfinite(value) or value < low or value > high:
        raise HTTPException(status_code=422, detail=f"invalid_{key}")
    return value


def _choice(payload: dict, key: str, allowed: tuple[str, ...],
            default: str) -> str:
    value = payload.get(key, default)
    if not isinstance(value, str) or value not in allowed:
        raise HTTPException(status_code=422, detail=f"invalid_{key}")
    return value


@router.put("/sites/{site_id}/calibration")
def put_calibration(
    site_id: str,
    payload: dict,
    request: Request,
    authorization: str | None = Header(default=None),
) -> dict:
    check_rate(request)
    principal = require_scope(authorization, "write")
    require_site_access(principal, site_id)
    variable = payload.get("variable")
    if not isinstance(variable, str) or not variable:
        raise HTTPException(status_code=422, detail="missing_variable")
    if not query("SELECT 1 FROM sites WHERE site_id=?", (site_id,)):
        raise HTTPException(status_code=404, detail="site_not_found")

    scale = _number(payload, "scale", IDENTITY_SCALE, -MAX_ABS_SCALE, MAX_ABS_SCALE)
    if scale == 0.0:
        raise HTTPException(status_code=422, detail="invalid_scale")
    offset = _number(payload, "offset", IDENTITY_OFFSET, -MAX_ABS_OFFSET,
                     MAX_ABS_OFFSET)
    method = _choice(payload, "method", METHODS, "unspecified")
    uncertainty = _optional_number(payload, "uncertainty", 0.0, MAX_UNCERTAINTY)
    uncertainty_kind = payload.get("uncertainty_kind")
    if uncertainty_kind is not None:
        if (not isinstance(uncertainty_kind, str)
                or uncertainty_kind not in UNCERTAINTY_KINDS):
            raise HTTPException(status_code=422,
                                detail="invalid_uncertainty_kind")
        if uncertainty is None:
            # Naming the kind of an uncertainty nobody quantified is worse than
            # saying nothing.
            raise HTTPException(status_code=422,
                                detail="uncertainty_kind_without_uncertainty")
    status = _choice(payload, "status", STATUSES, "applied")
    reference = payload.get("calibration_reference")
    sensor_id = payload.get("sensor_id")
    calibration_date = payload.get("calibration_date")
    for name, value in (("calibration_reference", reference),
                        ("sensor_id", sensor_id),
                        ("calibration_date", calibration_date),
                        ("notes", payload.get("notes"))):
        if value is not None and not isinstance(value, str):
            raise HTTPException(status_code=422, detail=f"invalid_{name}")
    if calibration_date is not None and not _is_iso_date(calibration_date):
        raise HTTPException(status_code=422, detail="invalid_calibration_date")

    now_ms = int(time.time() * 1000)
    with transaction() as conn:
        conn.execute(
            """INSERT INTO calibration
               (site_id, variable, scale, offset, method, calibration_reference,
                sensor_id, calibration_date, status, uncertainty,
                uncertainty_kind, notes, updated_utc_ms)
               VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)
               ON CONFLICT(site_id, variable) DO UPDATE SET
                 scale=excluded.scale, offset=excluded.offset,
                 method=excluded.method,
                 calibration_reference=excluded.calibration_reference,
                 sensor_id=excluded.sensor_id,
                 calibration_date=excluded.calibration_date,
                 status=excluded.status,
                 uncertainty=COALESCE(excluded.uncertainty,
                                     calibration.uncertainty),
                 uncertainty_kind=COALESCE(excluded.uncertainty_kind,
                                           calibration.uncertainty_kind),
                 notes=excluded.notes,
                 updated_utc_ms=excluded.updated_utc_ms""",
            (site_id, variable, scale, offset, method, reference, sensor_id,
             calibration_date, status, uncertainty, uncertainty_kind,
             payload.get("notes"), now_ms),
        )
    return {"status": "calibrated", "site_id": site_id, "variable": variable,
            "scale": scale, "offset": offset, "method": method,
            "calibration_status": status, "updated_utc_ms": now_ms}


def _is_iso_date(value: str) -> bool:
    parts = value.split("-")
    if len(parts) != 3 or len(parts[0]) != 4:
        return False
    try:
        return all(p.isdigit() for p in parts)
    except AttributeError:
        return False


@router.get("/sites/{site_id}/calibration")
def get_calibration(
    site_id: str,
    request: Request,
    authorization: str | None = Header(default=None),
) -> dict:
    check_rate(request)
    principal = require_scope(authorization, "read")
    require_site_access(principal, site_id)
    if not query("SELECT 1 FROM sites WHERE site_id=?", (site_id,)):
        raise HTTPException(status_code=404, detail="site_not_found")
    rows = query(
        "SELECT * FROM calibration WHERE site_id=? ORDER BY variable", (site_id,)
    )
    return {
        "site_id": site_id,
        "model": "calibrated_value = raw_value * scale + offset",
        "note": (
            "raw measurements are never modified; the central derives "
            "calibrated values so a recalibration can be replayed over history"
        ),
        "calibrations": [dict(r) for r in rows],
    }


@router.post("/sites/{site_id}/maintenance")
def log_maintenance(
    site_id: str,
    payload: dict,
    request: Request,
    authorization: str | None = Header(default=None),
) -> dict:
    check_rate(request)
    principal = require_scope(authorization, "write")
    require_site_access(principal, site_id)
    kind = _choice(payload, "kind", MAINTENANCE_KINDS, "note")
    at_utc_ms = payload.get("at_utc_ms", int(time.time() * 1000))
    if isinstance(at_utc_ms, bool) or not isinstance(at_utc_ms, int) or at_utc_ms < 0:
        raise HTTPException(status_code=422, detail="invalid_at_utc_ms")
    notes = payload.get("notes")
    if notes is not None and not isinstance(notes, str):
        raise HTTPException(status_code=422, detail="invalid_notes")
    if not query("SELECT 1 FROM sites WHERE site_id=?", (site_id,)):
        raise HTTPException(status_code=404, detail="site_not_found")
    with transaction() as conn:
        cur = conn.execute(
            "INSERT INTO maintenance_events(site_id, kind, at_utc_ms, notes)"
            " VALUES(?,?,?,?)", (site_id, kind, at_utc_ms, notes))
        event_id = cur.lastrowid
    return {"status": "logged", "event_id": event_id, "site_id": site_id,
            "kind": kind, "at_utc_ms": at_utc_ms}


@router.get("/sites/{site_id}/maintenance")
def list_maintenance(
    site_id: str,
    request: Request,
    authorization: str | None = Header(default=None),
) -> dict:
    check_rate(request)
    principal = require_scope(authorization, "read")
    require_site_access(principal, site_id)
    if not query("SELECT 1 FROM sites WHERE site_id=?", (site_id,)):
        raise HTTPException(status_code=404, detail="site_not_found")
    rows = query(
        "SELECT * FROM maintenance_events WHERE site_id=? ORDER BY at_utc_ms DESC"
        " LIMIT 200", (site_id,))
    return {"site_id": site_id, "events": [dict(r) for r in rows]}
