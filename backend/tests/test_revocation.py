"""Retiring a node's identity.

The mechanism has to hold against the case it exists for: a device that is still
present, still has its key, and keeps sending perfectly valid signed frames.
"""

from __future__ import annotations

import hashlib
import hmac
import os

import pytest
from fastapi.testclient import TestClient

os.environ["CAUCE_DB_PATH"] = "./data/test_revocation.sqlite"

from cauce_server import db  # noqa: E402
from cauce_server.config import settings  # noqa: E402
from cauce_server.lora_frames import (  # noqa: E402
    fragment_batch,
    make_record,
    sign_frame,
)
from cauce_server.main import app  # noqa: E402

DEVICE_KEY = "clave-dispositivo-0123456789"
OTHER_KEY = "otra-clave-del-atacante-9999"
RADIO_BUDGET = 115
ADMIN = {"Authorization": "Bearer admin-token"}
WRITE = {"Authorization": "Bearer write-token"}


@pytest.fixture()
def client(monkeypatch):
    # With no shared token configured, every scope check passes trivially and a
    # `write` credential could retire a device. Auth has to be on for the scoping
    # tests below to mean anything.
    monkeypatch.setattr(settings, "api_token", "admin-token")
    db.reset_for_tests()
    with TestClient(app) as c:
        yield c


def provision(client, node_id, key=DEVICE_KEY, algorithm=None):
    body = {"node_id": node_id, "device_key": key}
    if algorithm is not None:
        body["device_key_algorithm"] = algorithm
    response = client.post("/v1/provision", json=body, headers=ADMIN)
    assert response.status_code == 200, response.text
    return response.json()


def wifi_sync(client, node_id, key, sequence=1):
    import json

    payload = {
        "protocol_version": 1,
        "node_id": node_id,
        "measurements": [
            {
                "sequence": sequence,
                "timestamp_utc_ms": 1787356800000,
                "variable": "air_temperature",
                "value": 21.5,
                "unit": "degC",
                "quality": "VALID",
            }
        ],
    }
    # The central signs and verifies the raw request body, so the test sends exactly
    # the bytes it signed rather than letting the client re-serialise them.
    raw = json.dumps(payload).encode()
    headers = dict(ADMIN)
    headers["Content-Type"] = "application/json"
    headers["X-Cauce-Node"] = node_id
    headers["X-Cauce-Signature"] = hmac.new(
        key.encode(), raw, hashlib.sha256).hexdigest()
    return client.post("/v1/sync", content=raw, headers=headers)


def lora_sync(client, node_id, key, batch_id=1000):
    import base64

    batch = [make_record(1, 1787356800000, 21.5, node_id)]
    frames = [sign_frame(f, key)
              for f in fragment_batch(batch, batch_id=batch_id,
                                      budget=RADIO_BUDGET)]
    payload = {
        "protocol_version": 1,
        "node_id": node_id,
        "transport": "lora",
        "batch_id": batch_id,
        "record_count": len(batch),
        "frames": [base64.b64encode(f).decode("ascii") for f in frames],
    }
    return client.post("/v1/sync", json=payload, headers=ADMIN)


# --- the mechanism works ---------------------------------------------------

def test_a_retired_node_is_refused(client):
    provision(client, "RETIRE-1")
    assert wifi_sync(client, "RETIRE-1", DEVICE_KEY).status_code == 200

    response = client.post("/v1/nodes/RETIRE-1/revoke", json={"reason": "sold"},
                           headers=ADMIN)
    assert response.status_code == 200, response.text
    assert response.json()["retired"] is True

    # The key still works. That is the point: retirement must not depend on the
    # node cooperating or on its key being wrong.
    refused = wifi_sync(client, "RETIRE-1", DEVICE_KEY)
    assert refused.status_code == 403, refused.text
    assert refused.json()["detail"]["error"] == "node_retired"


def test_a_refusal_names_when_and_why(client):
    provision(client, "RETIRE-2")
    client.post("/v1/nodes/RETIRE-2/revoke", json={"reason": "water ingress"},
                headers=ADMIN)
    refused = wifi_sync(client, "RETIRE-2", DEVICE_KEY)
    assert refused.status_code == 403, refused.text
    detail = refused.json()["detail"]
    assert detail["reason"] == "water ingress"
    assert detail["retired_at_utc_ms"] > 0


def test_a_retired_nodes_measurements_are_kept(client):
    """Retirement withdraws the right to contribute, not the record of what was
    already contributed. A compromised device still made real observations during
    part of its compromise, and deleting them destroys the evidence too."""
    provision(client, "RETIRE-3")
    assert wifi_sync(client, "RETIRE-3", DEVICE_KEY).status_code == 200
    client.post("/v1/nodes/RETIRE-3/revoke", json={"reason": "key exposed"},
                headers=ADMIN)

    rows = client.get("/v1/nodes/RETIRE-3/measurements", headers=ADMIN)
    assert rows.status_code == 200, rows.text
    body = rows.json()
    measurements = body.get("measurements", body.get("items", []))
    assert len(measurements) == 1


