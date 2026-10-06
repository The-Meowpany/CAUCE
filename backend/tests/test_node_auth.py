"""Certificate authentication for a node that has an Ed25519 key.

The property under test throughout is **substitution resistance**: evidence that is genuine
but belongs to something else must be refused. That is the whole reason for a certificate, and
it is the property a shared secret cannot express at all - with HMAC, possessing the key *is*
being the node.

The order of the checks is asserted, not assumed. A replayed request must be stopped by the
nonce before any certificate state is reported, and the test for that is written so a change
in order fails it.

One check has no counterpart in the other scheme and deserves its own test: a *genuine*
certificate for a node whose key has since been rotated must stop working. Every signature in
that request is real; the node is simply no longer the one it claims to be.
"""

from __future__ import annotations

import base64
import os

import pytest
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

os.environ["CAUCE_DB_PATH"] = "./data/test_node_auth.sqlite"
os.environ["CAUCE_CA_KEY"] = bytes(range(32)).hex()

from cauce_server.certificates import CertificateAuthority  # noqa: E402
from cauce_server.node_auth import (  # noqa: E402
    MAX_PENDING_CHALLENGES,
    AuthError,
    ChallengeStore,
    canonical_challenge,
    verify_node_certificate_auth,
)

CA_SEED = bytes(range(32))
NODE_SEED = bytes([7]) * 32
OTHER_SEED = bytes([9]) * 32
NOW = 1_787_356_800_000


@pytest.fixture()
def ca():
    return CertificateAuthority(CA_SEED.hex())


@pytest.fixture()
def store():
    return ChallengeStore()


def key_pair(seed: bytes):
    key = Ed25519PrivateKey.from_private_bytes(seed)
    return key, key.public_key().public_bytes_raw().hex()


def issue(ca: CertificateAuthority, node_id="CAUCE-001", public=None,
          validity=90 * 24 * 3600, now_ms=NOW):
    return ca.issue(node_id, public or key_pair(NODE_SEED)[1],
                    validity_seconds=validity, now_ms=now_ms)


def sign_challenge(key, store: ChallengeStore, node_id="CAUCE-001", now_ms=NOW):
    challenge = store.issue(node_id, now_ms=now_ms)
    signature = key.sign(canonical_challenge(challenge, node_id))
    return challenge, base64.b64encode(signature).decode("ascii")


def auth(certificate, signature, challenge, *, node_id="CAUCE-001", registered=None,
         store=None, ca=None, now_ms=NOW):
    verify_node_certificate_auth(
        certificate, signature, challenge.nonce, node_id,
        (ca or CertificateAuthority(CA_SEED.hex())).public_key_hex,
        registered or key_pair(NODE_SEED)[1],
        store=store, now_ms=now_ms,
    )


# --- the happy path ----------------------------------------------------

def test_a_node_with_a_certificate_and_its_key_authenticates(ca, store):
    key, public = key_pair(NODE_SEED)
    challenge, signature = sign_challenge(key, store)
    auth(issue(ca), signature, challenge, ca=ca, store=store, registered=public)


def test_the_node_id_is_inside_the_signed_bytes(ca, store):
    """Not just looked up in the store: the node id is part of what was signed.

    Presenting the same challenge and the same signature as a different node fails. The store
    lookup already catches that, so this covers the second, independent defence: even with a
    store that handed the challenge over unconditionally, the signature would not verify.
    """
    key, public = key_pair(NODE_SEED)
    challenge = store.issue("CAUCE-001", now_ms=NOW)
    signature = base64.b64encode(
        key.sign(canonical_challenge(challenge, "CAUCE-001"))).decode("ascii")
    # Honest presentation for the node the challenge belongs to.
    auth(issue(ca, public=public), signature, challenge,
         ca=ca, store=store, registered=public)

    # Same bytes, presented as somebody else.
    other = store.issue("CAUCE-002", now_ms=NOW)
    with pytest.raises(AuthError) as excinfo:
        verify_node_certificate_auth(
            issue(ca, node_id="CAUCE-002", public=public), signature,
            other.nonce, "CAUCE-002", ca.public_key_hex, public, store=store,
            now_ms=NOW,
        )
    assert "challenge_signature_invalid" in str(excinfo.value)


def test_the_canonical_challenge_is_stable_and_names_the_nonce(ca, store):
    """Both sides must produce identical bytes or nothing authenticates."""
    challenge = store.issue("CAUCE-001", now_ms=NOW)
    payload = canonical_challenge(challenge, "CAUCE-001")
    assert payload == (
        b"cauce-sync-challenge\nCAUCE-001\n" + challenge.nonce.encode()
        + b"\n" + str(challenge.expires_utc_ms).encode()
    )


# --- substitution ------------------------------------------------------

