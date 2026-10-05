"""Node certificates: binding an Ed25519 public key to a node identity.

THE GAP THIS CLOSES

`POST /v1/provision` stores an Ed25519 public key against a `node_id`. That key then
verifies the node's frame signatures, so a node proves possession of the private key. What
nothing established was that the *central* vouched for the binding. `nodes.device_key` is a
row an operator wrote with the admin token, and the trust story ends there:

- the binding is asserted by whoever holds the admin token, with no separate authority, so
  rotating one node's key and adding a new node are the same operation and the same blast
  radius;
- a node's key is not attributable. Every node signs with a distinct key, but the central
  holds them as opaque text in one column with nothing recording who issued each;
- there is no expiry, so a key compromised once is a key compromised until somebody notices
  and edits a row.

This adds the missing layer: a CA key, held separately from the admin token, that signs a
certificate binding node identity to public key with an expiry. A verifier then needs the
CA public key alone, which is the property that matters - it can check a node without
holding the admin token and without a database lookup per frame.

WHAT IS AND IS NOT CLAIMED

This is **not** X.509. There is no ASN.1, no chain, no name constraints. It is one
certificate, signed by one CA, with a fixed shape. Calling it PKI without that
qualification would overstate it in the way a project usually overstates it: by implying
an interoperability story nobody can rely on.

The parts that are real:

- the signature is Ed25519 over a canonical byte string, so the binding is verifiable by
  anyone holding the CA public key;
- `not_before` and `not_after` are checked, and an expired certificate is rejected rather
  than warned about;
- serial numbers are unique, so revoking one certificate cannot accidentally revoke
  another;
- the CA private key is **never** returned by any endpoint, and its public key is what
  ships to verifiers.

WHAT IS DELIBERATELY ABSENT

No key rollover for the CA itself, because a CA that can rotate its own key without
changing what verifiers trust is not rotating, it is re-signing. Rotation means changing
the public key every verifier holds, and that is a deployment decision this does not make
on its own. See `docs/en/SECURITY.md`.
"""

from __future__ import annotations

import json
import secrets
import time

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives.asymmetric.ed25519 import (
    Ed25519PrivateKey,
    Ed25519PublicKey,
)

from .signing import (
    ED25519_PRIVATE_KEY_BYTES,
    ED25519_PUBLIC_KEY_BYTES,
    SignatureError,
    decode_key_material,
)

# The version travels in the certificate body so a future shape can be refused rather than
# misparsed. A parser that ignores it would accept a v2 body and read the fields it happens
# to recognise.
CERTIFICATE_VERSION = 1

CERTIFICATE_KIND = "cauce-node-certificate"

# Refused rather than clamped. A certificate outlives the deployment that issued it, and
# an operator who asks for ten years has usually typed the wrong number.
MAX_VALIDITY_SECONDS = 366 * 24 * 3600


class CertificateError(ValueError):
    """A certificate that cannot be issued or cannot be trusted."""


# Ed25519 curve arithmetic, for the one check the crypto library does not do.
#
# `Ed25519PublicKey.from_public_bytes` accepts any 32 bytes whatsoever: it stores them and
# waits to fail at verification, when it tries to decompress. So a certificate issued for a
# 32-byte value that is not a curve point looks perfectly well formed at every point up to
# and including issuance, and the failure surfaces as a signature that never verifies - at
# the far end, on a node, with the CA looking innocent.
#
# Whether a value is a curve point is decidable cheaply, so it is decided here. A group of
# order p over F_p has a point with the given y exactly when x^2 = (y^2-1)/(dy^2+1) is a
# quadratic residue, which is the Euler criterion below. This is 20 lines of arithmetic to
# turn "never works, much later" into "refused, now".
_ED25519_P = (2 ** 255) - 19
_ED25519_D = (-121665 * pow(121666, _ED25519_P - 2, _ED25519_P)) % _ED25519_P


