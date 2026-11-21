"""The HTML dashboard asks for the token once one is configured.

A browser cannot send a bearer header from the address bar, so the pages answer a Basic
challenge and take the token as the password. The same credential then reaches the /v1 API
the pages call, but only reads ride on it without `X-Requested-With`, because a browser
repeats a Basic credential on cross-site requests too.
"""

from __future__ import annotations

import base64

import pytest
from cauce_server import db
from cauce_server.config import settings
from cauce_server.main import app
from conftest import ADMIN_HEADERS, NO_AUTH
from fastapi.testclient import TestClient
from test_api import BASE_TS, _series, sync_payload

FLEET_PAGES = ("/", "/map", "/compare", "/system", "/colocation", "/heat", "/alerts")


def basic(token: str, user: str = "operator") -> dict:
    return {"Authorization": "Basic " + base64.b64encode(f"{user}:{token}".encode()).decode()}


@pytest.fixture()
def client(monkeypatch):
    db.reset_for_tests()
    monkeypatch.setattr(settings, "api_token", "admin-token")
    monkeypatch.setattr(settings, "dashboard_public", False)
    with TestClient(app, headers=ADMIN_HEADERS) as c:
        for node, site in (("CAUCE-001", "s1"), ("CAUCE-002", "s2")):
            c.post("/v1/sites", json={"site_id": site})
            c.post("/v1/sync", json=sync_payload(_series(node, BASE_TS, 5, base=20.0),
                                                 node_id=node))
            with db.transaction() as conn:
                conn.execute("UPDATE nodes SET site_id=? WHERE node_id=?", (site, node))
        yield c


def test_pages_challenge_a_browser_without_credentials(client):
    for path in FLEET_PAGES + ("/nodes/CAUCE-001", "/v1/live"):
        response = client.get(path, headers=NO_AUTH)
        assert response.status_code == 401, path
        assert response.headers["www-authenticate"].startswith("Basic realm=")


def test_the_token_as_basic_password_opens_every_page(client):
    for path in FLEET_PAGES + ("/nodes/CAUCE-001", "/nodes/CAUCE-001/report", "/v1/live"):
        assert client.get(path, headers=basic("admin-token")).status_code == 200, path
    assert client.get("/", headers=basic("wrong")).status_code == 401


def test_legal_pages_and_health_stay_open(client):
    assert client.get("/healthz", headers=NO_AUTH).status_code == 200
    assert client.get("/legal/privacy", headers=NO_AUTH).status_code == 200


def test_a_site_token_sees_its_own_site_only(client):
    token = client.post("/v1/tokens", json={"name": "s1", "scopes": "read",
                                            "site_id": "s1"}).json()["token"]
    creds = basic(token)
    # /v1/live throttles per client address and the suite shares one address.
    from cauce_server import live
    live._last_call.clear()
    index = client.get("/", headers=creds)
    assert index.status_code == 200
    assert "CAUCE-001" in index.text and "CAUCE-002" not in index.text
    assert client.get("/v1/live", headers=creds).json()["nodes"] == 1
    assert client.get("/nodes/CAUCE-001", headers=creds).status_code == 200
    assert client.get("/nodes/CAUCE-002", headers=creds).status_code == 403
    for path in FLEET_PAGES[1:]:
        assert client.get(path, headers=creds).status_code == 403, path


def test_basic_writes_need_the_requested_with_header(client):
    body = {"node_id": "CAUCE-001", "kind": "stale", "stale_min": 60, "channel": "webhook",
            "target": "http://hooks.local/h"}
    # What a cross-site form would send: the browser attaches the cached credential, but it
    # cannot add a custom header.
    assert client.post("/v1/alerts/rules", json=body,
                       headers=basic("admin-token")).status_code == 401
    ok = client.post("/v1/alerts/rules", json=body,
                     headers={**basic("admin-token"), "X-Requested-With": "cauce"})
    assert ok.status_code == 200
    assert client.get("/v1/alerts/rules", headers=basic("admin-token")).status_code == 200


def test_a_public_dashboard_stays_open_but_the_api_does_not(client, monkeypatch):
    monkeypatch.setattr(settings, "dashboard_public", True)
    assert client.get("/map", headers=NO_AUTH).status_code == 200
    assert client.get("/v1/nodes", headers=NO_AUTH).status_code == 401


def test_without_a_token_the_dashboard_is_open_as_before(client, monkeypatch):
    monkeypatch.setattr(settings, "api_token", "")
    assert client.get("/", headers=NO_AUTH).status_code == 200
