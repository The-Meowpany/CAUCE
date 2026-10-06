from __future__ import annotations

import base64
import hashlib
import hmac
import re
import time
from typing import Annotated

from fastapi import APIRouter, Header, HTTPException, Request

from . import node_auth, revocation
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
from .security import require_bearer_token, require_scope, require_site_access
from .signing import (
    DEFAULT_ALGORITHM,
    ED25519,
    ED25519_PUBLIC_KEY_BYTES,
    SignatureError,
    decode_key_material,
    get_algorithm,
)

router = APIRouter(prefix="/v1")

# Identifiers that arrive from a node and are stored verbatim. Letters, digits, underscore,
# dot and dash, bounded length: enough for every variable and sensor name the protocol has,
# and incapable of carrying markup, a quote, a backslash or a `</script>`.
#
# The dashboard renders these, so this is the input half of an XSS fix whose output half is
# dashboard._json_for_script and dashboard._form_value. Both halves exist because either
# alone is a condition on everything else staying correct.
NAME_PATTERN = re.compile(r"[A-Za-z0-9_.-]{1,64}")

# The validation itself lives in `identifiers`, shared with the LoRa relay and with
# `variable`/`sensor_id`. It used to live here, which meant the relay - a physical device on a
# radio, not something behind the admin token - had no way to reach it without importing this
# module, and so had no check. `NAME_PATTERN` is re-exported because tests and the sync
# handler read it from here.
from .identifiers import require_name, require_node_id  # noqa: E402

# Every quality value the firmware can put on the wire, taken from
# `cauce::core::qualityName` in firmware/lib/cauce_core/src/Types.cpp rather than from
# memory. A node that sends anything else is either a different firmware or an attack, and
# both are worth a 422 rather than a row in the database.
#
# Distinct from coverage.USABLE_QUALITIES, which answers a different question: which of
# these count towards coverage. INVALID and MISSING are perfectly legitimate values that
# that set deliberately excludes. Conflating the two would have refused honest data.
FIRMWARE_QUALITIES = frozenset({
    "VALID", "CALIBRATED", "UNCALIBRATED", "ESTIMATED",
    "SUSPECT", "INVALID", "MISSING", "UNKNOWN",
})

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
    require_scope(authorization, "write")
    node_id = require_node_id(payload.get("node_id"))
    device_key = payload.get("device_key")

    algorithm_name = payload.get("device_key_algorithm") or DEFAULT_ALGORITHM
    try:
        algorithm = get_algorithm(algorithm_name)
    except SignatureError as exc:
        raise HTTPException(status_code=422,
                            detail="unknown_signature_algorithm") from exc

    if algorithm.name == ED25519:
        # An Ed25519 "device key" stored by the central is a public key. It has
        # to be exactly 32 bytes: any other length is a truncated paste or a
        # seed pasted where a public key belongs, and accepting it would store
        # something that can never verify.
        try:
            decode_key_material(device_key, ED25519_PUBLIC_KEY_BYTES)
        except SignatureError as exc:
            raise HTTPException(
                status_code=422,
                detail="invalid_ed25519_public_key") from exc
    elif not isinstance(device_key, str) or len(device_key) < 16:
        raise HTTPException(status_code=422, detail="weak_device_key")

    with transaction() as conn:
        conn.execute(
            """INSERT INTO nodes(node_id, device_key, device_key_algorithm,
                                  first_seen_utc_ms, last_seen_utc_ms)
               VALUES(?,?,?,?,?)
               ON CONFLICT(node_id) DO UPDATE SET
                   device_key=excluded.device_key,
                   device_key_algorithm=excluded.device_key_algorithm""",
            (node_id, device_key, algorithm.name, int(time.time() * 1000),
             int(time.time() * 1000)),
        )

    # Provisioning is the only way a retired node comes back, and it is coupled to
    # the new key rather than exposed as an un-revoke. A retired node that could be
    # reinstated by flipping a flag would be reinstated by whoever retired it.
    was_retired = revocation.is_retired(node_id)
    if was_retired:
        revocation.reinstate_via_provisioning(node_id)

    return {
        "status": "provisioned",
        "node_id": node_id,
        "device_key_algorithm": algorithm.name,
        "signature_bytes": algorithm.signature_bytes,
        # Said out loud, because provisioning a retired node is how a rotation is
        # performed and an operator should not have to read the source to find out.
        "reinstated": was_retired,
    }


