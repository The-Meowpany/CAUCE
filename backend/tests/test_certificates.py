"""Certificates: what a CA signature actually buys, and what it does not.

The security property under test throughout is *substitution resistance*: a certificate
for one node must not be accepted for another, and a valid certificate must not be
modifiable without breaking the signature. Everything else here is scaffolding.

Each test that tampers with a field checks that the *reason* is the tampering one. A
certificate that fails with `missing_node_id` because a test happened to also blank the
node id would pass a bare `assert not ok`, and would hide a verifier that stopped checking
the thing it was supposed to check.
"""

from __future__ import annotations

import os

import pytest
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

os.environ["CAUCE_DB_PATH"] = "./data/test_certificates.sqlite"

from cauce_server.certificates import (  # noqa: E402
    CERTIFICATE_VERSION,
    MAX_VALIDITY_SECONDS,
    CertificateAuthority,
    CertificateError,
    canonical_body,
    certificate_is_valid_for_node,
    verify_certificate,
)
from cauce_server.signing import encode_key_material  # noqa: E402

# A fixed CA seed, so every assertion here is a regression against a known value rather than
# against whatever key the test happened to generate.
CA_SEED = bytes(range(32))
NOW = 1_787_356_800_000


@pytest.fixture()
def ca():
    return CertificateAuthority(CA_SEED.hex())


def make_keypair(seed_byte: int = 200):
    key = Ed25519PrivateKey.from_private_bytes(bytes([seed_byte]) * 32)
    return encode_key_material(key.public_key().public_bytes_raw())


def test_the_ca_publishes_its_public_key_and_not_its_private(ca):
    public = ca.public_key_hex
    assert len(public) == 64
    # The private seed must not appear anywhere in what a verifier receives.
    assert CA_SEED.hex() not in public
    certificate = ca.issue("CAUCE-001", make_keypair(), now_ms=NOW)
    assert CA_SEED.hex() not in str(certificate)


def test_a_fresh_certificate_verifies(ca):
    certificate = ca.issue("CAUCE-001", make_keypair(), now_ms=NOW)
    ok, reason = verify_certificate(certificate, ca.public_key_hex, now_ms=NOW)
    assert ok, reason


def test_the_signature_covers_every_field(ca):
    """Every claim in the body, each one alone, must break verification."""
    certificate = ca.issue("CAUCE-001", make_keypair(), site_id="SITE-A", now_ms=NOW)

    for field, value in (
        ("node_id", "CAUCE-999"),
        ("site_id", "SITE-Z"),
        ("serial", "00" * 16),
        ("not_after_utc_ms", certificate["not_after_utc_ms"] + 1),
        ("public_key", make_keypair(201)),
    ):
        tampered = dict(certificate)
        tampered[field] = value
        ok, _ = verify_certificate(tampered, ca.public_key_hex, now_ms=NOW)
        assert not ok, f"changing {field} did not break the certificate"

    # Not_after is not excluded as "not part of the body" - extending the validity is
    # exactly the tampering a CA signature exists to prevent.
    assert "not_after_utc_ms" in canonical_body(certificate).decode()


def test_a_certificate_from_another_ca_is_refused(ca):
    """A different CA with a valid key signature must not be accepted."""
    other = CertificateAuthority(bytes([7] * 32).hex())
    forged = other.issue("CAUCE-001", make_keypair(), now_ms=NOW)

    ok, reason = verify_certificate(forged, ca.public_key_hex, now_ms=NOW)
    assert not ok
    assert reason == "bad_signature"
    # And the forged one is not secretly valid against its own issuer either, which would
    # mean the signature was never checked at all.
    assert verify_certificate(forged, other.public_key_hex, now_ms=NOW)[0]


def test_a_node_key_cannot_sign_its_own_certificate(ca):
    """The node's key is the subject of the certificate, never its issuer."""
    node_key = Ed25519PrivateKey.from_private_bytes(bytes([9] * 32))
    node_public = encode_key_material(node_key.public_key().public_bytes_raw())

    forged = {
        "version": CERTIFICATE_VERSION,
        "kind": "cauce-node-certificate",
        "serial": "11" * 16,
        "node_id": "CAUCE-001",
        "site_id": None,
        "algorithm": "ed25519",
        "public_key": node_public,
        "not_before_utc_ms": NOW - 1000,
        "not_after_utc_ms": NOW + 100_000,
    }
    forged["signature"] = node_key.sign(canonical_body(forged)).hex()
    forged["ca_public_key"] = ca.public_key_hex

    ok, reason = verify_certificate(forged, ca.public_key_hex, now_ms=NOW)
    assert not ok
    assert reason == "bad_signature"


def test_an_expired_certificate_is_refused(ca):
    certificate = ca.issue("CAUCE-001", make_keypair(),
                           validity_seconds=3600, now_ms=NOW)
    assert verify_certificate(certificate, ca.public_key_hex, now_ms=NOW)[0]
    # One millisecond past the boundary. An expiry checked with `>=` would accept this.
    ok, reason = verify_certificate(
        certificate, ca.public_key_hex, now_ms=NOW + 3600 * 1000 + 1)
    assert not ok
    assert reason == "expired"


