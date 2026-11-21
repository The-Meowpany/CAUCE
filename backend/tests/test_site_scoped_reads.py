"""A site-scoped token reads its own site and nothing else.

Every read endpoint that names a node, a site, or returns a fleet-wide list is checked here
with a token confined to site s1, while CAUCE-002 lives in s2 and CAUCE-003 has no site.
"""

from __future__ import annotations

import pytest
from cauce_server import db
from cauce_server.config import settings
from cauce_server.main import app
from conftest import ADMIN_HEADERS
from fastapi.testclient import TestClient
from test_api import BASE_TS, _series, sync_payload

ADMIN = {"Authorization": "Bearer admin-token"}


@pytest.fixture()
def client(monkeypatch):
    db.reset_for_tests()
    monkeypatch.setattr(settings, "api_token", "admin-token")
    with TestClient(app, headers=ADMIN_HEADERS) as c:
        yield c


def _node(client, node, site):
    client.post("/v1/sync", json=sync_payload(
        _series(node, BASE_TS, 5, base=20.0), node_id=node))
    if site:
        client.post("/v1/sites", json={"site_id": site}, headers=ADMIN)
        with db.transaction() as conn:
            conn.execute("UPDATE nodes SET site_id=? WHERE node_id=?", (site, node))


@pytest.fixture()
def scoped(client):
    _node(client, "CAUCE-001", "s1")
    _node(client, "CAUCE-002", "s2")
    _node(client, "CAUCE-003", None)
    token = client.post("/v1/tokens", headers=ADMIN, json={
        "name": "s1-reader", "scopes": "read", "site_id": "s1"}).json()["token"]
    return {"Authorization": f"Bearer {token}"}


PER_NODE = [
    "/v1/nodes/{n}/coverage",
    "/v1/nodes/{n}/diagnostics",
    "/v1/nodes/{n}/revocation",
    "/v1/analytics/summary?node_id={n}&variable=air_temperature",
    "/v1/analytics/summary-fast?node_id={n}&variable=air_temperature",
    "/v1/analytics/heat-events?node_id={n}&variable=air_temperature",
    "/v1/nodes/{n}/export.csv",
    "/v1/nodes/{n}/coverage.csv",
]


@pytest.mark.parametrize("path", PER_NODE)
def test_per_node_reads_refuse_other_and_unassigned_nodes(client, scoped, path):
    for node in ("CAUCE-002", "CAUCE-003", "CAUCE-404"):
        response = client.get(path.format(n=node), headers=scoped)
        assert response.status_code == 403, (path, node, response.text)
        assert response.json()["detail"]["error"] == "site_out_of_scope"
    own = client.get(path.format(n="CAUCE-001"), headers=scoped)
    assert own.status_code != 403, (path, own.text)


def test_period_compare_is_scoped(client, scoped):
    query = "variable=air_temperature&a_start=0&a_end=10&b_start=10&b_end=20"
    assert client.get(f"/v1/analytics/period-compare?node_id=CAUCE-002&{query}",
                      headers=scoped).status_code == 403
    assert client.get(f"/v1/analytics/period-compare?node_id=CAUCE-001&{query}",
                      headers=scoped).status_code == 200


def test_lists_only_show_the_principals_site(client, scoped):
    nodes = client.get("/v1/nodes", headers=scoped).json()
    assert [n["node_id"] for n in nodes["nodes"]] == ["CAUCE-001"]
    assert nodes["total"] == 1
    assert [n["node_id"] for n in client.get("/v1/fleet", headers=scoped).json()["nodes"]] == [
        "CAUCE-001"]
    assert [s["site_id"] for s in client.get("/v1/sites", headers=scoped).json()["sites"]] == [
        "s1"]
    csv = client.get("/v1/export-all.csv", headers=scoped).text
    assert "CAUCE-001" in csv and "CAUCE-002" not in csv and "CAUCE-003" not in csv

    # A fleet-wide caller still sees everything.
    assert client.get("/v1/nodes", headers=ADMIN).json()["total"] == 3
    assert len(client.get("/v1/fleet", headers=ADMIN).json()["nodes"]) == 3


def test_site_coverage_and_interventions_are_scoped(client, scoped):
    assert client.get("/v1/sites/s2/coverage", headers=scoped).status_code == 403
    assert client.get("/v1/sites/s1/coverage", headers=scoped).status_code == 200
    assert client.get("/v1/interventions?site_id=s2", headers=scoped).status_code == 403
    for site in ("s1", "s2"):
        client.post("/v1/interventions", headers=ADMIN, json={
            "site_id": site, "kind": "shade", "start_utc_ms": BASE_TS})
    listed = client.get("/v1/interventions", headers=scoped).json()["interventions"]
    assert {i["site_id"] for i in listed} == {"s1"}


def test_alert_rules_and_log_are_scoped_and_targets_hidden(client, scoped):
    for node in ("CAUCE-001", "CAUCE-002"):
        assert client.post("/v1/alerts/rules", headers=ADMIN, json={
            "node_id": node, "kind": "stale", "stale_min": 60, "channel": "webhook",
            "target": "http://hooks.local/secret-path"}).status_code == 200
    rules = client.get("/v1/alerts/rules", headers=scoped).json()["rules"]
    assert [r["node_id"] for r in rules] == ["CAUCE-001"]
    assert rules[0]["target"] == "***"
    assert client.get("/v1/alerts/rules", headers=ADMIN).json()["rules"][0]["target"] == (
        "http://hooks.local/secret-path")
    with db.transaction() as conn:
        for node in ("CAUCE-001", "CAUCE-002"):
            conn.execute("INSERT INTO alert_log(rule_id,node_id,fired_utc_ms) VALUES(1,?,?)",
                         (node, BASE_TS))
    entries = client.get("/v1/alerts/log", headers=scoped).json()["entries"]
    assert [e["node_id"] for e in entries] == ["CAUCE-001"]


def test_alert_mutations_fail_closed(client, scoped, monkeypatch):
    rule = client.post("/v1/alerts/rules", headers=ADMIN, json={
        "node_id": "CAUCE-001", "kind": "stale", "stale_min": 60, "channel": "webhook",
        "target": "http://hooks.local/h"}).json()["rule_id"]
    # A read token is not the admin token, so it can neither edit a rule nor run a check.
    assert client.patch(f"/v1/alerts/rules/{rule}", headers=scoped,
                        json={"enabled": False}).status_code == 401
    assert client.post("/v1/alerts/check", headers=scoped).status_code == 401
    # And with no admin token configured at all, both are refused instead of left open.
    monkeypatch.setattr(settings, "api_token", "")
    assert client.patch(f"/v1/alerts/rules/{rule}", headers={},
                        json={"enabled": False}).status_code == 503
    assert client.get("/v1/alerts/check", headers={}).status_code == 503
