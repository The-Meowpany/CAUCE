"""Certificate authentication over HTTP, end to end against the real app.

The unit tests in `test_node_auth.py` cover the scheme. These cover the wiring, which is
where a correct scheme stops being enforced:

- the challenge endpoint's own refusals, which are the open-endpoint's whole risk;
- that `/v1/sync` honours a certificate and still honours an HMAC node;
- and above all that **a node presenting a certificate cannot fall back to the shared
  secret**, because a fallback would leave the weaker scheme permanently available and make
  the stronger one decorative.

That last one is asserted with a request that carries a valid HMAC signature *and* a bad
certificate, because that is the exact shape an attacker would send.
"""

from __future__ import annotations

import base64
import hashlib
import hmac
import json
import os

import pytest
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from fastapi.testclient import TestClient

os.environ["CAUCE_DB_PATH"] = "./data/test_node_auth_endpoint.sqlite"

from cauce_server import db  # noqa: E402
from cauce_server.config import settings  # noqa: E402
from cauce_server.main import app  # noqa: E402
from conftest import ADMIN_HEADERS  # noqa: E402

NODE = "CAUCE-AUTH"
BASE_TS = 1_787_356_800_000

CA_KEY = bytes(range(32)).hex()


@pytest.fixture(autouse=True)
def configured_ca():
    """Installs the CA on `settings`, not in the environment.

    `config.Settings` reads the environment once, when the package is first imported, and
    conftest is imported before any test module - so an `os.environ["CAUCE_CA_KEY"]` at the top
    of this file does nothing at all. It passed alone and failed twelve tests in the full
    suite, because whichever module triggered the first `cauce_server` import decided the CA
    key for every module after it.

    The same trap as the one in `tests/conftest.py`; the fix is the same. A setting nothing
    resets is set on `settings`.
    """
    previous = settings.ca_private_key
    settings.ca_private_key = CA_KEY
    yield
    settings.ca_private_key = previous


@pytest.fixture()
def client():
    db.reset_for_tests()
    with TestClient(app, headers=ADMIN_HEADERS) as c:
        yield c


def key_pair(seed_byte=7):
    key = Ed25519PrivateKey.from_private_bytes(bytes([seed_byte]) * 32)
    return key, key.public_key().public_bytes_raw().hex()


def provision_ed25519(client, node_id=NODE, seed_byte=7):
    _, public = key_pair(seed_byte)
    response = client.post("/v1/provision", json={
        "node_id": node_id, "device_key": public,
        "device_key_algorithm": "ed25519",
    })
    assert response.status_code == 200, response.text
    return key_pair(seed_byte)


def provision_hmac(client, node_id="CAUCE-HMAC", secret="a" * 32):
    response = client.post("/v1/provision", json={
        "node_id": node_id, "device_key": secret,
    })
    assert response.status_code == 200, response.text
    return secret


def body_bytes(node_id=NODE, seq=1):
    """The exact bytes `/v1/sync` signs, so a test cannot drift from the endpoint."""
    payload = {
        "protocol_version": 1,
        "node_id": node_id,
        "measurements": [{
            "sequence": seq, "timestamp_utc_ms": BASE_TS,
            "variable": "air_temperature", "value": 20.0,
            "unit": "degC", "quality": "VALID",
        }],
    }
    return json.dumps(payload).encode("utf-8"), payload


def challenge(client, node_id=NODE):
    response = client.post("/v1/sync/challenge", json={"node_id": node_id})
    return response


def certificate_for(client, node_id=NODE, seed_byte=7):
    response = client.post(f"/v1/nodes/{node_id}/certificate", json={})
    assert response.status_code == 200, response.text
    return response.json()["certificate"]


# --- the challenge endpoint --------------------------------------------

def test_a_challenge_names_the_exact_bytes_to_sign(client):
    """Publishing the payload means the firmware does not reimplement the encoding.

    The encoding is still pinned by a unit test; this is the copy the node actually gets, and
    the two are checked against each other here so a change to one cannot silently orphan the
    other.
    """
    provision_ed25519(client)
    response = challenge(client)
    assert response.status_code == 200, response.text
    body = response.json()
    assert body["node_id"] == NODE
    assert body["sign_this"] == (
        f"cauce-sync-challenge\n{NODE}\n{body['nonce']}\n{body['expires_utc_ms']}"
    )
    assert body["algorithm"] == "ed25519"
    assert body["expires_utc_ms"] > body["nonce"].__len__() * 0  # a real expiry, not absent


def test_an_unknown_node_gets_404(client):
    assert challenge(client, "CAUCE-NOBODY").status_code == 404


def test_a_retired_node_gets_no_challenge(client):
    """A retired node must not be able to authenticate, and must not learn it by asking.

    Answering the question here is what lets a node find out it was retired without a key,
    which is a small leak; refusing at answer time only would need the key first.
    """
    key, _ = key_pair()
    provision_ed25519(client)
    assert client.post(f"/v1/nodes/{NODE}/revoke", json={"reason": "test"}).status_code == 200
    response = challenge(client)
    assert response.status_code == 403
    assert response.json()["detail"] == "node_retired"


