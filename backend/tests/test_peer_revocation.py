"""Retiring a node tells its neighbours to stop trusting its peer credential.

The central can refuse a retired node's syncs on its own, but a peer credential is checked by
the neighbours and stays signature-valid until it expires. The `revoke_peer` command carries
the expiry of the node's last certificate, which bounds every credential it can still present.
"""

from __future__ import annotations

import json

import pytest
from cauce_server import db
from cauce_server.config import settings
from conftest import ADMIN_HEADERS
from fastapi.testclient import TestClient
from test_api import BASE_TS, _series, sync_payload
from test_peer_credential import RFC8032_TEST1_PUBLIC, RFC8032_TEST2_SEED


@pytest.fixture()
def client(monkeypatch):
    from cauce_server.main import app

    db.reset_for_tests()
    monkeypatch.setattr(settings, "ca_private_key", RFC8032_TEST2_SEED)
    with TestClient(app, headers=ADMIN_HEADERS) as c:
        for node, site in (("CAUCE-001", "s1"), ("CAUCE-002", "s1"), ("CAUCE-003", "s1"),
                           ("CAUCE-009", "s2")):
            c.post("/v1/sites", json={"site_id": site})
            c.post("/v1/sync", json=sync_payload(_series(node, BASE_TS, 2, base=20.0),
                                                 node_id=node))
            with db.transaction() as conn:
                conn.execute("UPDATE nodes SET site_id=? WHERE node_id=?", (site, node))
        yield c


def _revoke_commands(client, node_id):
    rows = db.query("SELECT payload, expires_at_utc_ms FROM commands"
                    " WHERE node_id=? AND kind='revoke_peer'", (node_id,))
    return [dict(r) for r in rows]


def test_retiring_a_certified_node_reaches_its_site_neighbours(client):
    client.post("/v1/provision", json={"node_id": "CAUCE-002", "device_key": RFC8032_TEST1_PUBLIC,
                                       "device_key_algorithm": "ed25519"})
    cert = client.post("/v1/nodes/CAUCE-002/certificate", json={}).json()
    result = client.post("/v1/nodes/CAUCE-002/revoke", json={"reason": "stolen"}).json()
    assert result["peers_notified"] == 2

    for neighbour in ("CAUCE-001", "CAUCE-003"):
        (command,) = _revoke_commands(client, neighbour)
        assert json.loads(command["payload"]) == {
            "node_id": "CAUCE-002", "not_after_ms": cert["not_after_utc_ms"]}
        # Delivered for as long as the credential it revokes could be presented.
        assert command["expires_at_utc_ms"] == pytest.approx(cert["not_after_utc_ms"], abs=1000)
    assert _revoke_commands(client, "CAUCE-009") == []  # another site
    assert _revoke_commands(client, "CAUCE-002") == []  # not to itself

    again = client.post("/v1/nodes/CAUCE-002/revoke", json={"reason": "stolen"}).json()
    assert again["peers_notified"] == 2
    assert len(_revoke_commands(client, "CAUCE-001")) == 1  # idempotent


def test_a_node_that_never_held_a_certificate_needs_no_peer_revocation(client):
    result = client.post("/v1/nodes/CAUCE-003/revoke", json={"reason": "gone"}).json()
    assert result["peers_notified"] == 0
    assert _revoke_commands(client, "CAUCE-001") == []
