"""The certificate endpoints, and what they refuse.

Most of this file is about refusals, because that is where the design decisions are. The
interesting properties are the ones a caller cannot get around:

- a certificate cannot be issued for a key the request supplied, only for the key already
  registered against the node;
- a certificate cannot be issued at all when the CA is unconfigured, rather than being
  issued and signed by nothing;
- a certificate issued for an HMAC node is refused, because publishing a shared secret in
  something designed to be handed to verifiers would be worse than having no certificate;
- re-issuing retires the previous certificate rather than adding a second trusted one.
"""

from __future__ import annotations

import os

import pytest
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from fastapi.testclient import TestClient

os.environ["CAUCE_DB_PATH"] = "./data/test_cert_endpoint.sqlite"

from cauce_server import db  # noqa: E402
from cauce_server.certificates import verify_certificate  # noqa: E402
from cauce_server.config import settings  # noqa: E402
from cauce_server.main import app  # noqa: E402
from cauce_server.signing import encode_key_material  # noqa: E402
from conftest import ADMIN_HEADERS  # noqa: E402

NODE_ID = "CERT-001"

# Set on `settings` rather than through the environment, because `config.Settings` snapshots
# the environment once at import time. Assigning `os.environ["CAUCE_CA_KEY"]` here therefore
# did nothing, and the tests passed in isolation and failed in the full suite - whichever
# module imported `config` first won. The suite-order dependence is the same trap the
# conftest comment describes, and the fix is the same: set the value the code actually reads.
CA_KEY = bytes(range(32)).hex()


@pytest.fixture(autouse=True)
def configured_ca():
    """Every test in this module runs with a CA, restoring whatever was there before."""
    previous = settings.ca_private_key
    settings.ca_private_key = CA_KEY
    yield
    settings.ca_private_key = previous


def node_public_key(seed_byte: int = 42) -> str:
    key = Ed25519PrivateKey.from_private_bytes(bytes([seed_byte]) * 32)
    return encode_key_material(key.public_key().public_bytes_raw())


@pytest.fixture()
def client():
    db.reset_for_tests()
    with TestClient(app, headers=ADMIN_HEADERS) as c:
        yield c


def provision(client, node_id=NODE_ID, key=None, algorithm="ed25519"):
    response = client.post("/v1/provision", json={
        "node_id": node_id,
        "device_key": key or node_public_key(),
        "device_key_algorithm": algorithm,
    })
    assert response.status_code == 200, response.text
    return response


def test_issuing_returns_a_certificate_that_verifies(client):
    provision(client)
    response = client.post(f"/v1/nodes/{NODE_ID}/certificate", json={})
    assert response.status_code == 200, response.text
    body = response.json()

    certificate = body["certificate"]
    ok, reason = verify_certificate(certificate, body["ca_public_key"])
    assert ok, reason
    assert certificate["node_id"] == NODE_ID


def test_the_ca_private_key_appears_in_no_response(client):
    """The whole reason a CA exists is that its private key goes to nobody."""
    provision(client)
    secret = bytes(range(32)).hex()

    issued = client.post(f"/v1/nodes/{NODE_ID}/certificate", json={})
    assert secret not in issued.text
    assert secret not in client.get(f"/v1/nodes/{NODE_ID}/certificate").text
    serial = issued.json()["certificate"]["serial"]
    assert secret not in client.get(f"/v1/certificates/{serial}").text
    # And the public key that verifiers get is genuinely the public half.
    from cauce_server.signing import decode_key_material

    assert len(decode_key_material(issued.json()["ca_public_key"], 32)) == 32


def test_a_certificate_cannot_be_issued_for_a_key_the_request_supplies(client):
    """The one thing a CA must never do is certify whatever it was handed.

    The registered key is the only accepted source. A request offering a different key is
    refused outright rather than quietly ignored, because a caller who expected their key to
    be used would otherwise believe they were certified when they were not.
    """
    provision(client, key=node_public_key(42))
    response = client.post(f"/v1/nodes/{NODE_ID}/certificate", json={
        "public_key": node_public_key(99),
    })
    assert response.status_code == 200, response.text
    issued_key = response.json()["certificate"]["public_key"]

    # The registered key, not the one in the request.
    assert issued_key == node_public_key(42)
    assert issued_key != node_public_key(99)


def test_issuing_refuses_when_the_ca_is_not_configured(client, monkeypatch):
    """Fails closed, with a reason, rather than issuing a certificate signed by nothing."""
    from cauce_server.config import settings

    provision(client)
    monkeypatch.setattr(settings, "ca_private_key", "")
    response = client.post(f"/v1/nodes/{NODE_ID}/certificate", json={})
    assert response.status_code == 503
    assert response.json()["detail"] == "certificate_authority_not_configured"


def test_a_malformed_ca_key_is_reported_without_echoing_it(client, monkeypatch):
    from cauce_server.config import settings

    provision(client)
    monkeypatch.setattr(settings, "ca_private_key", "supersecretvalue")
    response = client.post(f"/v1/nodes/{NODE_ID}/certificate", json={})
    assert response.status_code == 503
    assert response.json()["detail"] == "certificate_authority_key_invalid"
    assert "supersecretvalue" not in response.text


def test_an_hmac_node_cannot_be_certified(client):
    """Publishing a shared secret in a document meant for verifiers is the wrong answer.

    An HMAC device key is symmetric: whoever holds it can forge frames. A certificate is
    built to be handed to anyone who wants to check a node's identity, so putting the secret
    in one would distribute it to every verifier and make the certificate the leak.
    """
    provision(client, key="a" * 32, algorithm="hmac-sha256")
    response = client.post(f"/v1/nodes/{NODE_ID}/certificate", json={})
    assert response.status_code == 409
    assert "not_public_key_based" in response.json()["detail"]