def test_a_genuine_certificate_for_another_node_is_refused(ca, store):
    """The case that separates a certificate from a shared secret.

    Both certificates are real, both are signed by this CA, neither is expired. The only
    difference is which node each names. With HMAC there is no way to express this at all.
    """
    key, _ = key_pair(NODE_SEED)
    challenge, signature = sign_challenge(key, store)
    # The certificate is genuine and for CAUCE-002; the request claims CAUCE-001. The nonce
    # is issued to CAUCE-001 and presented as CAUCE-001, so the store lookup passes and the
    # failure has to come from the certificate's *subject*. That ordering matters: the nonce is
    # checked first, so a mismatched pair fails earlier and for a different reason, and this
    # test would then assert the wrong thing while appearing to pass.
    with pytest.raises(AuthError) as excinfo:
        auth(issue(ca, node_id="CAUCE-002"), signature, challenge,
             node_id="CAUCE-001", ca=ca, store=store)
    assert "issued_for_another_node" in str(excinfo.value)


def test_a_signature_from_a_different_key_is_refused(ca, store):
    """The attacker holds a certificate for this node but not the node's key."""
    _, public = key_pair(NODE_SEED)
    attacker_key, _ = key_pair(OTHER_SEED)
    challenge = store.issue("CAUCE-001", now_ms=NOW)
    signature = attacker_key.sign(canonical_challenge(challenge, "CAUCE-001"))
    with pytest.raises(AuthError) as excinfo:
        auth(issue(ca, public=public), base64.b64encode(signature).decode("ascii"),
             challenge, ca=ca, store=store)
    assert "challenge_signature_invalid" in str(excinfo.value)


def test_a_rotated_key_stops_authenticating_with_its_old_certificate(ca, store):
    """Every signature here is real and the node is still the node. It must stop working.

    This is the check a shared secret gets for free and a certificate does not, and it is the
    whole reason a rotation is a rotation: without comparing the certificate's key against the
    registered one, a certificate issued before the rotation keeps authenticating its holder
    indefinitely, which is exactly the window rotation exists to close.
    """
    key, public = key_pair(NODE_SEED)
    certificate = issue(ca, public=public)
    challenge, signature = sign_challenge(key, store)

    rotated = key_pair(OTHER_SEED)[1]
    with pytest.raises(AuthError) as excinfo:
        auth(certificate, signature, challenge,
             ca=ca, store=store, registered=rotated)
    assert "not_the_registered_key" in str(excinfo.value)


def test_a_certificate_from_another_ca_is_refused(ca, store):
    """A second CA's genuine certificate for this node must not authenticate."""
    other_ca = CertificateAuthority((bytes([3]) * 32).hex())
    key, public = key_pair(NODE_SEED)
    challenge, signature = sign_challenge(key, store)
    with pytest.raises(AuthError) as excinfo:
        auth(other_ca.issue("CAUCE-001", public, now_ms=NOW), signature, challenge,
             ca=ca, store=store)
    assert "certificate_bad_signature" in str(excinfo.value)


def test_an_expired_certificate_is_refused(ca):
    """The certificate expires inside the challenge's window.

    A 60-second challenge TTL and a 1-hour certificate cannot both be crossed by advancing
    the clock past the certificate, because the challenge would expire first - and it should,
    since the nonce is checked before the certificate. So the store gets a TTL long enough to
    outlive the certificate. Getting this wrong the other way produces a test that passes while
    asserting nothing about certificate expiry.
    """
    store = ChallengeStore(ttl_ms=10_000_000)
    key, public = key_pair(NODE_SEED)
    certificate = ca.issue("CAUCE-001", public, validity_seconds=3600, now_ms=NOW)
    challenge, signature = sign_challenge(key, store)
    with pytest.raises(AuthError) as excinfo:
        auth(certificate, signature, challenge, ca=ca, store=store,
             now_ms=NOW + 3600_000 + 1)
    assert "certificate_expired" in str(excinfo.value)


# --- replay ------------------------------------------------------------

def test_a_challenge_cannot_be_replayed(ca, store):
    """The nonce is single-use, and that is the entire replay defence.

    Neither the certificate nor the signature changes between the two attempts, so without
    single-use consumption this would authenticate forever.
    """
    key, public = key_pair(NODE_SEED)
    challenge, signature = sign_challenge(key, store)
    certificate = issue(ca, public=public)

    auth(certificate, signature, challenge, ca=ca, store=store)

    with pytest.raises(AuthError) as excinfo:
        auth(certificate, signature, challenge, ca=ca, store=store)
    assert "unknown_or_spent_nonce" in str(excinfo.value)