def test_an_hmac_node_is_told_it_cannot_answer_a_challenge(client):
    """Saying so is more useful than a nonce that could never succeed.

    The node's algorithm is not a secret - it is in every frame it sends - so the refusal
    leaks nothing that was not already public.
    """
    provision_hmac(client, "CAUCE-HMAC")
    response = challenge(client, "CAUCE-HMAC")
    assert response.status_code == 409
    assert "cannot_answer_a_challenge" in response.json()["detail"]


def test_a_challenge_is_refused_when_no_ca_is_configured(client, monkeypatch):
    provision_ed25519(client)
    monkeypatch.setattr(settings, "ca_private_key", "")
    response = challenge(client)
    assert response.status_code == 503
    assert response.json()["detail"] == "certificate_authority_not_configured"


# --- /v1/sync with a certificate ---------------------------------------

def cert_headers(key, certificate, nonce_body):
    """Headers a node sends: the certificate, the signed nonce, and the signature."""
    signature = key.sign(nonce_body["sign_this"].encode("utf-8"))
    return {
        "X-Cauce-Node": NODE,
        "X-Cauce-Certificate": base64.b64encode(
            json.dumps(certificate).encode()).decode(),
        "X-Cauce-Nonce": nonce_body["nonce"],
        "X-Cauce-Nonce-Signature": base64.b64encode(signature).decode(),
    }


def test_a_certificate_authenticated_sync_is_accepted(client):
    key, _ = key_pair()
    provision_ed25519(client)
    certificate = certificate_for(client)
    issued = challenge(client).json()
    raw, payload = body_bytes()

    headers = {**ADMIN_HEADERS, **cert_headers(key, certificate, issued)}
    response = client.post("/v1/sync", content=raw, headers=headers)
    assert response.status_code == 200, response.text
    assert response.json()["received"] == 1
    assert response.json()["acknowledged_sequence"] == 1


def test_certificate_auth_works_without_the_hmac_secret_at_all(client):
    """The whole point: the node proves possession, so it sends no shared secret.

    A request that carries a certificate and *no* `X-Cauce-Signature` must be accepted. If
    this ever needed the HMAC header as well, certificate authentication would be an
    embellishment on the old scheme rather than a replacement for it.
    """
    key, _ = key_pair()
    provision_ed25519(client)
    certificate = certificate_for(client)
    issued = challenge(client).json()
    raw, _ = body_bytes()

    headers = cert_headers(key, certificate, issued)
    assert "X-Cauce-Signature" not in headers
    response = client.post("/v1/sync", content=raw,
                           headers={**ADMIN_HEADERS, **headers})
    assert response.status_code == 200, response.text


def test_a_replayed_request_is_refused(client):
    """Neither the certificate nor the signature changes, so single-use is the whole defence."""
    key, _ = key_pair()
    provision_ed25519(client)
    certificate = certificate_for(client)
    issued = challenge(client).json()
    raw, _ = body_bytes()
    headers = {**ADMIN_HEADERS, **cert_headers(key, certificate, issued)}

    assert client.post("/v1/sync", content=raw, headers=headers).status_code == 200
    replay = client.post("/v1/sync", content=raw, headers=headers)
    assert replay.status_code == 401
    assert replay.json()["detail"] == "certificate_authentication_failed"


def test_a_failed_attempt_still_consumes_the_nonce(client):
    """A rejected attempt costs the caller a challenge, so it cannot be brute-forced cheaply.

    The nonce is spent before the certificate is examined, so this holds whether the
    certificate was wrong, expired, or for another node.
    """
    key, _ = key_pair()
    provision_ed25519(client)
    certificate = certificate_for(client)
    issued = challenge(client).json()
    raw, _ = body_bytes()

    # Wrong signature.
    headers = cert_headers(key, certificate, issued)
    headers["X-Cauce-Nonce-Signature"] = base64.b64encode(b"\x00" * 64).decode()
    assert client.post("/v1/sync", content=raw,
                       headers={**ADMIN_HEADERS, **headers}).status_code == 401

    # Same nonce, now with the right signature: still refused, because it was consumed.
    headers = cert_headers(key, certificate, issued)
    assert client.post("/v1/sync", content=raw,
                       headers={**ADMIN_HEADERS, **headers}).status_code == 401


def test_a_bad_certificate_cannot_fall_back_to_the_hmac_secret(client):
    """The fallback that must not exist.

    An attacker who holds one node's HMAC key presents it *and* a broken certificate. If the
    endpoint accepts this, the certificate is decorative: the weaker scheme is always
    available and the stronger one never has to succeed.
    """
    key, public = key_pair()
    provision_ed25519(client)
    certificate = certificate_for(client)
    issued = challenge(client).json()
    raw, _ = body_bytes()

    # A perfectly valid HMAC over the body, using the key the central has on file.
    valid_hmac = hmac.new(public.encode(), raw, hashlib.sha256).hexdigest()
    headers = {**ADMIN_HEADERS, **cert_headers(key, certificate, issued)}
    headers["X-Cauce-Signature"] = valid_hmac
    # And break the certificate's signature.
    tampered = dict(certificate)
    tampered["signature"] = "00" * 64
    headers["X-Cause-Signature"] = headers.pop("X-Cause-Signature", "")
    headers["X-Cauce-Certificate"] = base64.b64encode(
        json.dumps(tampered).encode()).decode()

    response = client.post("/v1/sync", content=raw, headers=headers)
    assert response.status_code == 401, "a broken certificate fell back to the HMAC path"
    assert response.json()["detail"] == "certificate_authentication_failed"


