"""Signature algorithms for node frames.

The central decides which algorithm a node uses from what was provisioned,
rather than from anything in the request. If the algorithm travelled with the
frame, an attacker would relabel an HMAC frame as Ed25519 and the central would
pick whichever check was cheaper to pass. Fixing it per node leaves exactly one
free choice, "none", which fails.

Two algorithms, each for a reason:

`hmac-sha256`
    What the fleet already runs. Symmetric, so anything that can verify can also
    forge. That is why the relay must never hold this key: frame signing keeps
    the number of parties holding it at one per node. No non-repudiation, and a
    node can deny its own measurements.

`ed25519`
    Asymmetric. The central stores a public key, so compromising the central
    does not let an attacker forge a node's past measurements, and a node cannot
    deny them either. That is the property HMAC structurally cannot provide, and
    it is what the larger key and 64-byte signature buy.

Signature length distinguishes the two on the wire: a 32-byte trailer is
HMAC-SHA-256, a 64-byte trailer is Ed25519. That is unambiguous given the node's
provisioned algorithm, which is the only thing that matters, and it let Ed25519
be added without touching the frame header the firmware and the replay harness
already pin byte for byte.

Ed25519 comes from `cryptography` rather than from arithmetic written here. A
hand-rolled curve implementation in the verification path is a timing side
channel and a correctness liability, and there is no reason to accept either
when a maintained library is already a dependency elsewhere in the stack.
"""

from __future__ import annotations

import base64
import binascii
import hashlib
import hmac
from dataclasses import dataclass

HMAC_SHA256 = "hmac-sha256"
ED25519 = "ed25519"

SIGNATURE_BYTES_HMAC = 32
SIGNATURE_BYTES_ED25519 = 64

# What a node provisioned before Ed25519 existed has. Provisioning validates the
# name it is given, so this default only applies to rows written by an older
# build and never to request input.
DEFAULT_ALGORITHM = HMAC_SHA256

ALGORITHMS = (HMAC_SHA256, ED25519)


class SignatureError(ValueError):
    """A key or algorithm that cannot be used as given."""


@dataclass(frozen=True)
class Algorithm:
    """Signing and verification for one algorithm, keyed by its wire name."""

    name: str
    signature_bytes: int

    def sign(self, body: bytes, key: str) -> bytes:
        """Returns the detached signature over `body`."""
        if self.name == HMAC_SHA256:
            if not key:
                raise SignatureError("cannot sign without a device key")
            return hmac.new(key.encode("utf-8"), body, hashlib.sha256).digest()
        if self.name == ED25519:
            return _ed25519_sign(body, key)
        raise SignatureError(f"unknown signature algorithm {self.name!r}")

    def verify(self, body: bytes, signature: bytes, key: str) -> bool:
        """Whether `signature` is the one `key` produces over `body`.

        Never raises. A wrong key, a malformed key and a tampered frame are the
        same answer, because the caller cannot act on the difference and a
        distinguishable failure is an oracle.
        """
        if len(signature) != self.signature_bytes:
            return False
        if self.name == HMAC_SHA256:
            if not key:
                return False
            expected = hmac.new(key.encode("utf-8"), body,
                                hashlib.sha256).digest()
            return hmac.compare_digest(expected, signature)
        if self.name == ED25519:
            try:
                raw = decode_key_material(key, ED25519_PUBLIC_KEY_BYTES)
                loaded = load_ed25519_public(raw)
                loaded.verify(signature, body)
                return True
            except Exception:
                return False
        return False


_BY_NAME = {
    HMAC_SHA256: Algorithm(HMAC_SHA256, SIGNATURE_BYTES_HMAC),
    ED25519: Algorithm(ED25519, SIGNATURE_BYTES_ED25519),
}

ED25519_PUBLIC_KEY_BYTES = 32
ED25519_PRIVATE_KEY_BYTES = 32


def get_algorithm(name: str | None) -> Algorithm:
    """Resolves a provisioned algorithm name, rejecting unknown ones."""
    if not name:
        return _BY_NAME[DEFAULT_ALGORITHM]
    algorithm = _BY_NAME.get(name)
    if algorithm is None:
        raise SignatureError(f"unknown signature algorithm {name!r}")
    return algorithm


def signature_bytes(name: str | None) -> int:
    """Trailer length for an algorithm, for framing and budget arithmetic."""
    return get_algorithm(name).signature_bytes


def sign_frame(frame: bytes, key: str, algorithm: str | None = None) -> bytes:
    """Appends the signature trailer for `algorithm` to `frame`."""
    return frame + get_algorithm(algorithm).sign(frame, key)


def verify_frame(frame: bytes, key: str, algorithm: str | None = None) -> bool:
    """Checks the signature trailer on a complete signed frame.

    `frame` is the frame exactly as transmitted, trailer included.
    """
    alg = get_algorithm(algorithm)
    if len(frame) <= alg.signature_bytes:
        return False
    return alg.verify(frame[:-alg.signature_bytes],
                      frame[-alg.signature_bytes:], key)


def split_signed_frame(frame: bytes, algorithm: str | None = None
                       ) -> tuple[bytes, bytes]:
    """Separates a signed frame into its body and its signature."""
    alg = get_algorithm(algorithm)
    if len(frame) <= alg.signature_bytes:
        raise SignatureError("frame is too short to carry a signature")
    return frame[:-alg.signature_bytes], frame[-alg.signature_bytes:]


# --- Ed25519 helpers -----------------------------------------------------


def load_ed25519_public(raw: bytes):
    """Returns a `cryptography` public key object for 32 raw bytes."""
    from cryptography.hazmat.primitives.asymmetric.ed25519 import (
        Ed25519PublicKey,
    )

    return Ed25519PublicKey.from_public_bytes(raw)


def load_ed25519_private(raw: bytes):
    """Returns a `cryptography` private key object for a 32-byte seed."""
    from cryptography.hazmat.primitives.asymmetric.ed25519 import (
        Ed25519PrivateKey,
    )

    return Ed25519PrivateKey.from_private_bytes(raw)


def _ed25519_sign(body: bytes, private_key: str) -> bytes:
    raw = decode_key_material(private_key, ED25519_PRIVATE_KEY_BYTES)
    key = load_ed25519_private(raw)
    return key.sign(body)


def decode_key_material(key: str, expected_bytes: int) -> bytes:
    """Reads key material given as hex or base64.

    Both are accepted because operators paste keys from both kinds of tool, but
    the encoding must be unambiguous: base64 that is also valid hex would
    otherwise decode two different ways depending on which branch ran.
    """
    if not isinstance(key, str):
        raise SignatureError("key material must be text")
    text = key.strip()
    if not text:
        raise SignatureError("key material is empty")

    raw: bytes | None = None
    if len(text) == expected_bytes * 2:
        try:
            raw = bytes.fromhex(text)
        except ValueError:
            raw = None
    if raw is None:
        try:
            candidate = base64.b64decode(text, validate=True)
        except (binascii.Error, ValueError) as exc:
            raise SignatureError(
                "key is neither hex nor base64") from exc
        if len(candidate) != expected_bytes:
            raise SignatureError(
                f"expected {expected_bytes} bytes of key material, "
                f"got {len(candidate)}")
        raw = candidate

    if len(raw) != expected_bytes:
        raise SignatureError(
            f"expected {expected_bytes} bytes of key material, got {len(raw)}")
    return raw


def encode_key_material(raw: bytes) -> str:
    """Renders key material as hex, the form provisioning accepts."""
    return raw.hex()
