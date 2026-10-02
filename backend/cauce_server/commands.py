"""Downlink commands.

A command is an instruction the central sends to a node that already reports
up. Two rules keep it safe to retry, which is the whole point of an
offline-first node:

- **Idempotency.** The caller supplies `idempotency_key`; `(node_id, key)` is
  unique, so re-posting the same intent returns the original command instead of
  queueing a second one. A node that loses the ack and retries therefore never
  actuates twice.
- **Honest delivery.** A command is only `delivered` once the node reports it in
  a `/v1/sync` round trip, and only `acked` once the node reports an outcome.
  A command that never reaches the node stays visibly pending instead of
  quietly expiring into a fiction.

Delivery is deliberately push-on-poll rather than a socket: the node already
talks to the central on its own schedule, and reusing that channel means no new
radio wakeups and no new failure mode.
"""

from __future__ import annotations

import json
import time

from fastapi import APIRouter, Header, HTTPException, Request

from .db import query, transaction
from .ratelimit import check_rate
from .security import require_scope, require_site_access

router = APIRouter(prefix="/v1", tags=["commands"])

# Kept small and side-effect free. `set_sampling_interval` and `resync_now` are
# safe to repeat; anything that physically moves hardware is not in this table
# on purpose.
COMMAND_KINDS: tuple[str, ...] = (
    "set_sampling_interval",
    "set_sync_interval",
    "request_resync",
    "set_led_mode",
)

MAX_COMMANDS_PER_POLL = 8
MAX_KEY_LENGTH = 64
MAX_PAYLOAD_BYTES = 512
DEFAULT_TTL_MS = 24 * 3600_000


def _utc_ms() -> int:
    return int(time.time() * 1000)


def node_exists(node_id: str) -> bool:
    return bool(query("SELECT 1 FROM nodes WHERE node_id=?", (node_id,)))


def _site_of(node_id: str) -> str | None:
    rows = query("SELECT site_id FROM nodes WHERE node_id=?", (node_id,))
    return rows[0]["site_id"] if rows else None


def enqueue_command(
    node_id: str,
    idempotency_key: str,
    kind: str,
    payload: dict | None = None,
    ttl_ms: int | None = None,
) -> tuple[dict, bool]:
    """Queues a command, or returns the existing one for the same key.

    Returns `(command, created)` so the caller can answer 200 for a repeat and
    201 for a first submission. A repeat with a *different* body is a client
    bug and is refused rather than silently ignored.
    """
    body = json.dumps(payload or {}, separators=(",", ":"), sort_keys=True)
    now = _utc_ms()
    expires = now + (DEFAULT_TTL_MS if ttl_ms is None else ttl_ms)

    with transaction() as conn:
        existing = conn.execute(
            "SELECT * FROM commands WHERE node_id=? AND idempotency_key=?",
            (node_id, idempotency_key),
        ).fetchone()
        if existing is not None:
            if existing["payload"] != body or existing["kind"] != kind:
                raise HTTPException(
                    status_code=409,
                    detail=(
                        "idempotency_key_reused_with_different_command"
                    ),
                )
            return dict(existing), False
        cur = conn.execute(
            "INSERT INTO commands(node_id, idempotency_key, kind, payload,"
            " created_at_utc_ms, expires_at_utc_ms)"
            " VALUES(?,?,?,?,?,?)",
            (node_id, idempotency_key, kind, body, now, expires),
        )
        command_id = cur.lastrowid

    return {
        "command_id": command_id,
        "node_id": node_id,
        "idempotency_key": idempotency_key,
        "kind": kind,
        "payload": body,
        "created_at_utc_ms": now,
        "expires_at_utc_ms": expires,
        "delivered_utc_ms": None,
        "acked_utc_ms": None,
        "result": None,
    }, True


def pending_commands(node_id: str, limit: int = MAX_COMMANDS_PER_POLL) -> list[dict]:
    """Commands the node has not acknowledged yet, oldest first.

    Expiry is applied here rather than by a sweeper so a stale command
    disappears exactly when the node next asks, with no background job to
    reason about.
    """
    now = _utc_ms()
    rows = query(
        "SELECT * FROM commands WHERE node_id=? AND acked_utc_ms IS NULL"
        " AND (expires_at_utc_ms IS NULL OR expires_at_utc_ms>?)"
        " ORDER BY command_id LIMIT ?",
        (node_id, now, limit),
    )
    return [dict(r) for r in rows]