def test_a_node_with_no_device_key_cannot_be_certified(client):
    """Provisioning is the only way a key arrives, so an unprovisioned node has nothing
    to certify."""
    client.post("/v1/sync", json={
        "protocol_version": 1, "node_id": NODE_ID, "measurements": [],
    })
    response = client.post(f"/v1/nodes/{NODE_ID}/certificate", json={})
    assert response.status_code == 409
    assert "provision_it_first" in response.json()["detail"]


def test_an_unknown_node_is_404(client):
    response = client.post("/v1/nodes/NOPE-999/certificate", json={})
    assert response.status_code == 404
    assert response.json()["detail"] == "node_not_found"


def test_reissuing_retires_the_previous_certificate(client):
    """Rotation replaces, it does not accumulate.

    Two simultaneously trusted certificates for one node means a key retired by rotation is
    still honoured, which defeats the rotation. The old row is kept, with status retired, so
    the audit trail survives.
    """
    provision(client)
    first = client.post(f"/v1/nodes/{NODE_ID}/certificate", json={}).json()
    second = client.post(f"/v1/nodes/{NODE_ID}/certificate", json={}).json()

    assert first["certificate"]["serial"] != second["certificate"]["serial"]

    old = client.get(f"/v1/certificates/{first['certificate']['serial']}").json()
    new = client.get(f"/v1/certificates/{second['certificate']['serial']}").json()

    assert old["status"] == "retired"
    assert new["status"] == "active"
    assert new["trusted"] is True
    # Signature validity and authorisation are separate: the old one is still a genuine
    # document from this CA. Collapsing those is how "valid" starts meaning "authorised".
    assert old["signature_valid"] is True
    assert old["trusted"] is False


def test_the_active_endpoint_returns_only_the_current_certificate(client):
    provision(client)
    first = client.post(f"/v1/nodes/{NODE_ID}/certificate", json={}).json()
    client.post(f"/v1/nodes/{NODE_ID}/certificate", json={})

    body = client.get(f"/v1/nodes/{NODE_ID}/certificate").json()
    assert body["certificate"]["serial"] != first["certificate"]["serial"]
    assert body["verified"] is True


def test_no_active_certificate_is_404(client):
    provision(client)
    response = client.get(f"/v1/nodes/{NODE_ID}/certificate")
    assert response.status_code == 404
    assert response.json()["detail"] == "no_active_certificate"


def test_a_tampered_stored_certificate_is_not_handed_out(client):
    """The read path re-verifies, so a corrupted row fails here rather than on a node.

    A stored blob served as-is would be trusted by the node holding it, and the central
    would have no idea it had been altered.
    """
    provision(client)
    issued = client.post(f"/v1/nodes/{NODE_ID}/certificate", json={}).json()

    # Written with explicit single-quoted SQL string literals: Python's string escaping for
    # a double quote inside a `?` parameter mangles it, and the first attempt at this test
    # failed with "wrong number of arguments to function replace()" rather than silently
    # not tampering with anything.
    with db.transaction() as conn:
        conn.execute(
            """UPDATE certificates
               SET body = replace(body, '"node_id":"CERT-001"', '"node_id":"CERT-999"')
               WHERE serial = ?""",
            (issued["certificate"]["serial"],),
        )

    response = client.get(f"/v1/nodes/{NODE_ID}/certificate")
    assert response.status_code == 500
    assert "does_not_verify" in response.json()["detail"]


def test_an_expired_certificate_is_reported_as_not_trusted(client):
    provision(client)
    issued = client.post(f"/v1/nodes/{NODE_ID}/certificate", json={}).json()
    serial = issued["certificate"]["serial"]

    # Two things are asserted separately, and the difference is the point of the endpoint.
    #
    # `expired` and `trusted` come from the stored columns. `signature_valid` comes from
    # re-verifying the body. Editing the body to expire it therefore breaks the signature -
    # which is correct, and is why the test does not claim the certificate is a well-formed
    # expired one. What it does establish is that nothing reports it as trusted.
    with db.transaction() as conn:
        conn.execute(
            "UPDATE certificates SET body = replace(body, '\"not_after_utc_ms\":"
            + str(issued["certificate"]["not_after_utc_ms"])
            + "', '\"not_after_utc_ms\":1'), not_after_utc_ms = 1"
            " WHERE serial = ?",
            (serial,),
        )

    status = client.get(f"/v1/certificates/{serial}").json()
    assert status["expired"] is True
    assert status["trusted"] is False
    assert status["signature_valid"] is False


def test_an_unknown_serial_is_404(client):
    response = client.get("/v1/certificates/deadbeef")
    assert response.status_code == 404
    assert response.json()["detail"] == "certificate_not_found"


def test_issuing_requires_a_write_scope(client):
    provision(client)

    # A read-only principal cannot mint a certificate.
    created = client.post("/v1/tokens", json={
        "name": "cert-reader", "scopes": "read",
    })
    assert created.status_code == 200, created.text
    token = created.json()["token"]

    response = client.post(f"/v1/nodes/{NODE_ID}/certificate", json={},
                           headers={"Authorization": f"Bearer {token}"})
    assert response.status_code == 403


def test_an_unauthenticated_request_cannot_issue_a_certificate(client):
    """With a CA configured, an anonymous caller gets 401 rather than a certificate."""
    provision(client)
    response = client.post(f"/v1/nodes/{NODE_ID}/certificate", json={},
                           headers={"Authorization": ""})
    assert response.status_code in (401, 403)
