"""Retiring a node's identity.

A device that is lost, stolen or known-compromised has to stop being able to
contribute data, and it has to stop being able to do so *without* the operator
having to reach the device. This module is the whole of that mechanism.

Three decisions worth stating, because the obvious alternatives are worse:

- **Retirement is not deletion.** The measurements a node already contributed
  stay. A device that was compromised for a week made real observations during part
  of that week, and deleting them destroys the evidence along with the data.
  Deleting the row would cascade them away.
- **There is no "un-revoke".** Reinstating a node means provisioning it again with
  a *new* key, which is what clears the retirement. An endpoint that flipped a flag
  back would let a compromised identity be restored by whoever compromised it.
- **Retirement is checked before authentication, not after.** Checking after a
  valid signature is written would mean the node's own key is still accepted for
  the request that reports its own retirement.
"""

from __future__ import annotations

from .db import query, transaction

# Why a node was retired. Free text on purpose: the operator knows things the schema
# does not ("sold", "water ingress", "key exposed in a support ticket"), and a
# closed vocabulary would get "other" used for all of them.
MAX_REASON_BYTES = 280


class NodeRetired(Exception):
    """Raised when a retired node tries to contribute data."""

    def __init__(self, node_id: str, retired_at_ms: int, reason: str | None):
        self.node_id = node_id
        self.retired_at_ms = retired_at_ms
        self.reason = reason
        super().__init__(f"node_retired:{node_id}")


def retirement_state(node_id: str) -> tuple[int | None, str | None]:
    """(retired_at_ms, reason) for a node, or (None, None) while it is good.

    A node that has never been seen reads as good rather than raising. Absence of a
    record is not evidence of retirement, and the authentication that follows will
    reject an unknown node anyway.
    """
    rows = query(
        "SELECT revoked_at_utc_ms, revoked_reason FROM nodes WHERE node_id=?",
        (node_id,),
    )
    if not rows:
        return (None, None)
    row = rows[0]
    return (row["revoked_at_utc_ms"], row["revoked_reason"])


def is_retired(node_id: str) -> bool:
    retired_at, _ = retirement_state(node_id)
    return retired_at is not None


def retire(node_id: str, reason: str | None) -> dict:
    """Retires a node's identity. Idempotent: retiring twice is not an error.

    Idempotent because the operator's intent is "this node must not be able to
    contribute", and that intent is already satisfied. Refusing the second call would
    only teach the operator to catch the exception.
    """
    now_ms = _now_ms()
    cleaned = (reason or "").strip()[:MAX_REASON_BYTES] or None
    with transaction() as conn:
        existing = conn.execute(
            "SELECT revoked_at_utc_ms FROM nodes WHERE node_id=?", (node_id,)
        ).fetchone()
        if existing is None:
            # Recorded even though the node is unknown. A retirement that silently
            # does nothing because the id was typed wrong is the one failure mode
            # here that matters: the operator believes the device is retired.
            conn.execute(
                """INSERT INTO nodes(node_id, first_seen_utc_ms,
                                    last_seen_utc_ms, revoked_at_utc_ms,
                                    revoked_reason)
                   VALUES(?,?,?,?,?)
                   ON CONFLICT(node_id) DO UPDATE SET
                       revoked_at_utc_ms=excluded.revoked_at_utc_ms,
                       revoked_reason=excluded.revoked_reason""",
                (node_id, now_ms, now_ms, now_ms, cleaned),
            )
            return {"node_id": node_id, "retired": True, "created": True,
                    "retired_at_utc_ms": now_ms, "reason": cleaned}
        conn.execute(
            """UPDATE nodes SET revoked_at_utc_ms=?, revoked_reason=?
               WHERE node_id=? AND revoked_at_utc_ms IS NULL""",
            (now_ms, cleaned, node_id),
        )
        row = conn.execute(
            "SELECT revoked_at_utc_ms FROM nodes WHERE node_id=?", (node_id,)
        ).fetchone()
        return {"node_id": node_id, "retired": True, "created": False,
                "retired_at_utc_ms": row["revoked_at_utc_ms"],
                "reason": cleaned}


def reinstate_via_provisioning(node_id: str) -> None:
    """Clears a retirement as a side effect of provisioning.

    This is the only way a node comes back, and it is deliberately coupled to
    provisioning rather than exposed as its own endpoint: coming back means being
    given a new key, and an endpoint that only flipped a flag would restore an
    identity without changing the thing that was compromised.
    """
    with transaction() as conn:
        conn.execute(
            """UPDATE nodes SET revoked_at_utc_ms=NULL, revoked_reason=NULL
               WHERE node_id=?""",
            (node_id,),
        )


def require_not_retired(node_id: str) -> None:
    """Raises `NodeRetired` when the node's identity has been retired."""
    retired_at, reason = retirement_state(node_id)
    if retired_at is not None:
        raise NodeRetired(node_id, retired_at, reason)


def _now_ms() -> int:
    import time

    return int(time.time() * 1000)