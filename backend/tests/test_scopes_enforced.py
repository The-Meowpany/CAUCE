"""Capability scopes and site scoping are enforced, not merely declared.

`security.require_scope` existed and was correct, but no endpoint in `api.py`
called it: every one compared against the shared admin token, which cannot tell
a read-only credential from an admin one. These tests fail against that state, so
they are the evidence rather than a restatement of the intent.

The shared admin token still works everywhere, which is what keeps a
single-operator deployment unchanged.
"""

import os

import pytest
from fastapi.testclient import TestClient

os.environ["CAUCE_DB_PATH"] = "./data/test_scopes.sqlite"

from cauce_server import db  # noqa: E402
from cauce_server.config import settings  # noqa: E402
from cauce_server.main import app  # noqa: E402
from conftest import ADMIN_HEADERS

ADMIN = {"Authorization": "Bearer admin-token"}


@pytest.fixture()
def client(monkeypatch):
    # Auth is off unless a shared token is configured, and with it off every
    # scope check trivially passes. Turning it on is what makes these tests mean
    # anything.
    monkeypatch.setattr(settings, "api_token", "admin-token")
    db.reset_for_tests()
    with TestClient(app, headers=ADMIN_HEADERS) as c:
        yield c


def issue(client, name, token, scopes, site_id=None):
    body = {"name": name, "token": token, "scopes": scopes}
    if site_id is not None:
        body["site_id"] = site_id
    response = client.post("/v1/tokens", json=body, headers=ADMIN)
    assert response.status_code == 200, response.text
    return {"Authorization": f"Bearer {token}"}


def seed(client):
    """A node in site s1 and another in s2.

    Node-to-site assignment is done in SQL because there is no endpoint for it,
    which is itself worth knowing: today a site is attached by the operator, not
    by a node claiming it.
    """
    client.post("/v1/sites", json={"site_id": "s1"}, headers=ADMIN)
    client.post("/v1/sites", json={"site_id": "s2"}, headers=ADMIN)
    payload = {
        "protocol_version": 1,
        "measurements": [
            {"sequence": 1, "timestamp_utc_ms": 1787356800000,
             "variable": "air_temperature", "value": 20.0, "unit": "degC",
             "quality": "VALID"},
        ],
    }
    # The site is attached by the SQL below, not by the sync payload, so the loop
    # only needs the node ids.
    for node_id in ("CAUCE-001", "CAUCE-002"):
        body = dict(payload, node_id=node_id)
        client.post("/v1/sync", json=body, headers=ADMIN)
    with db.transaction() as conn:
        conn.execute("UPDATE nodes SET site_id='s1' WHERE node_id='CAUCE-001'")
        conn.execute("UPDATE nodes SET site_id='s2' WHERE node_id='CAUCE-002'")
    return client


