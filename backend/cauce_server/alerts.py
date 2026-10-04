from __future__ import annotations

import json
import time
import urllib.request

from fastapi import APIRouter, Header, HTTPException, Request

from .config import settings
from .db import query, transaction
from .ratelimit import check_rate
from .security import require_admin_write, require_bearer_token

router = APIRouter(prefix="/v1")

KINDS = ("heat", "stale")
CHANNELS = ("webhook", "telegram")


def _send(rule: dict, message: str) -> bool:
    channel = rule["channel"]
    target = rule["target"] or ""
    try:
        if channel == "webhook":
            if not target:
                return False
            req = urllib.request.Request(
                target,
                data=json.dumps({"text": message,
                                 "node_id": rule["node_id"],
                                 "kind": rule["kind"]}).encode(),
                headers={"Content-Type": "application/json"}, method="POST")
            with urllib.request.urlopen(req, timeout=10) as resp:
                return 200 <= resp.status < 300
        if channel == "telegram":
            token = settings.telegram_bot_token
            if not token or not target:
                return False
            req = urllib.request.Request(
                f"https://api.telegram.org/bot{token}/sendMessage",
                data=json.dumps({"chat_id": target,
                                 "text": message}).encode(),
                headers={"Content-Type": "application/json"}, method="POST")
            with urllib.request.urlopen(req, timeout=10) as resp:
                return 200 <= resp.status < 300
    except Exception:
        return False
    return False


def _fire(rule: dict, message: str) -> None:
    now_ms = int(time.time() * 1000)
    delivered = _send(rule, message)
    with transaction() as conn:
        conn.execute(
            "UPDATE alert_rules SET last_fired_utc_ms=? WHERE rule_id=?",
            (now_ms, rule["rule_id"]))
        conn.execute(
            "INSERT INTO alert_log(rule_id,node_id,fired_utc_ms,message,delivered,attempts)"
            " VALUES(?,?,?,?,?,1)",
            (rule["rule_id"], rule["node_id"], now_ms, message,
             1 if delivered else 0))


def retry_pending(max_attempts: int = 5) -> int:
    redelivered = 0
    pending = query(
        "SELECT l.log_id, l.rule_id, l.node_id, l.message, l.attempts,"
        " r.channel, r.target, r.kind, r.enabled"
        " FROM alert_log l JOIN alert_rules r ON r.rule_id=l.rule_id"
        " WHERE l.delivered=0 AND l.attempts<?", (max_attempts,))
    for row in pending:
        if not row["enabled"]:
            continue
        rule = {"rule_id": row["rule_id"], "node_id": row["node_id"],
                "channel": row["channel"], "target": row["target"],
                "kind": row["kind"]}
        if _send(rule, row["message"]):
            with transaction() as conn:
                conn.execute(
                    "UPDATE alert_log SET delivered=1, attempts=attempts+1"
                    " WHERE log_id=?", (row["log_id"],))
            redelivered += 1
        else:
            with transaction() as conn:
                conn.execute(
                    "UPDATE alert_log SET attempts=attempts+1 WHERE log_id=?",
                    (row["log_id"],))
    return redelivered


def _due(rule: dict, now_ms: int) -> bool:
    return (now_ms - (rule["last_fired_utc_ms"] or 0)
            >= (rule["cooldown_min"] or 0) * 60000)


def evaluate_heat_rules(node_id: str) -> list[str]:
    now_ms = int(time.time() * 1000)
    fired = []
    rules = query(
        "SELECT * FROM alert_rules WHERE enabled=1 AND kind='heat'"
        " AND (node_id=? OR node_id='*')", (node_id,))
    for rule in rules:
        if not _due(rule, now_ms):
            continue
        rows = query(
            "SELECT timestamp_utc_ms, value FROM measurements WHERE node_id=?"
            " AND variable='air_temperature' AND value IS NOT NULL"
            " AND quality IN ('VALID','CALIBRATED','SUSPECT','UNCALIBRATED')"
            " ORDER BY sequence DESC LIMIT 500", (node_id,))
        if not rows:
            continue
        threshold = rule["threshold"] if rule["threshold"] is not None else 32.0
        span_ms = 0
        peak = None
        first_ts = rows[0]["timestamp_utc_ms"]
        for r in rows:
            if r["value"] is None or r["value"] < threshold:
                break
            peak = r["value"] if peak is None else max(peak, r["value"])
            span_ms = first_ts - r["timestamp_utc_ms"]
        need_ms = (rule["min_duration_min"] or 0) * 60000
        if peak is not None and span_ms >= need_ms:
            message = (f"CAUCE heat alert: {node_id} at {peak:.1f} C"
                       f" (>= {threshold:.1f} C for {span_ms // 60000} min)")
            _fire(dict(rule), message)
            fired.append(message)
    return fired


def evaluate_stale_rules() -> list[str]:
    now_ms = int(time.time() * 1000)
    fired = []
    rules = query(
        "SELECT * FROM alert_rules WHERE enabled=1 AND kind='stale'")
    for rule in rules:
        if not _due(rule, now_ms):
            continue
        stale_min = rule["stale_min"] or 60
        if rule["node_id"] == "*":
            nodes = query(
                "SELECT node_id, last_seen_utc_ms FROM nodes"
                " WHERE last_seen_utc_ms IS NOT NULL"
                " AND last_seen_utc_ms<?", (now_ms - stale_min * 60000,))
            for node in nodes:
                message = (f"CAUCE stale alert: {node['node_id']} silent for"
                           f" >{stale_min} min")
                _fire(dict(rule), message)
                fired.append(message)
        else:
            nodes = query(
                "SELECT node_id, last_seen_utc_ms FROM nodes WHERE node_id=?",
                (rule["node_id"],))
            if (nodes and nodes[0]["last_seen_utc_ms"]
                    and nodes[0]["last_seen_utc_ms"] < now_ms - stale_min * 60000):
                message = (f"CAUCE stale alert: {rule['node_id']} silent for"
                           f" >{stale_min} min")
                _fire(dict(rule), message)
                fired.append(message)
    return fired