def test_retiring_twice_is_not_an_error(client):
    provision(client, "RETIRE-4")
    first = client.post("/v1/nodes/RETIRE-4/revoke", json={"reason": "first"},
                        headers=ADMIN)
    second = client.post("/v1/nodes/RETIRE-4/revoke", json={"reason": "second"},
                         headers=ADMIN)
    assert first.status_code == 200, first.text
    assert second.status_code == 200, second.text
    # The original retirement stands. Refusing the second call would only teach the
    # operator to catch an exception.
    assert wifi_sync(client, "RETIRE-4", DEVICE_KEY).status_code == 403


# --- the mistake that matters ----------------------------------------------

def test_retiring_an_unknown_node_id_still_records_it(client):
    """A retirement that silently does nothing because the id was typed wrong is
    the failure mode that matters: the operator walks away believing the device is
    off the network."""
    response = client.post("/v1/nodes/NEVER-SEEN/revoke", json={"reason": "typo?"},
                           headers=ADMIN)
    assert response.status_code == 200, response.text
    assert response.json()["created"] is True
    assert response.json()["retired"] is True

    state = client.get("/v1/nodes/NEVER-SEEN/revocation", headers=ADMIN)
    assert state.json()["retired"] is True


# --- reinstatement is a rotation, not a flag -------------------------------

def test_reprovisioning_with_a_new_key_reinstates_the_node(client):
    old_key = "clave-antigua-0123456789012"
    new_key = "clave-nueva-901234567890123"
    provision(client, "ROTATE-1", old_key)
    client.post("/v1/nodes/ROTATE-1/revoke", json={"reason": "rotating"},
                headers=ADMIN)
    assert wifi_sync(client, "ROTATE-1", old_key).status_code == 403

    result = provision(client, "ROTATE-1", new_key)
    assert result["reinstated"] is True

    # The old key does not work and the new one does. An endpoint that only flipped
    # a flag would leave the compromised key live.
    assert wifi_sync(client, "ROTATE-1", old_key).status_code != 200
    assert wifi_sync(client, "ROTATE-1", new_key).status_code == 200


def test_there_is_no_un_revoke_endpoint(client):
    for path in ("/v1/nodes/ROTATE-1/unrevoke",
                 "/v1/nodes/ROTATE-1/reinstate",
                 "/v1/nodes/ROTATE-1/un-revoke"):
        response = client.post(path, json={}, headers=ADMIN)
        assert response.status_code == 404, path


def test_the_revocation_read_says_how_to_reinstate(client):
    provision(client, "ROTATE-2")
    body = client.get("/v1/nodes/ROTATE-2/revocation", headers=ADMIN).json()
    assert body["retired"] is False
    assert body["reinstate_by"] == "POST /v1/provision"


# --- scoping ---------------------------------------------------------------

def test_write_scope_cannot_retire_a_device(client):
    """Retiring removes a device's ability to report, which is a bigger hammer than
    changing a setting. A `write` token must not reach it."""
    provision(client, "SCOPE-1")
    issued = client.post("/v1/tokens", headers=ADMIN, json={
        "name": "operator",
        "token": "write-scope-token-0123456789",
        "scopes": "read,write",
    })
    assert issued.status_code in (200, 201), issued.text
    write = {"Authorization": "Bearer write-scope-token-0123456789"}

    response = client.post("/v1/nodes/SCOPE-1/revoke", json={}, headers=write)
    assert response.status_code == 403, response.text
    # And the node still reports, so the refusal was a refusal and not an accident.
    assert wifi_sync(client, "SCOPE-1", DEVICE_KEY).status_code == 200


def test_anonymous_cannot_retire_a_device(client):
    provision(client, "SCOPE-2")
    assert client.post("/v1/nodes/SCOPE-2/revoke", json={}).status_code == 401


# --- the relayed path is closed too ----------------------------------------

def test_a_retired_lora_node_is_refused_too(client):
    """The relayed path must be closed as well. Checking only the Wi-Fi path would
    leave a compromised node reporting through a gateway."""
    lora_key = "clave-lora-del-dispositivo-01"
    provision(client, "LORA-RETIRE", lora_key)
    assert lora_sync(client, "LORA-RETIRE", lora_key).status_code == 200

    client.post("/v1/nodes/LORA-RETIRE/revoke", json={"reason": "stolen"},
                headers=ADMIN)

    refused = lora_sync(client, "LORA-RETIRE", lora_key)
    assert refused.status_code == 403, refused.text
    assert refused.json()["detail"]["error"] == "node_retired"
