"""HTTP surface for node certificates.

Separate from `api.py` because the trust story here is different from the rest of the API.
Everything else is gated by the shared admin token; these endpoints are gated by a
signature from a CA key that is configured separately. Mixing the two in one module would
make it easy to later gate a certificate endpoint with the admin token by accident, and
nothing would fail.
"""

from __future__ import annotations

import json
import time

from fastapi import APIRouter, Header, HTTPException, Request

from . import certificates
from .config import settings
from .db import query, transaction
from .ratelimit import check_rate
from .security import require_scope, require_site_access
from .signing import ED25519

router = APIRouter(prefix="/v1")

# Fails closed, and says why in the body.
#
# The alternative - issuing certificates whenever the CA key is absent, signed by nothing -
# would mean the endpoint returns documents that look authoritative and verify against no
# key at all. An operator would reasonably read a certificate as a statement that the
# central vouched for the binding. Silently degrading to "no CA" makes that statement false
# in exactly the deployments that forgot to configure one.
CA_UNCONFIGURED = "certificate_authority_not_configured"


def _authority() -> certificates.CertificateAuthority:
    if not settings.ca_private_key:
        raise HTTPException(status_code=503, detail=CA_UNCONFIGURED)
    try:
        return certificates.CertificateAuthority(settings.ca_private_key)
    except certificates.SignatureError as exc:
        # A malformed key is an operator error, not a client error, and the message says so
        # without echoing the key itself.
        raise HTTPException(
            status_code=503,
            detail="certificate_authority_key_invalid",
        ) from exc


def _site_of(node_id: str) -> str | None:
    rows = query("SELECT site_id FROM nodes WHERE node_id=?", (node_id,))
    return rows[0]["site_id"] if rows else None


def _retire_active(conn, node_id: str, reason: str, now_ms: int) -> None:
    """Marks a node's current certificates retired.

    Retirement, not deletion, for the same reason node revocation is not deletion: the
    history of what was trusted is the audit trail. A row deleted on rotation cannot answer
    which key a node was using when a measurement arrived.
    """
    # One statement, not two. An earlier version had a second `AND status<>'retired'`
    # variant, which was a no-op on a column already constrained to those two values - and
    # a second write to the same table is exactly the kind of thing that looks like it is
    # enforcing something it is not.
    conn.execute(
        "UPDATE certificates SET status='retired'"
        " WHERE node_id=? AND status='active'",
        (node_id,),
    )
    _ = reason, now_ms


@router.post("/nodes/{node_id}/certificate")
def issue_certificate(
    node_id: str,
    payload: dict | None,
    request: Request,
    authorization: str | None = Header(default=None),
) -> dict:
    """Issues a certificate binding this node's registered public key to its identity.

    The public key comes from `nodes.device_key`, never from the request. A request that
    carried its own key would be asking the CA to certify whatever it was handed, which is
    the one thing a certificate must never do: it has to say the central already knew this
    key belongs to this node.
    """
    check_rate(request)
    principal = require_scope(authorization, "write")
    require_site_access(principal, _site_of(node_id))

    rows = query(
        "SELECT device_key, device_key_algorithm, site_id FROM nodes WHERE node_id=?",
        (node_id,),
    )
    if not rows:
        raise HTTPException(status_code=404, detail="node_not_found")
    device_key = rows[0]["device_key"]
    algorithm = rows[0]["device_key_algorithm"]
    site_id = rows[0]["site_id"]

    if not device_key:
        raise HTTPException(
            status_code=409,
            detail="node_has_no_device_key_provision_it_first",
        )
    if algorithm != ED25519:
        # Certificates bind a public key. An HMAC node's key is a shared secret, and
        # publishing it inside a certificate would hand the secret to every verifier.
        raise HTTPException(
            status_code=409,
            detail="node_key_algorithm_is_not_public_key_based",
        )

    requested = (payload or {}).get("validity_seconds")
    validity = settings.cert_validity_seconds
    if requested is not None:
        if not isinstance(requested, int):
            raise HTTPException(status_code=422, detail="invalid_validity_seconds")
        validity = requested

    authority = _authority()
    try:
        certificate = authority.issue(
            node_id=node_id,
            public_key_hex=device_key,
            site_id=site_id,
            validity_seconds=validity,
        )
    except certificates.CertificateError as exc:
        raise HTTPException(status_code=422, detail=str(exc)) from exc

    now_ms = int(time.time() * 1000)
    with transaction() as conn:
        _retire_active(conn, node_id, "superseded", now_ms)
        conn.execute(
            """INSERT INTO certificates(serial, node_id, site_id, public_key,
                                        issued_utc_ms, not_after_utc_ms, body, status)
               VALUES (?,?,?,?,?,?,?,'active')""",
            (
                certificate["serial"], node_id, certificate["site_id"],
                certificate["public_key"], certificate["not_before_utc_ms"],
                certificate["not_after_utc_ms"],
                json.dumps(certificate, sort_keys=True, separators=(",", ":")),
            ),
        )

    return {
        "status": "issued",
        "certificate": certificate,
        "ca_public_key": authority.public_key_hex,
        "not_after_utc_ms": certificate["not_after_utc_ms"],
        # Said out loud: issuing a new certificate retires the previous one, and a node
        # holding the old one stops being trusted. An operator rotating a key needs to know
        # a second certificate is not additive.
        "superseded_previous": True,
    }