@router.post("/nodes/{node_id}/revoke")
def revoke_node(
    node_id: str,
    payload: dict | None = None,
    authorization: str | None = Header(default=None),
) -> dict:
    """Retires a node's identity.

    Admin, not write: retiring a device removes its ability to report, which is a
    bigger hammer than changing a setting, and a token scoped to `write` should not
    reach it.

    Idempotent, and it records the retirement even for a node id that was never
    provisioned. A retirement that silently does nothing because the id was typed
    wrong is the failure mode that matters here: the operator walks away believing
    the device is off the network.
    """
    require_scope(authorization, "admin")
    reason = (payload or {}).get("reason")
    if reason is not None and not isinstance(reason, str):
        raise HTTPException(status_code=422, detail="reason_must_be_a_string")
    return revocation.retire(node_id, reason)


@router.get("/nodes/{node_id}/revocation")
def get_node_revocation(
    node_id: str,
    authorization: str | None = Header(default=None),
) -> dict:
    """Whether a node's identity is retired. Read scope, like any other node read."""
    require_site_access(require_scope(authorization, "read"), None)
    retired_at, reason = revocation.retirement_state(node_id)
    return {
        "node_id": node_id,
        "retired": retired_at is not None,
        "retired_at_utc_ms": retired_at,
        "reason": reason,
        # There is no un-revoke endpoint on purpose. Reinstating a node means
        # provisioning it again with a new key, and this field says so rather than
        # leaving the reader to assume a flag can be flipped.
        "reinstate_by": "POST /v1/provision",
    }


@router.post("/sync/challenge")
def issue_sync_challenge(
    payload: dict,
    request: Request,
    authorization: str | None = Header(default=None),
) -> dict:
    """Issues a single-use nonce for certificate authentication.

    Unauthenticated by design, because a node cannot authenticate until it has a challenge.
    The cost of that is an open endpoint, and it is bounded three ways: the store caps how
    many outstanding challenges exist and evicts the oldest rather than refusing, the nonce
    lives 60 seconds, and answering one requires the node's private key, which nobody gets
    from this endpoint.

    A challenge is issued for a node that exists and is not retired. Refusing here rather
    than at answer time would turn this into a way to enumerate provisioned nodes, and it is
    rate limited like everything else under `/v1`.
    """
    _check_rate(request)
    node_id = require_node_id((payload or {}).get("node_id"))

    if not settings.ca_private_key:
        raise HTTPException(status_code=503,
                            detail="certificate_authority_not_configured")

    rows = query(
        "SELECT device_key, device_key_algorithm, revoked_at_utc_ms FROM nodes"
        " WHERE node_id=?",
        (node_id,),
    )
    if not rows:
        raise HTTPException(status_code=404, detail="node_not_found")
    if rows[0]["revoked_at_utc_ms"] is not None:
        raise HTTPException(status_code=403, detail="node_retired")
    if rows[0]["device_key_algorithm"] != ED25519:
        # Only a node with a public key can answer a challenge. Saying so is more useful than
        # handing out a nonce that could never succeed, and the node's own algorithm is not a
        # secret.
        raise HTTPException(status_code=409,
                            detail="node_key_algorithm_cannot_answer_a_challenge")
    if not rows[0]["device_key"]:
        raise HTTPException(status_code=409, detail="node_not_provisioned")

    challenge = node_auth.CHALLENGES.issue(node_id)
    return {
        "node_id": node_id,
        "nonce": challenge.nonce,
        "expires_utc_ms": challenge.expires_utc_ms,
        # The exact bytes to sign, so the firmware does not have to reimplement the encoding.
        # The encoding is pinned by `test_the_canonical_challenge_is_stable`, and publishing
        # it is not a secret: the node could compute it from the three fields above.
        "sign_this": node_auth.canonical_challenge(challenge, node_id).decode("utf-8"),
        "algorithm": ED25519,
        "note": (
            "single use; a rejected attempt still consumes the nonce, so fetch another"
        ),
    }