class TestScopesAreEnforced:
    def test_a_read_token_can_read(self, client):
        seed(client)
        headers = issue(client, "reader", "tok-reader-0123456789ab", "read")
        assert client.get("/v1/nodes", headers=headers).status_code == 200
        assert client.get("/v1/nodes/CAUCE-001", headers=headers).status_code \
            == 200

    def test_a_read_token_cannot_write(self, client):
        """The whole point: previously the shared-token check let this through."""
        seed(client)
        headers = issue(client, "reader", "tok-reader-0123456789ab", "read")
        response = client.post(
            "/v1/nodes/CAUCE-001/time-reconstruct", json={}, headers=headers)
        assert response.status_code == 403
        assert response.json()["detail"]["error"] == "insufficient_scope"
        assert response.json()["detail"]["required"] == "write"

    def test_a_read_token_cannot_run_admin_work(self, client):
        seed(client)
        headers = issue(client, "reader", "tok-reader-0123456789ab", "read")
        assert client.post("/v1/maintenance/retention", json={},
                           headers=headers).status_code == 403
        assert client.get("/v1/maintenance/backup",
                          headers=headers).status_code == 403

    def test_a_write_token_cannot_do_admin_work(self, client):
        seed(client)
        headers = issue(client, "writer", "tok-writer-0123456789abc", "write")
        assert client.post("/v1/maintenance/retention", json={},
                           headers=headers).status_code == 403
        assert client.get("/v1/nodes", headers=headers).status_code == 403

    def test_a_write_token_can_write(self, client):
        seed(client)
        headers = issue(client, "writer", "tok-writer-0123456789abc", "write")
        issued = client.post("/v1/nodes/CAUCE-001/commands", json={
            "kind": "request_resync", "idempotency_key": "k2",
            "arguments": {}}, headers=headers)
        assert issued.status_code == 200, issued.text

    def test_admin_can_do_everything(self, client):
        seed(client)
        headers = issue(client, "boss", "tok-admin-0123456789abcd", "admin")
        assert client.get("/v1/nodes", headers=headers).status_code == 200
        assert client.get("/v1/maintenance/backup",
                          headers=headers).status_code == 200
        issued = client.post("/v1/nodes/CAUCE-001/commands", json={
            "kind": "request_resync", "idempotency_key": "k2",
            "arguments": {}}, headers=headers)
        assert issued.status_code == 200, issued.text

    def test_the_shared_admin_token_is_unchanged(self, client):
        """A single-operator deployment must not have to change anything."""
        seed(client)
        assert client.get("/v1/nodes", headers=ADMIN).status_code == 200
        assert client.get("/v1/maintenance/backup",
                          headers=ADMIN).status_code == 200

    def test_an_unknown_token_is_refused(self, client):
        seed(client)
        headers = {"Authorization": "Bearer not-a-real-token"}
        assert client.get("/v1/nodes", headers=headers).status_code == 401


class TestSiteScoping:
    def test_a_site_scoped_principal_cannot_read_another_site(self, client):
        seed(client)
        headers = issue(client, "op1", "tok-op1-0123456789abcdef", "read",
                        site_id="s1")
        assert client.get("/v1/nodes/CAUCE-001", headers=headers).status_code \
            == 200
        response = client.get("/v1/nodes/CAUCE-002", headers=headers)
        assert response.status_code == 403
        assert response.json()["detail"]["error"] == "site_out_of_scope"

    def test_a_site_scoped_principal_cannot_read_another_sites_measurements(
            self, client):
        seed(client)
        headers = issue(client, "op1", "tok-op1-0123456789abcdef", "read",
                        site_id="s1")
        assert client.get("/v1/nodes/CAUCE-001/measurements",
                          headers=headers).status_code == 200
        assert client.get("/v1/nodes/CAUCE-002/measurements",
                          headers=headers).status_code == 403

    def test_a_site_scoped_principal_cannot_write_to_another_site(self, client):
        seed(client)
        headers = issue(client, "op1", "tok-op1-0123456789abcdef", "write",
                        site_id="s1")
        issued = client.post("/v1/nodes/CAUCE-001/commands", json={
            "kind": "request_resync", "idempotency_key": "k2",
            "arguments": {}}, headers=headers)
        assert issued.status_code == 200, issued.text
        assert client.post("/v1/nodes/CAUCE-002/time-reconstruct",
                           headers=headers).status_code == 403

    def test_compare_cannot_mix_another_sites_node(self, client):
        """A per-site operator must not infer s2's data by pairing it with s1."""
        seed(client)
        headers = issue(client, "op1", "tok-op1-0123456789abcdef", "read",
                        site_id="s1")
        ok = client.get("/v1/analytics/compare?node_a=CAUCE-001"
                        "&node_b=CAUCE-001&variable=air_temperature",
                        headers=headers)
        assert ok.status_code == 200
        blocked = client.get("/v1/analytics/compare?node_a=CAUCE-001"
                             "&node_b=CAUCE-002&variable=air_temperature",
                             headers=headers)
        assert blocked.status_code == 403

    def test_a_fleet_wide_principal_is_unaffected(self, client):
        seed(client)
        headers = issue(client, "fleet", "tok-fleet-0123456789abcde", "read")
        assert client.get("/v1/nodes/CAUCE-002", headers=headers).status_code \
            == 200

    def test_the_admin_token_is_unaffected_by_site_scoping(self, client):
        seed(client)
        assert client.get("/v1/nodes/CAUCE-002", headers=ADMIN).status_code \
            == 200