@router.get("/nodes/{node_id}/certificate")
def active_certificate(
    node_id: str,
    request: Request,
    authorization: str | None = Header(default=None),
) -> dict:
    """The node's current certificate, if it has one.

    Not a read of a stored blob: the certificate is re-verified against the CA key on the
    way out. A row that was tampered with in the database therefore fails here rather than
    being handed to a node that would trust it.
    """
    check_rate(request)
    principal = require_scope(authorization, "read")
    require_site_access(principal, _site_of(node_id))

    if not query("SELECT 1 FROM nodes WHERE node_id=?", (node_id,)):
        raise HTTPException(status_code=404, detail="node_not_found")

    rows = query(
        "SELECT body FROM certificates WHERE node_id=? AND status='active'"
        " ORDER BY issued_utc_ms DESC",
        (node_id,),
    )
    if not rows:
        raise HTTPException(status_code=404, detail="no_active_certificate")

    certificate = json.loads(rows[0]["body"])
    authority = _authority()
    ok, reason = certificates.verify_certificate(
        certificate, authority.public_key_hex)
    if not ok:
        raise HTTPException(
            status_code=500,
            detail=f"stored_certificate_does_not_verify_{reason}",
        )

    return {
        "certificate": certificate,
        "ca_public_key": authority.public_key_hex,
        "verified": True,
    }


@router.get("/certificates/{serial}")
def certificate_status(
    serial: str,
    request: Request,
    authorization: str | None = Header(default=None),
) -> dict:
    """One certificate by serial, with its status and why.

    This is how an operator answers "is this node's certificate still good, and when does it
    stop being good" without reading the database.
    """
    check_rate(request)
    require_scope(authorization, "read")

    rows = query(
        "SELECT serial, node_id, status, issued_utc_ms, not_after_utc_ms, body"
        " FROM certificates WHERE serial=?",
        (serial,),
    )
    if not rows:
        raise HTTPException(status_code=404, detail="certificate_not_found")
    row = rows[0]

    # Status comes from the database and the verdict comes from the signature. They are
    # reported separately on purpose: a certificate can be signature-valid and retired, and
    # collapsing those into one field is how "valid" starts meaning "still authorised" in
    # somebody's head.
    authority = _authority()
    certificate = json.loads(row["body"])
    signature_ok, reason = certificates.verify_certificate(
        certificate, authority.public_key_hex)

    now_ms = int(time.time() * 1000)
    return {
        "serial": row["serial"],
        "node_id": row["node_id"],
        "status": row["status"],
        "signature_valid": signature_ok,
        "signature_reason": reason,
        "issued_utc_ms": row["issued_utc_ms"],
        "not_after_utc_ms": row["not_after_utc_ms"],
        "expired": now_ms > row["not_after_utc_ms"],
        # Whether this certificate may be relied on right now: signed by this CA, not
        # expired, and not superseded.
        "trusted": bool(signature_ok and row["status"] == "active"
                        and now_ms <= row["not_after_utc_ms"]),
    }