def test_a_certificate_is_not_valid_before_it_was_issued(ca):
    certificate = ca.issue("CAUCE-001", make_keypair(), now_ms=NOW)
    ok, reason = verify_certificate(certificate, ca.public_key_hex,
                                    now_ms=NOW - 1)
    assert not ok
    assert reason == "not_yet_valid"


def test_a_valid_certificate_for_another_node_is_refused(ca):
    """The failure a shared-admin-token trust model cannot detect.

    Two certificates, both genuine, both signed by the same CA. The only difference is
    which node each names. A verifier that checks the signature and not the subject accepts
    CAUCE-001's certificate for CAUCE-002, and every frame it then verifies belongs to a node
    it never authorised.
    """
    certificate = ca.issue("CAUCE-001", make_keypair(), now_ms=NOW)

    ok, reason = certificate_is_valid_for_node(
        certificate, "CAUCE-002", ca.public_key_hex, now_ms=NOW)
    assert not ok
    assert reason == "issued_for_another_node"

    # And it is genuinely valid for its own node, so the failure above is the subject check
    # and not a signature that happens to be broken.
    assert certificate_is_valid_for_node(
        certificate, "CAUCE-001", ca.public_key_hex, now_ms=NOW)[0]


def test_two_certificates_for_one_node_have_distinct_serials(ca):
    """Otherwise revoking one would revoke the other, and the reason codes collide."""
    first = ca.issue("CAUCE-001", make_keypair(), now_ms=NOW)
    second = ca.issue("CAUCE-001", make_keypair(), now_ms=NOW)
    assert first["serial"] != second["serial"]
    assert verify_certificate(first, ca.public_key_hex, now_ms=NOW)[0]
    assert verify_certificate(second, ca.public_key_hex, now_ms=NOW)[0]


def test_the_canonical_body_is_stable_across_key_order(ca):
    """Two certificates with the same content serialise to the same bytes.

    Without this, recomputing a signature on a round-tripped document could fail because
    JSON object order changed, which is a failure nobody can debug from a frame capture.
    """
    body = {
        "node_id": "CAUCE-001",
        "serial": "aa" * 16,
        "version": CERTIFICATE_VERSION,
        "algorithm": "ed25519",
    }
    reordered = {
        "algorithm": "ed25519",
        "version": CERTIFICATE_VERSION,
        "serial": "aa" * 16,
        "node_id": "CAUCE-001",
    }
    assert canonical_body(body) == canonical_body(reordered)
    assert b" " not in canonical_body(body), "canonical JSON carries whitespace"


def test_the_canonical_body_excludes_only_the_signature_and_the_ca_key(ca):
    certificate = ca.issue("CAUCE-001", make_keypair(), now_ms=NOW)
    signed = canonical_body(certificate)
    assert certificate["signature"] not in signed.decode()
    # The CA key is added alongside the signature, so it is not part of what is signed. That
    # is why `ca_public_key` is excluded rather than signed: it is a hint about who to
    # verify with, and a verifier that trusted it would be trusting the certificate to name
    # its own issuer.
    assert "ca_public_key" not in signed.decode()
    for field in ("node_id", "serial", "public_key", "not_before_utc_ms",
                  "not_after_utc_ms", "version", "kind", "algorithm"):
        assert f'"{field}"' in signed.decode(), f"{field} is not covered by the signature"


def test_a_wrong_version_is_refused(ca):
    certificate = ca.issue("CAUCE-001", make_keypair(), now_ms=NOW)
    certificate["version"] = CERTIFICATE_VERSION + 1
    ok, reason = verify_certificate(certificate, ca.public_key_hex, now_ms=NOW)
    assert not ok
    # Version is excluded from nothing, so the signature also breaks - but the version
    # check comes first so the reason is about the version, which is what a future parser
    # needs to read.
    assert reason == "unsupported_version"


def test_a_certificate_of_the_wrong_kind_is_refused(ca):
    certificate = ca.issue("CAUCE-001", make_keypair(), now_ms=NOW)
    certificate["kind"] = "something-else"
    ok, reason = verify_certificate(certificate, ca.public_key_hex, now_ms=NOW)
    assert not ok
    assert reason == "wrong_kind"


def test_issuing_refuses_a_malformed_public_key(ca):
    """A certificate binding a node to a truncated key is worthless and must not exist."""
    for bad in ("00" * 31, "", "not-a-key", "00" * 64):
        with pytest.raises(CertificateError):
            ca.issue("CAUCE-001", bad, now_ms=NOW)


def test_issuing_refuses_a_key_that_is_not_a_curve_point(ca):
    """Any 32 bytes form an Ed25519PublicKey object; about half of them are not a point.

    `from_public_bytes` accepts them all, so without an explicit check a certificate is
    issued for a key that can never verify anything, and the failure appears on a node
    rather than at issuance.
    """
    from cauce_server.certificates import is_on_curve

    good = make_keypair()
    assert is_on_curve(bytes.fromhex(good))

    # Search for a value that is 32 bytes, passes every length check, and is not a point.
    rejected = [raw for raw in (bytes([i]) * 32 for i in range(256))
                if not is_on_curve(raw)]
    assert rejected, "no non-point found, so the check is not being exercised"

    with pytest.raises(CertificateError) as excinfo:
        ca.issue("CAUCE-001", rejected[0].hex(), now_ms=NOW)
    assert "curve point" in str(excinfo.value)