def test_incomplete_certificate_headers_are_refused(client):
    """Presenting *any* certificate header commits the request to certificate auth.

    Half the evidence is not half the decision: if a request with a certificate and no nonce
    quietly used HMAC, the rule above would be a suggestion.
    """
    key, public = key_pair()
    provision_ed25519(client)
    certificate = certificate_for(client)
    raw, _ = body_bytes()
    valid_hmac = hmac.new(public.encode(), raw, hashlib.sha256).hexdigest()

    headers = {
        "X-Cauce-Node": NODE,
        "X-Cauce-Signature": valid_hmac,
        "X-Cauce-Certificate": base64.b64encode(
            json.dumps(certificate).encode()).decode(),
    }
    response = client.post("/v1/sync", content=raw,
                           headers={**ADMIN_HEADERS, **headers})
    assert response.status_code == 401
    assert response.json()["detail"] == "certificate_authentication_incomplete"


def test_an_undecodable_certificate_is_refused_not_500(client):
    key, _ = key_pair()
    provision_ed25519(client)
    issued = challenge(client).json()
    raw, _ = body_bytes()
    headers = {
        "X-Cauce-Node": NODE,
        "X-Cauce-Certificate": "not base64 at all",
        "X-Cauce-Nonce": issued["nonce"],
        "X-Cauce-Nonce-Signature": base64.b64encode(b"\x00" * 64).decode(),
    }
    response = client.post("/v1/sync", content=raw,
                           headers={**ADMIN_HEADERS, **headers})
    assert response.status_code == 401
    assert response.json()["detail"] == "invalid_certificate"


def test_a_retired_node_cannot_authenticate_with_a_certificate_it_already_holds(client):
    """Retirement must beat a valid certificate, not merely a valid signature."""
    key, _ = key_pair()
    provision_ed25519(client)
    certificate = certificate_for(client)
    issued = challenge(client).json()
    client.post(f"/v1/nodes/{NODE}/revoke", json={"reason": "test"})

    raw, _ = body_bytes()
    headers = cert_headers(key, certificate, issued)
    response = client.post("/v1/sync", content=raw,
                           headers={**ADMIN_HEADERS, **headers})
    assert response.status_code in (401, 403), response.status_code


# --- the HMAC path is untouched ----------------------------------------

def test_an_hmac_node_still_works_exactly_as_before(client):
    """Every node provisioned before certificates existed must keep working."""
    secret = provision_hmac(client, "CAUCE-HMAC")
    raw, _ = body_bytes(node_id="CAUCE-HMAC")
    signature = hmac.new(secret.encode(), raw, hashlib.sha256).hexdigest()
    response = client.post("/v1/sync", content=raw, headers={
        **ADMIN_HEADERS,
        "X-Cauce-Node": "CAUCE-HMAC",
        "X-Cauce-Signature": signature,
    })
    assert response.status_code == 200, response.text


def test_an_hmac_node_with_a_wrong_signature_is_still_refused(client):
    """The old check did not regress while the new one was added beside it."""
    provision_hmac(client, "CAUCE-HMAC")
    raw, _ = body_bytes(node_id="CAUCE-HMAC")
    response = client.post("/v1/sync", content=raw, headers={
        **ADMIN_HEADERS,
        "X-Cauce-Node": "CAUCE-HMAC",
        "X-Cauce-Signature": "00" * 32,
    })
    assert response.status_code == 401
    assert response.json()["detail"] == "invalid_signature"


def test_the_hmac_path_still_refuses_a_mismatched_identity_header(client):
    """The header/payload mismatch check survived."""
    secret = provision_hmac(client, "CAUCE-HMAC")
    raw, _ = body_bytes(node_id="CAUCE-HMAC")
    signature = hmac.new(secret.encode(), raw, hashlib.sha256).hexdigest()
    response = client.post("/v1/sync", content=raw, headers={
        **ADMIN_HEADERS,
        "X-Cauce-Node": "CAUCE-OTHER",
        "X-Cauce-Signature": signature,
    })
    assert response.status_code == 401


# --- rate limiting -----------------------------------------------------

def test_the_challenge_endpoint_is_rate_limited(client):
    """It is unauthenticated by design, so the limit is the boundary on that.

    The rate limiter keys on client address, and every request here comes from the same test
    client, so the budget is shared with the provisioning calls made first. Enough requests
    therefore cross whichever threshold the deployment uses.
    """
    provision_ed25519(client)
    statuses = {challenge(client).status_code for _ in range(300)}
    assert 429 in statuses, f"the challenge endpoint never rate limited: {statuses}"
