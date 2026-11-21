"""CA-vouched peer credentials, the compact form a node broadcasts over ESP-NOW.

firmware/test/test_peer_driver.cpp verifies the same 120 bytes with the node's own Ed25519, so
the issuing side and the verifying side cannot disagree on the layout without a failure.
"""

from __future__ import annotations

from cauce_server import certificates
from test_api import client  # noqa: F401  (fixture)

RFC8032_TEST2_SEED = "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb"
RFC8032_TEST1_PUBLIC = "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a"
VECTOR = (
    "43415543452d30303200000000000000d75a980182b10ab7d54bfed3c964073a"
    "0ee172f3daa62325af021a68f707511a00505c18a30100004f84a65672b062b1"
    "c6c8915c26c4448ca27755271bb6104d57f4de3edd85829e69ee0466c946214f"
    "2d3119f6322b556c266f2ca58d20f97add893dfdfa0c2703")


def test_the_ca_issues_the_firmware_known_answer():
    ca = certificates.CertificateAuthority(RFC8032_TEST2_SEED)
    assert ca.peer_credential("CAUCE-002", RFC8032_TEST1_PUBLIC, 1800000000000) == VECTOR


def test_a_credential_verifies_and_a_tampered_one_does_not():
    ca = certificates.CertificateAuthority(RFC8032_TEST2_SEED)
    raw = bytes.fromhex(VECTOR)
    assert certificates.verify_peer_credential(raw, ca.public_key_hex) == {
        "node_id": "CAUCE-002", "public_key": RFC8032_TEST1_PUBLIC,
        "not_after_utc_ms": 1800000000000}
    tampered = bytearray(raw)
    tampered[20] ^= 1
    assert certificates.verify_peer_credential(bytes(tampered), ca.public_key_hex) is None


def test_issuing_a_certificate_also_returns_the_peer_credential(client, monkeypatch):  # noqa: F811
    from cauce_server.config import settings

    monkeypatch.setattr(settings, "ca_private_key", RFC8032_TEST2_SEED)
    client.post("/v1/provision", json={"node_id": "CAUCE-002",
                                       "device_key": RFC8032_TEST1_PUBLIC,
                                       "device_key_algorithm": "ed25519"})
    body = client.post("/v1/nodes/CAUCE-002/certificate", json={}).json()
    decoded = certificates.verify_peer_credential(bytes.fromhex(body["peer_credential"]),
                                                  body["ca_public_key"])
    assert decoded["node_id"] == "CAUCE-002"
    assert decoded["public_key"] == RFC8032_TEST1_PUBLIC
    assert decoded["not_after_utc_ms"] == body["not_after_utc_ms"]
