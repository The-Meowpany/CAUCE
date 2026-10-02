from __future__ import annotations

import os

import pytest
from fastapi.testclient import TestClient

os.environ["CAUCE_DB_PATH"] = "./data/test_commands.sqlite"

from cauce_server import db  # noqa: E402
from cauce_server.config import settings  # noqa: E402
from cauce_server.main import app  # noqa: E402
from test_api import BASE_TS, _series, sync_payload  # noqa: E402


@pytest.fixture()
def client():
    db.reset_for_tests()
    with TestClient(app) as c:
        yield c


def _register(client, node_id="CAUCE-001", count=4, start_seq=1):
    payload = sync_payload(_series(node_id, BASE_TS, count, start_seq=start_seq),
                           node_id=node_id)
    return client.post("/v1/sync", json=payload)


def _send(client, node_id="CAUCE-001", count=1, start_seq=1, **extra):
    body = sync_payload(_series(node_id, BASE_TS, count, start_seq=start_seq),
                        node_id=node_id)
    body.update(extra)
    return client.post("/v1/sync", json=body)


def _queue(client, node_id="CAUCE-001", kind="request_resync", key="k1",
           **extra):
    body = {"kind": kind, "idempotency_key": key}
    body.update(extra)
    return client.post(f"/v1/nodes/{node_id}/commands", json=body)


def test_command_is_queued_then_delivered_then_acked(client):
    _register(client)
    posted = _queue(client, key="cadence", kind="set_sampling_interval",
                    payload={"seconds": 120}).json()
    assert posted["status"] == "queued"
    assert posted["created"] is True
    command_id = posted["command_id"]
    assert "not delivered" in posted["note"]

    listing = client.get("/v1/nodes/CAUCE-001/commands").json()
    assert listing["commands"][0]["state"] == "pending"
    assert listing["commands"][0]["delivered_utc_ms"] is None

    polled = _send(client, count=2, start_seq=5).json()
    assert len(polled["commands"]) == 1
    assert polled["commands"][0]["command_id"] == command_id
    assert polled["commands"][0]["kind"] == "set_sampling_interval"
    assert '"seconds":120' in polled["commands"][0]["payload"]

    # Unacked, so it comes back: at-least-once is the whole contract.
    assert len(_send(client, count=1, start_seq=8).json()["commands"]) == 1

    settled = _send(client, count=1, start_seq=9,
                    command_receipts=[{"command_id": command_id,
                                       "state": "acked",
                                       "detail": "applied 120"}]).json()
    assert settled["commands"] == []

    final = client.get("/v1/nodes/CAUCE-001/commands").json()["commands"][0]
    assert final["state"] == "acked"
    assert final["result"] == "applied 120"


def test_same_idempotency_key_is_never_queued_twice(client):
    _register(client)
    first = _queue(client, key="resync-1").json()
    second = _queue(client, key="resync-1").json()
    assert first["created"] is True
    assert second["created"] is False
    assert second["status"] == "already_queued"
    assert first["command_id"] == second["command_id"]
    assert len(_send(client).json()["commands"]) == 1


def test_reusing_a_key_for_a_different_command_is_refused(client):
    _register(client)
    _queue(client, key="k1", kind="request_resync")
    clash = _queue(client, key="k1", kind="set_led_mode")
    assert clash.status_code == 409
    assert "idempotency_key_reused" in str(clash.json())


def test_a_receipt_for_another_node_is_ignored(client):
    _register(client, node_id="CAUCE-001")
    _register(client, node_id="CAUCE-002", count=3)
    foreign = _queue(client, node_id="CAUCE-002", key="x").json()

    _send(client, node_id="CAUCE-001",
          command_receipts=[{"command_id": foreign["command_id"],
                             "state": "acked"}])

    row = client.get("/v1/nodes/CAUCE-002/commands").json()["commands"][0]
    assert row["state"] == "pending"
    assert row["acked_utc_ms"] is None


def test_expired_commands_stop_being_offered(client):
    _register(client)
    _queue(client, key="ttl", ttl_ms=1000)
    with db.transaction() as conn:
        conn.execute("UPDATE commands SET expires_at_utc_ms=?", (BASE_TS - 1,))
    assert _send(client).json()["commands"] == []
    row = client.get("/v1/nodes/CAUCE-001/commands").json()["commands"][0]
    assert row["state"] == "expired"


def test_delivered_receipt_is_distinct_from_acked(client):
    _register(client)
    command_id = _queue(client, kind="set_sync_interval", key="d").json()[
        "command_id"]
    _send(client, command_receipts=[{"command_id": command_id,
                                     "state": "delivered"}])
    row = client.get("/v1/nodes/CAUCE-001/commands").json()["commands"][0]
    assert row["state"] == "delivered"
    assert row["delivered_utc_ms"] is not None
    assert row["acked_utc_ms"] is None
    assert len(_send(client).json()["commands"]) == 1


def test_command_validation(client):
    _register(client)
    assert _queue(client, kind="self_destruct").status_code == 422
    assert _queue(client, key="").status_code == 422
    assert _queue(client, key="x" * 65).status_code == 422
    assert _queue(client, key="p", payload="notadict").status_code == 422
    assert _queue(client, key="t", ttl_ms=0).status_code == 422
    assert _queue(client, key="b", payload={"blob": "x" * 2000}
                  ).status_code == 422
    assert _queue(client, node_id="NOPE").status_code == 404


def test_commands_require_the_admin_token_when_one_is_configured(
        client, monkeypatch):
    _register(client)
    monkeypatch.setattr(settings, "api_token", "admin-token")
    assert _queue(client, key="k").status_code == 401
    ok = client.post("/v1/nodes/CAUCE-001/commands",
                     headers={"Authorization": "Bearer admin-token"},
                     json={"kind": "request_resync", "idempotency_key": "k"})
    assert ok.status_code == 200


def test_malformed_receipts_are_rejected_not_trusted(client):
    _register(client)
    bad = _send(client, command_receipts="nope")
    assert bad.status_code == 422
    # Structurally wrong entries are skipped rather than fatal, and the sync
    # itself still succeeds.
    ok = _send(client, start_seq=2,
               command_receipts=[{"command_id": "x", "state": "acked"},
                                 {"command_id": 999, "state": "weird"}])
    assert ok.status_code == 200


def test_sync_response_always_carries_the_commands_field(client):
    _register(client)
    assert _send(client, start_seq=6).json()["commands"] == []


def test_healthz_reports_the_new_tables(client):
    body = client.get("/healthz").json()
    assert body["tables"]["commands"] == 0
    assert body["tables"]["calibration"] == 0
    assert body["tables"]["maintenance_events"] == 0