def record_receipts(node_id: str, receipts: list[dict]) -> dict:
    """Stores what the node reported about commands it was given.

    A receipt is `(command_id, state, detail)` where state is `delivered` or
    `acked`. Receipts for unknown or foreign commands are counted and ignored
    rather than trusted: the node is an authenticated peer but not an authority
    on someone else's command.
    """
    accepted = 0
    ignored = 0
    now = _utc_ms()
    with transaction() as conn:
        for receipt in receipts:
            command_id = receipt.get("command_id")
            state = receipt.get("state")
            detail = receipt.get("detail")
            if (
                isinstance(command_id, bool)
                or not isinstance(command_id, int)
                or state not in ("delivered", "acked")
            ):
                ignored += 1
                continue
            row = conn.execute(
                "SELECT command_id FROM commands WHERE command_id=? AND node_id=?",
                (command_id, node_id),
            ).fetchone()
            if row is None:
                ignored += 1
                continue
            conn.execute(
                "INSERT OR IGNORE INTO command_receipts(node_id, command_id,"
                " state, detail, at_utc_ms) VALUES(?,?,?,?,?)",
                (node_id, command_id, state, detail, now),
            )
            if state == "delivered":
                conn.execute(
                    "UPDATE commands SET delivered_utc_ms=COALESCE("
                    "delivered_utc_ms, ?) WHERE command_id=?",
                    (now, command_id),
                )
            else:
                conn.execute(
                    "UPDATE commands SET delivered_utc_ms=COALESCE("
                    "delivered_utc_ms, ?), acked_utc_ms=COALESCE(acked_utc_ms, ?),"
                    " result=? WHERE command_id=?",
                    (now, now, detail, command_id),
                )
            accepted += 1
    return {"accepted": accepted, "ignored": ignored}


def command_status(node_id: str, command_id: int) -> dict | None:
    rows = query(
        "SELECT * FROM commands WHERE node_id=? AND command_id=?",
        (node_id, command_id),
    )
    if not rows:
        return None
    command = dict(rows[0])
    receipts = query(
        "SELECT state, detail, at_utc_ms FROM command_receipts"
        " WHERE node_id=? AND command_id=? ORDER BY at_utc_ms",
        (node_id, command_id),
    )
    command["receipts"] = [dict(r) for r in receipts]
    if command["acked_utc_ms"] is not None:
        command["state"] = "acked"
    elif command["delivered_utc_ms"] is not None:
        command["state"] = "delivered"
    else:
        command["state"] = "pending"
    return command


@router.post("/nodes/{node_id}/commands")
def post_command(
    node_id: str,
    payload: dict,
    request: Request,
    authorization: str | None = Header(default=None),
) -> dict:
    check_rate(request)
    principal = require_scope(authorization, "write")

    kind = payload.get("kind")
    if kind not in COMMAND_KINDS:
        raise HTTPException(
            status_code=422,
            detail={"error": "invalid_kind", "allowed": list(COMMAND_KINDS)},
        )
    key = payload.get("idempotency_key")
    if not isinstance(key, str) or not key or len(key) > MAX_KEY_LENGTH:
        raise HTTPException(status_code=422, detail="invalid_idempotency_key")

    body = payload.get("payload", {})
    if not isinstance(body, dict):
        raise HTTPException(status_code=422, detail="invalid_payload")
    encoded = json.dumps(body, separators=(",", ":"), sort_keys=True)
    if len(encoded.encode("utf-8")) > MAX_PAYLOAD_BYTES:
        raise HTTPException(status_code=422, detail="payload_too_large")

    ttl_ms = payload.get("ttl_ms", DEFAULT_TTL_MS)
    if isinstance(ttl_ms, bool) or not isinstance(ttl_ms, int) or ttl_ms <= 0:
        raise HTTPException(status_code=422, detail="invalid_ttl_ms")

    if not node_exists(node_id):
        raise HTTPException(status_code=404, detail="node_not_found")
    require_site_access(principal, _site_of(node_id))

    command, created = enqueue_command(node_id, key, kind, body, ttl_ms)
    return {
        "status": "queued" if created else "already_queued",
        "created": created,
        "command_id": command["command_id"],
        "node_id": node_id,
        "kind": kind,
        "idempotency_key": key,
        "expires_at_utc_ms": command["expires_at_utc_ms"],
        "note": (
            "queued, not delivered: the node marks it delivered when it next "
            "reports it in a /v1/sync round trip"
        ),
    }


@router.get("/nodes/{node_id}/commands")
def list_commands(
    node_id: str,
    request: Request,
    limit: int = 50,
    authorization: str | None = Header(default=None),
) -> dict:
    check_rate(request)
    principal = require_scope(authorization, "read")
    if not node_exists(node_id):
        raise HTTPException(status_code=404, detail="node_not_found")
    require_site_access(principal, _site_of(node_id))
    limit = max(1, min(limit, 500))
    rows = query(
        "SELECT * FROM commands WHERE node_id=? ORDER BY command_id DESC LIMIT ?",
        (node_id, limit),
    )
    now = _utc_ms()
    commands = []
    for row in rows:
        command = dict(row)
        if command["acked_utc_ms"] is not None:
            command["state"] = "acked"
        elif command["delivered_utc_ms"] is not None:
            command["state"] = "delivered"
        elif command["expires_at_utc_ms"] is not None and command[
            "expires_at_utc_ms"
        ] <= now:
            command["state"] = "expired"
        else:
            command["state"] = "pending"
        commands.append(command)
    return {"node_id": node_id, "commands": commands, "total": len(commands)}
