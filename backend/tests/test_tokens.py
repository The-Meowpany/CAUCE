"""Token administration endpoints.

The regression that matters here is reachability: `create_token` existed as a
library function called only from tests, so the per-principal authorization
model could not be used in a deployment at all. These tests drive the HTTP
surface the way an operator would.
"""

from __future__ import annotations

import os

import pytest
from fastapi.testclient import TestClient

os.environ["CAUCE_DB_PATH"] = "./data/test_tokens.sqlite"

from cauce_server import db  # noqa: E402
from cauce_server.config import settings  # noqa: E402
from cauce_server.main import app  # noqa: E402
from test_api import BASE_TS, _series, sync_payload  # noqa: E402

ADMIN = {"Authorization": "Bearer admin-token"}


@pytest.fixture()
def client(monkeypatch):
    db.reset_for_tests()
    monkeypatch.setattr(settings, "api_token", "admin-token")
    with TestClient(app) as c:
        yield c


def _seed(client, node="CAUCE-001", site="s1"):
    client.post("/v1/sites", json={"site_id": site}, headers=ADMIN)
    client.post("/v1/sync", json=sync_payload(
        _series(node, BASE_TS, 5, base=20.0), node_id=node))
    with db.transaction() as conn:
        conn.execute("UPDATE nodes SET site_id=? WHERE node_id=?", (site, node))


# --- issuing -------------------------------------------------------------

def test_a_token_can_be_issued_over_http(client):
    response = client.post("/v1/tokens", headers=ADMIN, json={
        "name": "field-reader", "scopes": "read"})
    assert response.status_code == 200, response.text
    body = response.json()
    assert body["status"] == "issued"
    assert body["scopes"] == ["read"]
    assert body["site_id"] is None
    # The plaintext is shown exactly once.
    assert len(body["token"]) >= 32
    assert "store this now" in body["note"]


def test_an_issued_token_actually_works(client):
    _seed(client)
    issued = client.post("/v1/tokens", headers=ADMIN, json={
        "name": "reader", "scopes": "read"}).json()

    reader = {"Authorization": f"Bearer {issued['token']}"}
    assert client.get("/v1/sites/s1/calibration", headers=reader).status_code == 200
    # Its scope is real, not advisory.
    assert client.put("/v1/sites/s1/calibration", headers=reader,
                       json={"variable": "air_temperature",
                             "offset": -1.0}).status_code == 403


def test_a_supplied_token_is_accepted_for_external_generation(client):
    issued = client.post("/v1/tokens", headers=ADMIN, json={
        "name": "external", "scopes": "read",
        "token": "a-secret-generated-elsewhere"}).json()
    assert issued["token"] == "a-secret-generated-elsewhere"
    assert client.get("/v1/sites/s1/calibration", headers={
        "Authorization": "Bearer a-secret-generated-elsewhere"}
    ).status_code in (200, 404)


def test_scopes_are_validated(client):
    for bad in ("", "   ", "delete,read", "superuser"):
        response = client.post("/v1/tokens", headers=ADMIN,
                               json={"name": "x", "scopes": bad})
        assert response.status_code == 422, bad


def test_admin_scope_subsumes_the_others_in_storage(client):
    issued = client.post("/v1/tokens", headers=ADMIN, json={
        "name": "boss", "scopes": "read,write,admin"}).json()
    assert issued["scopes"] == ["admin"]
    with db.transaction() as conn:
        row = conn.execute("SELECT scopes FROM api_tokens WHERE name='boss'"
                           ).fetchone()
    assert row["scopes"] == "admin"


def test_a_site_scoped_token_carries_its_site(client):
    _seed(client)
    issued = client.post("/v1/tokens", headers=ADMIN, json={
        "name": "s1-op", "scopes": "read,write", "site_id": "s1"}).json()
    assert issued["site_id"] == "s1"

    scoped = {"Authorization": f"Bearer {issued['token']}"}
    assert client.get("/v1/sites/s1/calibration", headers=scoped).status_code == 200
    assert client.get("/v1/sites/other/calibration",
                      headers=scoped).status_code == 403


