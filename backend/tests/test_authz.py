"""Per-principal authorization.

The project used to have a single global admin token, so anyone who could read
the environment could read and write every site. `api_tokens` adds real
principals: a token, its scopes, and optionally the one site it belongs to.

Two rules shape the design:

- **A read-only operator cannot queue a command.** Scopes are checked before
  the payload is even validated, so an insufficient-scope caller learns nothing
  about the endpoint beyond the fact that it exists.
- **A site-scoped operator cannot touch another site**, even through a URL it
  guessed. Without this, scopes would only limit *what* an operator could do,
  never *where*, which is the half that leaks data.
"""

from __future__ import annotations

import os
import time

import pytest
from fastapi.testclient import TestClient

os.environ["CAUCE_DB_PATH"] = "./data/test_authz.sqlite"

from cauce_server import db  # noqa: E402
from cauce_server.config import settings  # noqa: E402
from cauce_server.main import app  # noqa: E402
from cauce_server.security import create_token, hash_token  # noqa: E402
from conftest import ADMIN_HEADERS
from test_api import BASE_TS, _series, sync_payload  # noqa: E402

SCOPES = ("read", "write", "admin")


@pytest.fixture()
def client(monkeypatch):
    db.reset_for_tests()
    monkeypatch.setattr(settings, "api_token", "shared-admin-token")
    with TestClient(app, headers=ADMIN_HEADERS) as c:
        yield c


def _issue(name, token, scopes, site_id=None):
    assert create_token(name, token, scopes, site_id, int(time.time() * 1000))


def _seed(client, node="CAUCE-001", site="s1"):
    client.post("/v1/sites", json={"site_id": site}, headers={
        "Authorization": "Bearer shared-admin-token"})
    client.post("/v1/sync", json=sync_payload(_series(node, BASE_TS, 5, base=20.0),
                                              node_id=node))
    with db.transaction() as conn:
        conn.execute("UPDATE nodes SET site_id=? WHERE node_id=?", (site, node))


def test_no_token_means_the_shared_admin_gate_still_applies(client):
    _seed(client)
    assert client.put("/v1/sites/s1/calibration", json={
        "variable": "air_temperature", "offset": -1.0}
    ).status_code == 401
    ok = client.put("/v1/sites/s1/calibration",
                     headers={"Authorization": "Bearer shared-admin-token"},
                     json={"variable": "air_temperature", "offset": -1.0})
    assert ok.status_code == 200


def test_scopes_separate_read_from_write(client):
    _seed(client)
    _issue("reader", "reader-secret", "read")
    reader = {"Authorization": "Bearer reader-secret"}

    assert client.get("/v1/sites/s1/calibration",
                      headers=reader).status_code == 200
    forbidden = client.put("/v1/sites/s1/calibration", headers=reader,
                            json={"variable": "air_temperature", "offset": -1.0})
    assert forbidden.status_code == 403
    assert forbidden.json()["detail"]["error"] == "insufficient_scope"


def test_admin_scope_satisfies_everything(client):
    _seed(client)
    _issue("boss", "boss-secret", "read,write,admin")
    boss = {"Authorization": "Bearer boss-secret"}
    assert client.put("/v1/sites/s1/calibration", headers=boss,
                       json={"variable": "air_temperature",
                             "offset": -1.0}).status_code == 200
    assert client.post("/v1/nodes/CAUCE-001/commands", headers=boss,
                       json={"kind": "request_resync",
                             "idempotency_key": "k"}).status_code == 200


def test_a_site_scoped_principal_cannot_reach_another_site(client):
    _seed(client, node="CAUCE-001", site="s1")
    _seed(client, node="CAUCE-002", site="s2")
    _issue("s1-op", "s1-secret", "read,write", site_id="s1")
    scoped = {"Authorization": "Bearer s1-secret"}

    assert client.get("/v1/sites/s1/calibration", headers=scoped).status_code == 200
    denied = client.get("/v1/sites/s2/calibration", headers=scoped)
    assert denied.status_code == 403
    assert denied.json()["detail"]["error"] == "site_out_of_scope"

    # And it holds for writes, not just reads.
    assert client.put("/v1/sites/s2/calibration", headers=scoped,
                       json={"variable": "air_temperature",
                             "offset": -1.0}).status_code == 403


def test_a_site_scoped_principal_cannot_queue_commands_for_another_site(client):
    _seed(client, node="CAUCE-001", site="s1")
    _seed(client, node="CAUCE-002", site="s2")
    _issue("s1-op", "s1-secret", "read,write", site_id="s1")
    scoped = {"Authorization": "Bearer s1-secret"}

    assert client.post("/v1/nodes/CAUCE-001/commands", headers=scoped,
                       json={"kind": "request_resync",
                             "idempotency_key": "ok"}).status_code == 200
    denied = client.post("/v1/nodes/CAUCE-002/commands", headers=scoped,
                         json={"kind": "request_resync",
                               "idempotency_key": "no"})
    assert denied.status_code == 403


def test_a_fleet_wide_principal_is_not_limited_by_site(client):
    _seed(client, node="CAUCE-001", site="s1")
    _seed(client, node="CAUCE-002", site="s2")
    _issue("network", "network-secret", "read,write")
    everywhere = {"Authorization": "Bearer network-secret"}
    assert client.get("/v1/sites/s1/calibration",
                      headers=everywhere).status_code == 200
    assert client.get("/v1/sites/s2/calibration",
                      headers=everywhere).status_code == 200


def test_unknown_tokens_are_rejected(client):
    _seed(client)
    assert client.get("/v1/sites/s1/calibration",
                      headers={"Authorization": "Bearer nope"}).status_code == 401
    assert client.get("/v1/sites/s1/calibration",
                      headers={"Authorization": "Basic nope"}).status_code == 401
    assert client.get("/v1/sites/s1/calibration",
                      headers={"Authorization": "Bearer "}).status_code == 401


def test_tokens_are_stored_hashed_never_in_the_clear(client):
    _issue("reader", "super-secret-value", "read")
    with db.transaction() as conn:
        rows = conn.execute("SELECT * FROM api_tokens").fetchall()
    assert len(rows) == 1
    stored = rows[0]["token_sha256"]
    assert "super-secret-value" not in stored
    assert stored == hash_token("super-secret-value")


def test_a_duplicate_name_is_refused(client):
    _issue("reader", "secret-one", "read")
    assert create_token("reader", "secret-two", "admin", None, 0) is False


def test_an_issued_token_cannot_act_before_it_is_revoked(client):
    _seed(client)
    _issue("temp", "temp-secret", "write")
    headers = {"Authorization": "Bearer temp-secret"}
    assert client.put("/v1/sites/s1/calibration", headers=headers,
                       json={"variable": "air_temperature",
                             "offset": -1.0}).status_code == 200

    from cauce_server.security import delete_token
    assert delete_token("temp") is True
    assert client.put("/v1/sites/s1/calibration", headers=headers,
                       json={"variable": "air_temperature",
                             "offset": -2.0}).status_code == 401