@router.post("/alerts/rules")
def create_rule(payload: dict, request: Request,
                authorization: str | None = Header(default=None)) -> dict:
    check_rate(request)
    require_admin_write(authorization)
    kind = payload.get("kind")
    channel = payload.get("channel")
    node_id = payload.get("node_id")
    if kind not in KINDS:
        raise HTTPException(status_code=422, detail="invalid_kind")
    if channel not in CHANNELS:
        raise HTTPException(status_code=422, detail="invalid_channel")
    if not isinstance(node_id, str) or not node_id:
        raise HTTPException(status_code=422, detail="missing_node_id")
    threshold = payload.get("threshold")
    if threshold is not None and (
            not isinstance(threshold, (int, float))
            or threshold != threshold
            or threshold in (float("inf"), float("-inf"))):
        raise HTTPException(status_code=422, detail="invalid_threshold")
    for key, minimum in (("stale_min", 1), ("cooldown_min", 1),
                         ("min_duration_min", 0)):
        value = payload.get(key)
        if value is None:
            continue
        if not isinstance(value, int) or value < minimum:
            raise HTTPException(status_code=422, detail=f"invalid_{key}")
    target = payload.get("target") or ""
    if not isinstance(target, str) or len(target) > 500:
        raise HTTPException(status_code=422, detail="invalid_target")
    with transaction() as conn:
        cur = conn.execute(
            """INSERT INTO alert_rules(node_id,kind,threshold,min_duration_min,
               stale_min,channel,target,cooldown_min,enabled)
               VALUES(?,?,?,?,?,?,?,?,1)""",
            (node_id, kind, threshold,
             int(payload.get("min_duration_min") or 0),
             int(payload.get("stale_min") or 60), channel, target,
             int(payload.get("cooldown_min") or 60)))
        rule_id = cur.lastrowid
    return {"status": "created", "rule_id": rule_id}


@router.patch("/alerts/rules/{rule_id}")
def update_rule(rule_id: int, payload: dict, request: Request,
                authorization: str | None = Header(default=None)) -> dict:
    check_rate(request)
    require_bearer_token(authorization, settings.api_token)
    fields: list = []
    params: list = []
    if "enabled" in payload:
        if not isinstance(payload["enabled"], bool):
            raise HTTPException(status_code=422, detail="invalid_enabled")
        fields.append("enabled=?")
        params.append(1 if payload["enabled"] else 0)
    if "cooldown_min" in payload:
        value = payload["cooldown_min"]
        if not isinstance(value, int) or value < 1:
            raise HTTPException(status_code=422, detail="invalid_cooldown_min")
        fields.append("cooldown_min=?")
        params.append(value)
    if "threshold" in payload:
        value = payload["threshold"]
        if value is not None and (
                not isinstance(value, (int, float)) or value != value
                or value in (float("inf"), float("-inf"))):
            raise HTTPException(status_code=422, detail="invalid_threshold")
        fields.append("threshold=?")
        params.append(value)
    if "target" in payload:
        value = payload["target"] or ""
        if not isinstance(value, str) or len(value) > 500:
            raise HTTPException(status_code=422, detail="invalid_target")
        fields.append("target=?")
        params.append(value)
    if not fields:
        raise HTTPException(status_code=422, detail="nothing_to_update")
    params.append(rule_id)
    with transaction() as conn:
        cur = conn.execute(
            f"UPDATE alert_rules SET {', '.join(fields)} WHERE rule_id=?",
            tuple(params))
        if cur.rowcount == 0:
            raise HTTPException(status_code=404, detail="rule_not_found")
    return {"status": "updated", "rule_id": rule_id}


@router.get("/alerts/rules")
def list_rules(request: Request,
               authorization: str | None = Header(default=None)) -> dict:
    check_rate(request)
    require_bearer_token(authorization, settings.api_token)
    return {"rules": [dict(r) for r in query(
        "SELECT * FROM alert_rules ORDER BY rule_id")]}


@router.delete("/alerts/rules/{rule_id}")
def delete_rule(rule_id: int, request: Request,
                authorization: str | None = Header(default=None)) -> dict:
    check_rate(request)
    require_admin_write(authorization)
    with transaction() as conn:
        cur = conn.execute("DELETE FROM alert_rules WHERE rule_id=?",
                           (rule_id,))
        if cur.rowcount == 0:
            raise HTTPException(status_code=404, detail="rule_not_found")
    return {"status": "deleted", "rule_id": rule_id}


@router.get("/alerts/log")
def alert_log(request: Request,
              authorization: str | None = Header(default=None),
              limit: int = 50) -> dict:
    check_rate(request)
    require_bearer_token(authorization, settings.api_token)
    limit = max(1, min(limit, 500))
    return {"entries": [dict(r) for r in query(
        "SELECT * FROM alert_log ORDER BY log_id DESC LIMIT ?", (limit,))]}


@router.post("/alerts/check")
@router.get("/alerts/check")
def run_check(request: Request,
              authorization: str | None = Header(default=None)) -> dict:
    check_rate(request)
    require_bearer_token(authorization, settings.api_token)
    fired = evaluate_stale_rules()
    redelivered = retry_pending()
    return {"status": "checked", "fired": fired,
            "redelivered": redelivered}
