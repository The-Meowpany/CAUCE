from __future__ import annotations

import time

from fastapi import APIRouter, Header, HTTPException, Request

from .analytics import summary_stats
from .config import settings
from .db import query, transaction

router = APIRouter(prefix="/v1")


def _require_api_auth(authorization: str | None) -> None:
    expected = settings.api_token
    if not expected:
        return
    if authorization != f"Bearer {expected}":
        raise HTTPException(status_code=401, detail="unauthorized")


@router.post("/sites")
def create_site(
    payload: dict,
    request: Request,
    authorization: str | None = Header(default=None),
) -> dict:
    _require_api_auth(authorization)
    site_id = payload.get("site_id")
    if not isinstance(site_id, str) or not site_id:
        raise HTTPException(status_code=422, detail="missing_site_id")
    with transaction() as conn:
        existing = conn.execute(
            "SELECT 1 FROM sites WHERE site_id=?", (site_id,)
        ).fetchone()
        if existing:
            raise HTTPException(status_code=409, detail="site_exists")
        conn.execute(
            "INSERT INTO sites(site_id, name, notes) VALUES(?,?,?)",
            (site_id, payload.get("name"), payload.get("notes")),
        )
    return {"status": "created", "site_id": site_id}


@router.get("/sites")
def list_sites(authorization: str | None = Header(default=None)) -> dict:
    _require_api_auth(authorization)
    rows = query(
        """SELECT s.site_id, s.name, s.notes,
                  (SELECT COUNT(*) FROM nodes n WHERE n.site_id=s.site_id) AS node_count,
                  (SELECT COUNT(*) FROM interventions i WHERE i.site_id=s.site_id) AS intervention_count
           FROM sites s ORDER BY s.site_id"""
    )
    return {"sites": [dict(r) for r in rows]}


@router.put("/nodes/{node_id}/site")
def assign_node_site(
    node_id: str,
    payload: dict,
    request: Request,
    authorization: str | None = Header(default=None),
) -> dict:
    _require_api_auth(authorization)
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
    _require_api_auth(authorization)
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
    site_id: str | None = None,
    authorization: str | None = Header(default=None),
) -> dict:
    _require_api_auth(authorization)
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
    authorization: str | None = Header(default=None),
) -> dict:
    _require_api_auth(authorization)

    rows = query(
        "SELECT * FROM interventions WHERE intervention_id=?",
        (intervention_id,),
    )
    if not rows:
        raise HTTPException(status_code=404, detail="intervention_not_found")
    iv = rows[0]
    start = iv["start_utc_ms"]
    end = iv["end_utc_ms"] or int(time.time() * 1000)

    def values(lo: int, hi: int) -> list[float]:
        raw = query(
            """SELECT value FROM measurements
               WHERE node_id=? AND variable=? AND timestamp_utc_ms>=? AND timestamp_utc_ms<?
                 AND quality IN ('VALID','CALIBRATED','SUSPECT','UNCALIBRATED')
                 AND value IS NOT NULL""",
            (node_id, variable, lo, hi),
        )
        return [r["value"] for r in raw]

    before = summary_stats(values(0, start))
    after = summary_stats(values(start, end))

    mean_shift = None
    if before["count"] and after["count"]:
        mean_shift = round(after["mean"] - before["mean"], 3)

    sufficient = before["count"] >= 30 and after["count"] >= 30

    return {
        "metric_type": "derived_before_after",
        "note": (
            "comparacion descriptiva; no implica causalidad. "
            + ("muestras suficientes" if sufficient else "MUESTRAS INSUFICIENTES (<30 por periodo): interpretar con cautela")
        ),
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
        "before": before,
        "after": after,
        "mean_shift": mean_shift,
    }
