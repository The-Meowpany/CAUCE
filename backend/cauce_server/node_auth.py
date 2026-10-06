"""Certificate-based node authentication, for nodes that have an Ed25519 key.

THE PROBLEM THIS SOLVES

`/v1/sync` authenticates a node with an HMAC over the request body, keyed by
`nodes.device_key`. That is symmetric: every node holding the key can produce a valid
signature for that node, and the central stores a secret that can both authenticate and forge.

The central already had the pieces to do better and was not using them. An Ed25519 key was
provisioned, a CA exists to certify the binding of identity to public key, and the firmware
signs frames with exactly the key that would answer a challenge. So an Ed25519 node could
prove possession of a private key the central has only ever seen the public half of.

THE FLOW

    POST /v1/sync/challenge   {node_id}          -> {nonce, expires_utc_ms}
    POST /v1/sync             X-Cauce-Certificate, X-Cauce-Nonce-Signature, body

The node signs the nonce with its private key. The central then checks four things, and each
is load-bearing:

1. the certificate is signed by this CA - so the binding of identity to key is not a row
   somebody wrote with the admin token;
2. the certificate names *this* node - a genuine certificate for a different node verifies
   perfectly and must not be accepted here, which is the failure a shared secret cannot even
   express;
3. the certificate is unexpired and unrevoked by supersession;
4. the nonce was issued to this node, has not expired, and has not been spent.

Point 4 is what makes this replay-resistant. Without it a captured
certificate-plus-signature pair would authenticate forever, because neither the certificate
nor the signature changes. Single-use is the whole of the replay defence, so the nonce store
is not an optimisation.

WHAT DOES NOT CHANGE

The HMAC path stays, and stays first. Every node provisioned before this exists has an HMAC
key, and an Ed25519 node that has not been issued a certificate must keep working. A node
presenting a certificate is required to use certificate auth; it cannot fall back to the
shared secret, because a fallback there would mean the weaker scheme is always available and
the stronger one is decorative.
"""

from __future__ import annotations

import base64
import secrets
import time
from dataclasses import dataclass

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey

from .certificates import certificate_is_valid_for_node
from .signing import (
    ED25519_PUBLIC_KEY_BYTES,
    SignatureError,
    decode_key_material,
)

# How long a challenge stays usable. Long enough for a node on a slow LoRa hop to fetch and
# answer, short enough that a captured pair has a small window. There is no retry logic that
# would justify much more: a node that misses this fetches another.
CHALLENGE_TTL_MS = 60_000

# Challenges are held in memory, deliberately.
#
# They are single-use, short-lived and worthless once spent, so persisting them would buy
# nothing and cost a write on the central's hot path for every sync. The trade-off is that
# challenges do not survive a restart, which means a node mid-handshake across a central
# restart gets a 401 and fetches another. That is the correct failure: retryable, and it
# never accepts anything it should have rejected.
#
# The ceiling exists because this is an unauthenticated endpoint - anybody can ask for a
# challenge - and an unbounded map would be a memory exhaustion vector reachable without a
# credential.
MAX_PENDING_CHALLENGES = 4096


class AuthError(Exception):
    """A node failed certificate authentication. The reason is safe to return.

    Every value of `reason` describes a fact about the presented evidence that the caller
    already knows: their certificate is expired, their nonce was spent, the certificate names
    a different node. None of them distinguishes "close" from "far", so this is not an
    oracle.
    """


@dataclass(frozen=True)
class Challenge:
    node_id: str
    nonce: str
    expires_utc_ms: int
    spent: bool = False