def _authenticate_by_certificate(node_id_claim, certificate_b64, nonce,
                                 nonce_signature, registered_device_key) -> None:
    """Authenticates a node by certificate, or raises 401.

    The nonce is consumed by `verify_node_certificate_auth` before the certificate is
    examined, which is what makes a failed attempt non-retryable: a caller cannot make a node
    burn unlimited nonces, and a captured request cannot be replayed. That is why a rejected
    attempt still costs the caller a challenge, and it is a deliberate trade - a legitimate
    node whose certificate is misconfigured has to fetch another.
    """
    import base64 as _b64
    import json as _json
    import logging

    from . import certificates as certificates_mod
    from .node_auth import AuthError, verify_node_certificate_auth

    if not settings.ca_private_key:
        raise HTTPException(status_code=503,
                            detail="certificate_authority_not_configured")
    if not certificate_b64 or not nonce or not nonce_signature:
        logging.warning("sync: certificate auth with incomplete headers")
        raise HTTPException(status_code=401,
                            detail="certificate_authentication_incomplete")

    try:
        certificate = _json.loads(_b64.b64decode(certificate_b64, validate=True))
    except Exception:
        logging.warning("sync: certificate was not decodable")
        raise HTTPException(status_code=401, detail="invalid_certificate") from None

    authority = certificates_mod.CertificateAuthority(settings.ca_private_key)
    try:
        verify_node_certificate_auth(
            certificate=certificate,
            signature_b64=nonce_signature,
            nonce=nonce,
            node_id=node_id_claim,
            ca_public_key_hex=authority.public_key_hex,
            registered_public_key_hex=registered_device_key,
        )
    except AuthError as exc:
        # The reason goes to the log; the caller gets one opaque code. Returning the specific
        # reason would help an attacker distinguish an expired certificate from a wrong key,
        # and it tells a legitimate operator nothing they cannot read in the central's log.
        logging.warning(f"sync: certificate auth failed for {node_id_claim}: {exc}")
        raise HTTPException(status_code=401,
                            detail="certificate_authentication_failed") from None