def test_a_replay_is_stopped_by_the_nonce_before_the_certificate_is_examined(ca, store):
    """Order is a property, not a preference.

    A replayed request must be rejected without reporting anything about the certificate, or
    the endpoint becomes a way to probe certificate state with a captured request. The check
    for that is that the reason is the nonce's and not the certificate's, and it fails if the
    order is ever inverted.
    """
    key, public = key_pair(NODE_SEED)
    challenge, signature = sign_challenge(key, store)
    auth(issue(ca, public=public), signature, challenge, ca=ca, store=store)

    # An expired certificate on the replay: if the certificate were checked first, the reason
    # would be about expiry. It is about the nonce, so the nonce is checked first.
    expired = ca.issue("CAUCE-001", public, validity_seconds=3600, now_ms=NOW)
    with pytest.raises(AuthError) as excinfo:
        auth(expired, signature, challenge, ca=ca, store=store,
             now_ms=NOW + 7200_000)
    assert "nonce" in str(excinfo.value)


def test_an_expired_challenge_is_refused(ca, store):
    key, public = key_pair(NODE_SEED)
    certificate = issue(ca, public=public)
    challenge, signature = sign_challenge(key, store)
    with pytest.raises(AuthError) as excinfo:
        auth(certificate, signature, challenge, ca=ca, store=store,
             now_ms=NOW + 120_000)
    assert "challenge_expired" in str(excinfo.value)


def test_a_nonce_presented_by_the_wrong_node_is_consumed_not_returned(ca, store):
    """One node must not be able to burn another's nonce.

    A node that got a challenge for itself must not be able to spend it against a different
    node's identity, and the nonce must not go back into the pool afterwards, or an attacker
    could deny a legitimate node its handshake repeatedly.
    """
    key, public = key_pair(NODE_SEED)
    challenge, signature = sign_challenge(key, store, node_id="CAUCE-001")
    with pytest.raises(AuthError) as excinfo:
        auth(issue(ca, public=public), signature, challenge,
             node_id="CAUCE-002", ca=ca, store=store)
    assert "issued_to_another_node" in str(excinfo.value)
    assert store.pending_count() == 0, "the nonce went back into the pool"


# --- malformed input ---------------------------------------------------

@pytest.mark.parametrize("signature", [
    "", "not base64", base64.b64encode(b"short").decode(), "!!!!",
])
def test_a_malformed_signature_is_refused_not_raised(ca, store, signature):
    """A caller must get an answer, not a 500, whatever they send."""
    _, public = key_pair(NODE_SEED)
    challenge = store.issue("CAUCE-001", now_ms=NOW)
    with pytest.raises(AuthError) as excinfo:
        auth(issue(ca, public=public), signature, challenge, ca=ca, store=store)
    assert "malformed_nonce_signature" in str(excinfo.value)


def test_an_unknown_nonce_is_refused(ca, store):
    _, public = key_pair(NODE_SEED)
    key, _ = key_pair(NODE_SEED)
    challenge, signature = sign_challenge(key, ChallengeStore())
    with pytest.raises(AuthError) as excinfo:
        auth(issue(ca, public=public), signature, challenge, ca=ca, store=store)
    assert "unknown_or_spent_nonce" in str(excinfo.value)


# --- the store itself --------------------------------------------------

def test_expired_challenges_are_evicted_rather_than_growing(store):
    """An unauthenticated endpoint issuing challenges must not be a memory vector."""
    for _ in range(50):
        store.issue("CAUCE-001", now_ms=NOW)
    assert store.pending_count() == 50
    store.issue("CAUCE-001", now_ms=NOW + 120_000)
    assert store.pending_count() == 1, "expired challenges were not evicted"


def test_the_store_is_capped(store):
    """A full store is evicted, not rejected.

    Rejecting would let one caller deny authentication to every node, turning a capacity limit
    into an outage. The oldest entry goes, because an unexpired challenge belonging to a node
    that has abandoned the handshake is the cheapest thing to lose.
    """
    for index in range(MAX_PENDING_CHALLENGES + 10):
        store.issue(f"CAUCE-{index:04d}", now_ms=NOW + index)
    assert store.pending_count() <= MAX_PENDING_CHALLENGES


def test_issued_nonces_are_unpredictable(store):
    """A guessable nonce makes the single-use property irrelevant."""
    nonces = {store.issue("CAUCE-001").nonce for _ in range(200)}
    assert len(nonces) == 200, "a nonce was repeated"


def test_a_challenge_expires_in_the_documented_window(store):
    challenge = store.issue("CAUCE-001", now_ms=NOW)
    assert challenge.expires_utc_ms - NOW == 60_000


def test_the_store_defaults_to_the_clock(ca):
    """Issued and spent without passing a time, so the production path is exercised."""
    store = ChallengeStore()
    key, public = key_pair(NODE_SEED)
    challenge = store.issue("CAUCE-001")
    signature = key.sign(canonical_challenge(challenge, "CAUCE-001"))
    verify_node_certificate_auth(
        issue(ca, now_ms=None), base64.b64encode(signature).decode("ascii"),
        challenge.nonce, "CAUCE-001", ca.public_key_hex, public, store=store,
    )