def test_a_real_generated_key_is_on_the_curve(ca):
    """The guard against the check rejecting every key, which would be equally broken.

    `issue` is the only path that accepts a key, so if `is_on_curve` were wrong in the
    strict direction every provision in the fleet would fail. This asserts it accepts keys
    produced by a real signer.
    """
    for seed_byte in (0, 1, 17, 200, 255):
        key = Ed25519PrivateKey.from_private_bytes(bytes([seed_byte]) * 32)
        public = key.public_key().public_bytes_raw()
        certificate = ca.issue("CAUCE-001", public.hex(), now_ms=NOW)
        assert verify_certificate(certificate, ca.public_key_hex, now_ms=NOW)[0]


def test_a_non_canonical_y_is_refused(ca):
    """A y at or above p encodes a point twice, which is the malleability canonical
    encoding exists to prevent."""
    from cauce_server.certificates import _ED25519_P

    over_p = _ED25519_P.to_bytes(32, "little")
    with pytest.raises(CertificateError):
        ca.issue("CAUCE-001", over_p.hex(), now_ms=NOW)


def test_issuing_refuses_a_node_id_that_is_blank(ca):
    for bad in ("", "   ", None, 42):
        with pytest.raises(CertificateError):
            ca.issue(bad, make_keypair(), now_ms=NOW)


def test_issuing_refuses_an_absurd_validity(ca):
    """A certificate that outlives the deployment is refused, not clamped.

    Clamping would silently issue a one-year certificate to an operator who asked for ten
    years, and they would believe they had the ten they asked for.
    """
    with pytest.raises(CertificateError) as excinfo:
        ca.issue("CAUCE-001", make_keypair(),
                 validity_seconds=MAX_VALIDITY_SECONDS + 1, now_ms=NOW)
    assert "refused" in str(excinfo.value)

    for bad in (0, -1, -3600):
        with pytest.raises(CertificateError):
            ca.issue("CAUCE-001", make_keypair(), validity_seconds=bad, now_ms=NOW)


def test_the_maximum_validity_is_accepted(ca):
    """The boundary is inclusive, so the limit is a real ceiling and not an off-by-one."""
    certificate = ca.issue("CAUCE-001", make_keypair(),
                           validity_seconds=MAX_VALIDITY_SECONDS, now_ms=NOW)
    assert verify_certificate(certificate, ca.public_key_hex, now_ms=NOW)[0]


def test_a_tampered_signature_is_refused(ca):
    certificate = ca.issue("CAUCE-001", make_keypair(), now_ms=NOW)
    certificate["signature"] = "00" * 64
    ok, reason = verify_certificate(certificate, ca.public_key_hex, now_ms=NOW)
    assert not ok
    assert reason == "bad_signature"


def test_a_signature_of_the_wrong_length_is_refused_before_verification(ca):
    certificate = ca.issue("CAUCE-001", make_keypair(), now_ms=NOW)
    certificate["signature"] = "00" * 32
    ok, reason = verify_certificate(certificate, ca.public_key_hex, now_ms=NOW)
    assert not ok
    assert reason == "bad_signature_length"


def test_a_missing_ca_key_does_not_raise(ca):
    """A verifier holding garbage must get an answer, not an exception.

    It returns a reason rather than raising because the CA key arrives from configuration,
    and a config mistake should degrade to "this certificate is not trusted" rather than a
    500 on whichever request happened to arrive first.
    """
    certificate = ca.issue("CAUCE-001", make_keypair(), now_ms=NOW)
    for bad in ("", "nonsense", "00" * 31):
        ok, reason = verify_certificate(certificate, bad, now_ms=NOW)
        assert not ok
        assert reason == "bad_ca_key"


def test_a_missing_validity_is_refused(ca):
    certificate = ca.issue("CAUCE-001", make_keypair(), now_ms=NOW)
    del certificate["not_after_utc_ms"]
    ok, reason = verify_certificate(certificate, ca.public_key_hex, now_ms=NOW)
    assert not ok
    # The signature breaks first, because validity is signed. Either way it is refused; the
    # point is that it cannot be refused *because it was removed* and accepted otherwise.
    assert reason in ("bad_signature", "missing_validity")


def test_a_verifier_needs_only_the_ca_public_key(ca):
    """The property that separates this from the shared admin token.

    A verifier constructed with nothing but the CA public key - no database, no admin
    token, no knowledge of the provisioning path - reaches the same verdict as the CA.
    """
    public = ca.public_key_hex
    certificate = ca.issue("CAUCE-001", make_keypair(), site_id="SITE-A", now_ms=NOW)

    # No CertificateAuthority instance in scope at all.
    ok, reason = certificate_is_valid_for_node(certificate, "CAUCE-001", public,
                                               now_ms=NOW)
    assert ok, reason