class ChallengeStore:
    """Single-use, expiring nonces for certificate authentication."""

    def __init__(self, ttl_ms: int = CHALLENGE_TTL_MS):
        self._ttl_ms = ttl_ms
        self._pending: dict[str, Challenge] = {}

    def issue(self, node_id: str, now_ms: int | None = None) -> Challenge:
        moment = int(now_ms if now_ms is not None else time.time() * 1000)
        self._evict_expired(moment)
        # A full store is evicted rather than rejected: refusing would let one caller deny
        # authentication to every node, which turns a capacity limit into an outage. The
        # oldest entry goes, because an unexpired challenge belonging to a node that has
        # already abandoned the handshake is the cheapest thing to lose.
        while len(self._pending) >= MAX_PENDING_CHALLENGES:
            oldest = min(self._pending, key=lambda k: self._pending[k].expires_utc_ms)
            self._pending.pop(oldest, None)
        challenge = Challenge(
            node_id=node_id,
            nonce=secrets.token_urlsafe(32),
            expires_utc_ms=moment + self._ttl_ms,
        )
        self._pending[challenge.nonce] = challenge
        return challenge

    def spend(self, nonce: str, node_id: str, now_ms: int | None = None) -> Challenge:
        """Consumes a nonce, or raises.

        Consumption is by removal, so a second attempt with the same nonce finds nothing.
        That is what makes it single-use without a separate "already spent" state that could
        be raced.
        """
        moment = int(now_ms if now_ms is not None else time.time() * 1000)
        challenge = self._pending.pop(nonce, None)
        if challenge is None:
            # Unknown and already-spent are the same answer on purpose: telling them apart
            # would confirm that a nonce was once valid, which is a small piece of
            # information about a captured request worth nothing on its own but free to give.
            raise AuthError("unknown_or_spent_nonce")
        if challenge.node_id != node_id:
            # Spent it anyway. The nonce is bound to the node it was issued to, and a
            # mismatch means the caller is not that node; consuming rather than returning it
            # to the pool stops an attacker from burning a legitimate node's nonce.
            raise AuthError("nonce_issued_to_another_node")
        if moment > challenge.expires_utc_ms:
            raise AuthError("challenge_expired")
        return challenge

    def _evict_expired(self, now_ms: int) -> None:
        for nonce in [n for n, c in self._pending.items()
                      if now_ms > c.expires_utc_ms]:
            self._pending.pop(nonce, None)

    def pending_count(self) -> int:
        return len(self._pending)


# One store per process. A multi-worker central would have one per worker, so a challenge
# issued by worker A is unknown to worker B and the node gets a 401 and retries. That is
# correct behaviour under a wrong assumption about the deployment, and the alternative -
# sharing the store over Redis - is a dependency this project does not otherwise have, for a
# 60-second window that retries cleanly.
CHALLENGES = ChallengeStore()


def canonical_challenge(challenge: Challenge, node_id: str) -> bytes:
    """The exact bytes a node signs.

    Node id and nonce and an expiry, joined with a separator that cannot occur in a node id,
    because `node_id` is restricted to `[A-Za-z0-9_.-]`. Both sides must produce identical
    bytes or every authentication fails, so the encoding is spelled out here and pinned by a
    test on both sides of the claim.
    """
    return f"cauce-sync-challenge\n{node_id}\n{challenge.nonce}\n{challenge.expires_utc_ms}".encode()


def verify_node_certificate_auth(
    certificate: dict,
    signature_b64: str,
    nonce: str,
    node_id: str,
    ca_public_key_hex: str,
    registered_public_key_hex: str,
    store: ChallengeStore = CHALLENGES,
    now_ms: int | None = None,
) -> None:
    """Authenticates a node, or raises `AuthError`.

    The order of the checks is deliberate and is the order in which a caller should learn
    why it was rejected:

    1. the nonce, because it is the cheapest thing to reject and consuming it first means a
       replayed request cannot proceed to the more expensive checks at all;
    2. the certificate's signature and subject, so an attacker learns nothing about a node's
       certificate state without a nonce they were issued;
    3. that the certificate's key is the one registered for this node, which is the check
       that stops a *valid, genuine* certificate for this node from being used after the
       central rotated the key - the case where every signature is real and the node is
       nonetheless no longer the one it claims;
    4. the nonce signature itself, last, because it is the only expensive operation.
    """
    moment = int(now_ms if now_ms is not None else time.time() * 1000)

    challenge = store.spend(nonce, node_id, now_ms=moment)

    ok, reason = certificate_is_valid_for_node(
        certificate, node_id, ca_public_key_hex, now_ms=moment)
    if not ok:
        raise AuthError(f"certificate_{reason}")

    # Check 3: the certificate must certify the key the central has on file. Without this, a
    # certificate issued before a key rotation keeps authenticating its holder forever, which
    # is precisely the window a rotation exists to close.
    try:
        presented = decode_key_material(
            certificate.get("public_key", ""), ED25519_PUBLIC_KEY_BYTES).hex()
        registered = decode_key_material(
            registered_public_key_hex, ED25519_PUBLIC_KEY_BYTES).hex()
    except SignatureError as exc:
        raise AuthError("malformed_public_key") from exc
    if not secrets.compare_digest(presented, registered):
        raise AuthError("certificate_key_is_not_the_registered_key")

    try:
        signature = base64.b64decode(signature_b64, validate=True)
    except Exception as exc:
        raise AuthError("malformed_nonce_signature") from exc
    if len(signature) != 64:
        raise AuthError("malformed_nonce_signature")

    try:
        public = Ed25519PublicKey.from_public_bytes(
            decode_key_material(presented, ED25519_PUBLIC_KEY_BYTES))
        public.verify(signature, canonical_challenge(challenge, node_id))
    except InvalidSignature as exc:
        raise AuthError("challenge_signature_invalid") from exc
    except (ValueError, TypeError) as exc:
        raise AuthError("challenge_signature_invalid") from exc