def test_token_validation(client):
    assert client.post("/v1/tokens", headers=ADMIN,
                       json={"scopes": "read"}).status_code == 422
    assert client.post("/v1/tokens", headers=ADMIN,
                       json={"name": "", "scopes": "read"}).status_code == 422
    assert client.post("/v1/tokens", headers=ADMIN,
                       json={"name": "n" * 65, "scopes": "read"}
                       ).status_code == 422
    assert client.post("/v1/tokens", headers=ADMIN,
                       json={"name": "n", "scopes": "read",
                             "site_id": "not a site"}).status_code == 422
    assert client.post("/v1/tokens", headers=ADMIN,
                       json={"name": "n", "scopes": "read",
                             "token": "t" * 200}).status_code == 422


def test_a_duplicate_name_is_refused(client):
    client.post("/v1/tokens", headers=ADMIN,
                json={"name": "dup", "scopes": "read"})
    again = client.post("/v1/tokens", headers=ADMIN,
                        json={"name": "dup", "scopes": "admin"})
    assert again.status_code == 409
    assert again.json()["detail"] == "token_name_exists"


# --- listing and revocation ---------------------------------------------

def test_tokens_are_listed_without_their_digests(client):
    client.post("/v1/tokens", headers=ADMIN,
                json={"name": "a", "scopes": "read"})
    client.post("/v1/tokens", headers=ADMIN,
                json={"name": "b", "scopes": "write", "site_id": "s1"})
    body = client.get("/v1/tokens", headers=ADMIN).json()
    names = sorted(t["name"] for t in body["tokens"])
    assert names == ["a", "b"]
    assert "token" not in body["tokens"][0]
    assert "token_sha256" not in body["tokens"][0]
    assert "digest" in body["note"]


def test_a_token_can_be_revoked_and_stops_working(client):
    _seed(client)
    issued = client.post("/v1/tokens", headers=ADMIN, json={
        "name": "temp", "scopes": "read"}).json()
    headers = {"Authorization": f"Bearer {issued['token']}"}
    assert client.get("/v1/sites/s1/calibration", headers=headers).status_code == 200

    revoked = client.delete("/v1/tokens/temp", headers=ADMIN)
    assert revoked.status_code == 200
    assert revoked.json()["status"] == "revoked"
    assert client.get("/v1/sites/s1/calibration", headers=headers).status_code == 401
    assert client.delete("/v1/tokens/temp", headers=ADMIN).status_code == 404


def test_a_single_token_can_be_described(client):
    client.post("/v1/tokens", headers=ADMIN,
                json={"name": "solo", "scopes": "read,write"})
    body = client.get("/v1/tokens/solo", headers=ADMIN).json()
    assert body["name"] == "solo"
    assert body["scopes"] == "read,write"
    assert client.get("/v1/tokens/nope", headers=ADMIN).status_code == 404


# --- the guard that makes the scope system real --------------------------

def test_only_the_shared_admin_may_mint_credentials(client):
    _seed(client)
    issued = client.post("/v1/tokens", headers=ADMIN, json={
        "name": "minter", "scopes": "read"}).json()
    scoped = {"Authorization": f"Bearer {issued['token']}"}

    # A scoped principal must not be able to widen its own reach, so this is
    # refused at the admin gate rather than by a missing scope.
    assert client.post("/v1/tokens", headers=scoped,
                       json={"name": "escalated", "scopes": "admin"}
                       ).status_code == 401
    assert client.get("/v1/tokens", headers=scoped).status_code == 401
    assert client.delete("/v1/tokens/minter", headers=scoped).status_code == 401


def test_token_endpoints_require_authentication(client):
    assert client.post("/v1/tokens",
                       json={"name": "x", "scopes": "read"}).status_code == 401
    assert client.get("/v1/tokens").status_code == 401
    assert client.delete("/v1/tokens/x").status_code == 401


def test_healthz_reports_tokens(client):
    body = client.get("/healthz").json()
    assert body["tables"]["api_tokens"] == 0