def is_on_curve(public_raw: bytes) -> bool:
    """Whether 32 bytes decode to a point of the Ed25519 curve.

    Rejects two distinct things, both of which a key length check misses:

    - a non-canonical `y`, one that is >= p once the sign bit is stripped. Accepting it
      would give two encodings of one point and therefore two public keys that are the same
      key, which is the same malleability problem canonical point encoding exists to prevent.
    - a `y` with no corresponding `x`, about half of all values. Verification against one of
      these can never succeed.
    """
    if len(public_raw) != ED25519_PUBLIC_KEY_BYTES:
        return False
    y = int.from_bytes(public_raw, "little") & ((1 << 255) - 1)
    if y >= _ED25519_P:
        return False
    y_squared = (y * y) % _ED25519_P
    denominator = (_ED25519_D * y_squared + 1) % _ED25519_P
    if denominator == 0:
        return False
    # x^2 = (y^2 - 1) / (d*y^2 + 1); modular division is multiplication by the inverse.
    x_squared = ((y_squared - 1) * pow(denominator, _ED25519_P - 2, _ED25519_P)) % _ED25519_P
    if x_squared == 0:
        return True
    # Euler's criterion: a non-zero element of F_p is a square iff its (p-1)/2 power is 1.
    return pow(x_squared, (_ED25519_P - 1) // 2, _ED25519_P) == 1


def canonical_body(certificate: dict) -> bytes:
    """The exact bytes a signature covers.

    Sorted keys, no whitespace, UTF-8. Every detail here is load-bearing:

    - `sort_keys` so two certificates for the same node serialise identically and one
      signature can be recomputed;
    - `separators` so there is exactly one valid encoding of a given body, rather than one
      per whitespace combination;
    - the signature and the CA key are *excluded*, because they are what is added to the
      body rather than part of it. Including them would be circular.

    The firmware has to reproduce this byte for byte to verify a certificate without
    trusting the transport, so the encoding is pinned on both sides by a shared
    known-answer test.
    """
    body = {k: v for k, v in certificate.items()
            if k not in ("signature", "ca_public_key")}
    return json.dumps(
        body, sort_keys=True, separators=(",", ":"), ensure_ascii=False,
    ).encode("utf-8")


class CertificateAuthority:
    """Issues and verifies node certificates.

    The CA private key is passed in and kept for the lifetime of the instance. It is never
    exposed by `public_key`, and nothing in this module returns it.
    """

    def __init__(self, private_key_hex: str):
        raw = decode_key_material(private_key_hex, ED25519_PRIVATE_KEY_BYTES)
        self._private = Ed25519PrivateKey.from_private_bytes(raw)
        self._public = self._private.public_key()

    @property
    def public_key_hex(self) -> str:
        """What a verifier holds. Safe to publish; this is the whole point of a CA key."""
        return self._public.public_bytes_raw().hex()

    def issue(
        self,
        node_id: str,
        public_key_hex: str,
        site_id: str | None = None,
        validity_seconds: int = 90 * 24 * 3600,
        now_ms: int | None = None,
    ) -> dict:
        """Signs a certificate binding `node_id` to `public_key_hex`.

        The public key is validated here rather than trusted, because a certificate that
        binds a node to a 31-byte key is worthless and would only fail later at a
        verifier, which is the worst place to find out.
        """
        if not isinstance(node_id, str) or not node_id.strip():
            raise CertificateError("node_id is required")
        try:
            public_raw = decode_key_material(public_key_hex, ED25519_PUBLIC_KEY_BYTES)
        except SignatureError as exc:
            raise CertificateError(
                "public key must be 32 bytes of Ed25519 key material") from exc
# The crypto library will happily accept any 32 bytes, so the point check is ours.
        if not is_on_curve(public_raw):
            raise CertificateError(
                "public key is not a valid Ed25519 curve point; it could never "
                "verify a signature, so no certificate is issued for it"
            )

        if not isinstance(validity_seconds, int) or validity_seconds <= 0:
            raise CertificateError("validity must be a positive number of seconds")
        if validity_seconds > MAX_VALIDITY_SECONDS:
            raise CertificateError(
                f"validity above {MAX_VALIDITY_SECONDS}s is refused; "
                "issue short certificates and renew deliberately"
            )

        issued = int(now_ms if now_ms is not None else time.time() * 1000)
        body = {
            "version": CERTIFICATE_VERSION,
            "kind": CERTIFICATE_KIND,
            "serial": secrets.token_hex(16),
            "node_id": node_id.strip(),
            "site_id": site_id,
            "algorithm": "ed25519",
            "public_key": public_raw.hex(),
            "not_before_utc_ms": issued,
            "not_after_utc_ms": issued + validity_seconds * 1000,
        }
        certificate = dict(body)
        certificate["signature"] = self._private.sign(canonical_body(body)).hex()
        certificate["ca_public_key"] = self.public_key_hex
        return certificate


def verify_certificate(
    certificate: dict,
    ca_public_key_hex: str,
    now_ms: int | None = None,
) -> tuple[bool, str]:
    """Checks a certificate against a CA public key.

    Returns `(ok, reason)`. The reason is a machine-readable string, not a message meant
    for a person: it goes into a log and a response body where a caller may act on it, and
    a distinguishable failure per field is not an oracle here because the certificate is the
    caller's own document rather than something being probed.

    Deliberately does not distinguish "bad signature" from "malformed body" to the caller
    beyond the reason string's own value; both mean the same thing operationally.
    """
    if not isinstance(certificate, dict):
        return False, "not_an_object"
    version = certificate.get("version")
    if version != CERTIFICATE_VERSION:
        return False, "unsupported_version"
    if certificate.get("kind") != CERTIFICATE_KIND:
        return False, "wrong_kind"
    if certificate.get("algorithm") != "ed25519":
        return False, "unsupported_algorithm"

    node_id = certificate.get("node_id")
    if not isinstance(node_id, str) or not node_id.strip():
        return False, "missing_node_id"

    signature_hex = certificate.get("signature")
    if not isinstance(signature_hex, str) or not signature_hex:
        return False, "missing_signature"

    try:
        ca_raw = decode_key_material(ca_public_key_hex, ED25519_PUBLIC_KEY_BYTES)
        ca = Ed25519PublicKey.from_public_bytes(ca_raw)
        signature = bytes.fromhex(signature_hex)
    except (SignatureError, ValueError):
        return False, "bad_ca_key"
    if len(signature) != 64:
        return False, "bad_signature_length"

    try:
        ca.verify(signature, canonical_body(certificate))
    except InvalidSignature:
        return False, "bad_signature"
    except (ValueError, TypeError):
        return False, "malformed_body"

    moment = int(now_ms if now_ms is not None else time.time() * 1000)
    not_before = certificate.get("not_before_utc_ms")
    not_after = certificate.get("not_after_utc_ms")
    if not isinstance(not_before, int) or not isinstance(not_after, int):
        return False, "missing_validity"
    if moment < not_before:
        return False, "not_yet_valid"
    if moment > not_after:
        return False, "expired"

    return True, "ok"


def certificate_is_valid_for_node(
    certificate: dict,
    node_id: str,
    ca_public_key_hex: str,
    now_ms: int | None = None,
) -> tuple[bool, str]:
    """Verifies, and additionally requires the certificate to name this node.

    Separated from `verify_certificate` on purpose. A valid certificate for a *different*
    node is the failure that a shared-admin-token trust model cannot distinguish from
    success, so it gets its own check rather than being folded into a boolean.
    """
    ok, reason = verify_certificate(certificate, ca_public_key_hex, now_ms)
    if not ok:
        return False, reason
    if certificate.get("node_id") != node_id:
        return False, "issued_for_another_node"
    return True, "ok"