@router.post("/sync")
async def sync_batch(
    request: Request,
    authorization: str | None = Header(default=None),
x_cauce_node: str | None = Header(default=None),
    x_cauce_signature: str | None = Header(default=None),
    x_cauce_certificate: str | None = Header(default=None),
    x_cauce_nonce: str | None = Header(default=None),
    x_cauce_nonce_signature: str | None = Header(default=None),
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

    if device_key and not isinstance(payload.get("frames"), list):
        # Provisioned node, direct Wi-Fi path.
        #
        # Two schemes, and which one applies is decided by what the request presents rather
        # than by a flag the caller sets. A node holding a certificate proves possession of its
        # private key; a node holding only an HMAC key keeps the HMAC path, because every node
        # provisioned before certificates existed has one and must keep working.
        #
        # Certificate authentication does NOT fall back to HMAC. A fallback would leave the
        # weaker scheme permanently available and make the stronger one decorative: an
        # attacker holding one node's HMAC key would present it and skip the certificate
        # entirely. So presenting a certificate means the certificate must work.
        presented_certificate = bool(x_cauce_certificate)
        if presented_certificate:
            _authenticate_by_certificate(
                node_id_claim, x_cauce_certificate, x_cauce_nonce,
                x_cauce_nonce_signature, device_key,
            )
        else:
            # HMAC over the raw body, and the identity header must match the payload's
            # node_id. A relayed narrowband batch is the exception: it carries no body
            # signature because the relay does not hold the key. Its frames are signed by the
            # node and verified one by one in `_expand_signed_frames`.
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

    # Retirement is checked before the frame is authenticated. Checking afterwards
    # would accept the request that a node uses to report its own retirement, and
    # more importantly it would mean a retired identity still gets to spend the
    # central's rate-limit budget on signature verification.
    try:
        revocation.require_not_retired(node_id)
    except revocation.NodeRetired as exc:
        raise HTTPException(
            status_code=403,
            detail={
                "error": "node_retired",
                "retired_at_utc_ms": exc.retired_at_ms,
                "reason": exc.reason,
            },
        ) from exc

    # A relayed narrowband batch arrives as the frames the node signed, not as
    # reconstructed JSON. The relay never holds the node's key, so this is the
    # only place the signature can be checked.
    if isinstance(payload.get("frames"), list):
        from .lora_ingest import expand_signed_frames

        payload = expand_signed_frames(payload)

    measurements = payload.get("measurements")
    if not isinstance(measurements, list):
        raise HTTPException(status_code=422, detail="missing_measurements")

    # `node_id` is stored, rendered, exported and used as a filename, and it was the one
    # identifier on this path with no shape check at all - `variable` and `sensor_id` both
    # had one, `node_id` was only required to be a non-empty string.
    #
    # What that allowed, confirmed against HEAD before fixing: a node could sync as
    # `=cmd|'/C calc'!A0` and the central answered 200. The identifier then reached
    # `/v1/export-all.csv` unquoted in the first column, so an operator who opened the export
    # in a spreadsheet evaluated the operator's formula rather than reading a node id. That is
    # CSV formula injection, and it is a stored issue: it fires on whoever opens the file
    # next, which is nobody who can see the cause.
    #
    # `require_node_id` and the reasoning behind the character class are in `identifiers`.
    node_id = require_node_id(payload.get("node_id"))

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
        # `variable` and `sensor_id` are stored verbatim and rendered by the dashboard, so
        # their shape is part of the trust boundary, not cosmetic tidiness. Before this, a
        # node could sync a variable called `</script><script>alert(1)</script>` and it came
        # back out of the central's own page as markup.
        #
        # A character class rather than an allowlist of known variables, because the set of
        # variables is meant to grow: the protocol version, not this regex, decides what is
        # understood. Mirrors SITE_ID_PATTERN in evaluation.py.
        for field in ("variable", "sensor_id"):
            require_name(rec.get(field), field, optional=(field == "sensor_id"))
        # `quality` is the third field that reaches the dashboard unescaped, in a class
        # attribute this time: `class="q-{quality}"`. A node could sync
        # `x" onmouseover="alert(1)` and the central's node page returned
        # `<td class="q-x" onmouseover="alert(1)">`, which fires without a click.
        #
        # Unlike variable and sensor_id, this one has a closed set rather than a shape, so
        # membership is the check. `estimated` and `unknown` are in the firmware enum and
        # were missing from an earlier version of this list, which would have refused
        # honest readings from a node that sends them.
        if rec.get("quality") not in FIRMWARE_QUALITIES:
            raise HTTPException(status_code=422, detail="invalid_quality")

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
            value = rec["value"]
            sumsq = value ** 2

            # 15-minute bucket, then hourly. Both are fed from the raw row
            # rather than derived from each other: an hour's aggregates do not
            # say how the hour was distributed inside it.
            bucket_ts = (rec["timestamp_utc_ms"] // 900000) * 900000
            conn.execute(
                """INSERT INTO agg_15min(node_id,variable,bucket_ts,cnt,sum,sumsq,min_v,max_v)
                   VALUES(?,?,?,?,?,?,?,?)
                   ON CONFLICT(node_id,variable,bucket_ts) DO UPDATE SET
                     cnt = cnt + excluded.cnt,
                     sum = sum + excluded.sum,
                     sumsq = sumsq + excluded.sumsq,
                     min_v = MIN(min_v, excluded.min_v),
                     max_v = MAX(max_v, excluded.max_v)""",
                (node_id, rec["variable"], bucket_ts, 1, value, sumsq,
                 value, value),
            )

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
                (node_id, rec["variable"], hour_ts, 1, value, sumsq,
                 value, value),
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

    # Downlink rides the same round trip the node already makes, so it costs no
    # extra radio wakeup. Receipts first, so a command acknowledged in this very
    # response cannot be handed back again.
    from .commands import pending_commands, record_receipts

    receipts = payload.get("command_receipts") or []
    if not isinstance(receipts, list):
        raise HTTPException(status_code=422, detail="invalid_command_receipts")
    if receipts:
        record_receipts(node_id, receipts)

    commands = [
        {
            "command_id": c["command_id"],
            "kind": c["kind"],
            "payload": c["payload"],
        }
        for c in pending_commands(node_id)
    ]

    return {
        "acknowledged_sequence": acked if acked is not None else 0,
        "received": len(measurements),
        "commands": commands,
    }


@router.get("/ota/manifest")
def ota_manifest(
    request: Request,
    node_id: str | None = None,
    authorization: str | None = Header(default=None),
) -> dict:
    """The release descriptor a node updates from.

    Authenticated, which it was not for its whole life: the handler took no
    `authorization` at all, so it ignored `CAUCE_API_TOKEN` even when one was configured.
    Every other admin surface checks it. That made this the one endpoint where an operator
    who had deliberately locked the central down still published the exact version, URL,
    size and per-node HMAC of the next firmware to anyone who asked.

    `require_bearer_token` rather than `require_admin_write`: this is a read, and reads stay
    open when no token is configured so the dashboard works on a trusted LAN. Setting the
    token closes it, which is the same rule every other admin read follows.
    """
    _check_rate(request)
    require_bearer_token(authorization, settings.api_token)
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
            # The canonical string and the key derivation must be byte-identical to
            # OtaManager::runCheck in the firmware, or the node rejects every manifest.
            #
            # It was not. The firmware keyed the HMAC with SHA-256(device_key) as 32 raw
            # bytes and signed "version|sha256|url|total_size"; this side keyed it with the
            # ASCII device_key and signed "version|url|total_size". Two independent
            # mismatches, so no signature produced here could ever have been accepted by a
            # node. It failed in the safe direction - the node refuses the update - which is
            # why it went unnoticed, and also why the manifest gate had never actually
            # protected anything: it had never let an update through either.
            #
            # `version|sha256|url|total_size` rather than a shorter canonical, because the
            # signature has to commit to the image hash. Signing version, URL and size
            # alone would let a rewritten `sha256` field ride along under a signature that
            # still verifies.
            canonical = f"{version}|{sha256}|{url}|{total_size}"
            manifest_key = hashlib.sha256(
                rows[0]["device_key"].encode("utf-8")).digest()
            signature = hmac.new(manifest_key, canonical.encode("utf-8"),
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
    require_scope(authorization, "read")
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


def _site_of_node(node_id: str) -> str | None:
    """The site a node belongs to, for site-scoped principals."""
    rows = query("SELECT site_id FROM nodes WHERE node_id=?", (node_id,))
    return rows[0]["site_id"] if rows else None

@router.get("/nodes/{node_id}")
def get_node(node_id: str, request: Request, authorization: str | None = Header(default=None)) -> dict:
    _check_rate(request)
    require_site_access(require_scope(authorization, "read"),
                        _site_of_node(node_id))
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
    require_site_access(require_scope(authorization, "read"),
                        _site_of_node(node_id))
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
    require_scope(authorization, "admin")
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
    require_scope(authorization, "admin")
    days = payload.get("older_than_days", 90)
    if not isinstance(days, int) or not 1 <= days <= 3650:
        raise HTTPException(status_code=422, detail="invalid_retention_days")
    state = purge_older_than(days)
    # Every purgeable table is reported, not just the two that existed when
    # this endpoint was written. An operator who runs retention by hand needs to
    # see what it actually removed, and `deleted_buckets` is kept as an alias
    # for the original hourly counter so nothing reading it breaks.
    counters = {
        key: int(value)
        for key, value in state.items()
        if key.startswith("deleted_")
    }
    counters["deleted_buckets"] = counters.get("deleted_hourly_buckets", 0)
    return {
        "status": "retained",
        "older_than_days": days,
        "vacuumed": bool(state.get("vacuumed")),
        **counters,
    }


@router.get("/maintenance/retention")
def retention_state(
    request: Request,
    authorization: str | None = Header(default=None),
) -> dict:
    _check_rate(request)
    require_scope(authorization, "read")
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
    require_scope(authorization, "read")
    if granularity not in ("auto", "raw", "15min", "hourly", "daily"):
        raise HTTPException(status_code=422, detail="invalid_granularity")
    calibration = calibration_for(node_id, variable)

    span_ms = None
    if from_utc_ms is not None and to_utc_ms is not None:
        span_ms = to_utc_ms - from_utc_ms
    elif to_utc_ms is not None:
        span_ms = to_utc_ms - (from_utc_ms or 0)

    use_daily = granularity == "daily" or (
        granularity == "auto" and span_ms is not None
        and span_ms >= DAILY_MIN_SPAN_MS
        and _daily_coverage_ok(node_id, variable)
    )
    use_hourly = not use_daily and (
        granularity == "hourly" or (
            granularity == "auto" and span_ms is not None
            and span_ms >= HOURLY_MIN_SPAN_MS
            and _hourly_coverage_ok(node_id, variable, from_utc_ms, to_utc_ms)
        )
    )
    # Between raw and hourly. `auto` takes it only for a window long enough that
    # reading raw is wasteful but short enough that hourly would hide the shape
    # of the period asked about.
    use_q15 = not use_daily and not use_hourly and (
        granularity == "15min" or (
            granularity == "auto" and span_ms is not None
            and Q15_MIN_SPAN_MS <= span_ms <= Q15_MAX_SPAN_MS
            and _q15_coverage_ok(node_id, variable)
        )
    )
    if use_q15:
        raw_q15 = _q15_stats(node_id, variable, from_utc_ms, to_utc_ms)
        calibrated_q15 = transform_stats(raw_q15, calibration)
        return {
            "node_id": node_id,
            "variable": variable,
            "metric_type": "derived",
            "source": "materialized_15min",
            "granularity": "15min",
            "note": (
                "descriptive statistics only; does not imply causality. "
                "15-minute buckets: sub-quarter-hour variation is averaged away. "
                "use granularity=raw to see every sample"
            ),
            "calibration": calibration_summary(calibration),
            **({"calibrated": calibrated_q15} if calibration else {}),
            **raw_q15,
        }
    if use_daily:
        raw_daily = _daily_stats(node_id, variable, from_utc_ms, to_utc_ms)
        calibrated_daily = transform_stats(raw_daily, calibration)
        return {
            "node_id": node_id,
            "variable": variable,
            "metric_type": "derived",
            "source": "materialized_daily",
            "granularity": "daily",
            "note": (
                "descriptive statistics only; does not imply causality. "
                "daily buckets: every intra-day peak and trough is invisible. "
                "use granularity=hourly or raw to see within-day behaviour"
            ),
            "calibration": calibration_summary(calibration),
            **({"calibrated": calibrated_daily} if calibration else {}),
            **raw_daily,
        }
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
DAY_MS = 24 * HOUR_MS
# Below a week the raw rows are cheap to read and give exact extremes, so
# `auto` stays on raw. Above it the hourly buckets are the cheaper answer and
# the response says which one it used.
Q15_MS = 15 * 60 * 1000
# Below a day there are enough raw rows to be worth folding, and not enough
# hours for an hourly bucket to say anything about the shape of the period.
Q15_MIN_SPAN_MS = 2 * HOUR_MS
# Above two days, 15-minute buckets stop being a saving: two days is 192 buckets
# against at most 2880 hourly... and an hourly table is only eligible past seven
# days, so the window between the two ceilings reads raw. Deliberate. An
# aggregate that returns more rows than the raw table it replaced is not an
# optimisation, and this tier only earns its place where it is genuinely fewer.
Q15_MAX_SPAN_MS = 2 * 24 * HOUR_MS
HOURLY_MIN_SPAN_MS = 7 * 24 * HOUR_MS
# Past a season the question is a trend, not an extreme, and daily buckets
# turn ~500 hourly rows into ~90. Set high on purpose: a daily bucket hides
# every intra-day peak, so it is only a fair answer when nobody is asking for
# one.
DAILY_MIN_SPAN_MS = 120 * 24 * HOUR_MS


def _q15_stats(node_id: str, variable: str, from_utc_ms: int | None,
               to_utc_ms: int | None) -> dict:
    sql = ("SELECT bucket_ts, cnt, sum, sumsq, min_v, max_v"
           " FROM agg_15min WHERE node_id=? AND variable=?")
    params: list = [node_id, variable]
    if from_utc_ms is not None:
        sql += " AND bucket_ts>=?"
        params.append((from_utc_ms // Q15_MS) * Q15_MS)
    if to_utc_ms is not None:
        sql += " AND bucket_ts<=?"
        params.append((to_utc_ms // Q15_MS) * Q15_MS)
    sql += " ORDER BY bucket_ts"
    return _agg_stats(query(sql, tuple(params)), "15min")


def _q15_coverage_ok(node_id: str, variable: str) -> bool:
    """Whether the 15-minute table holds enough of this variable to use.

    The same 90% rule the hourly and daily paths use: a materialised aggregate
    that is silently missing samples reports a mean over a subset, which looks
    exactly like a real change in the data.
    """
    rows = query(
        "SELECT COUNT(*) AS buckets, COALESCE(SUM(cnt),0) AS samples"
        " FROM agg_15min WHERE node_id=? AND variable=?",
        (node_id, variable),
    )[0]
    if rows["buckets"] == 0:
        return False
    raw = query(
        "SELECT COUNT(*) AS c FROM measurements WHERE node_id=? AND variable=?",
        (node_id, variable),
    )[0]["c"]
    return raw == 0 or rows["samples"] >= raw * 0.9


def _daily_stats(node_id: str, variable: str, from_utc_ms: int | None,
                 to_utc_ms: int | None) -> dict:
    sql = ("SELECT day_ts AS bucket_ts, cnt, sum, sumsq, min_v, max_v"
           " FROM agg_daily WHERE node_id=? AND variable=?")
    params: list = [node_id, variable]
    if from_utc_ms is not None:
        sql += " AND day_ts>=?"
        params.append((from_utc_ms // DAY_MS) * DAY_MS)
    if to_utc_ms is not None:
        sql += " AND day_ts<=?"
        params.append((to_utc_ms // DAY_MS) * DAY_MS)
    sql += " ORDER BY day_ts"
    return _agg_stats(query(sql, tuple(params)), "daily")


def _daily_coverage_ok(node_id: str, variable: str) -> bool:
    rows = query(
        "SELECT COUNT(*) AS buckets, COALESCE(SUM(cnt),0) AS samples"
        " FROM agg_daily WHERE node_id=? AND variable=?",
        (node_id, variable),
    )[0]
    if rows["buckets"] == 0:
        return False
    raw = query(
        "SELECT COUNT(*) AS c FROM measurements WHERE node_id=? AND variable=?",
        (node_id, variable),
    )[0]["c"]
    return raw == 0 or rows["samples"] >= raw * 0.9


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
    return _agg_stats(query(sql, tuple(params)), "hourly")


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
    principal = require_scope(authorization, "read")
    for _peer in (node_a, node_b):
        require_site_access(principal, _site_of_node(_peer))
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
    authorization: Annotated[str | None, Header()] = None,
) -> dict:
    # Annotated rather than `= Header(default=None)`: the dashboard calls this function
    # directly instead of going through FastAPI, and with the plain Header default the
    # callee received a Header object where a string was expected. The default of an
    # Annotated parameter is a real None, so a direct call is simply unauthenticated and
    # the read stays open when no token is configured.
    _check_rate(request)
    require_scope(authorization, "read")
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
    require_scope(authorization, "read")
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


def _agg_stats(rows, granularity: str = "hourly") -> dict:
    import math
    cnt = sum(r["cnt"] for r in rows)
    if cnt == 0:
        return {"count": 0, "granularity": granularity}
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
        "granularity": granularity,
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
    require_scope(authorization, "read")
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
    require_site_access(require_scope(authorization, "write"),
                        _site_of_node(node_id))

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

