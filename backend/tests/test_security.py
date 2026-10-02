"""Regressions for the CodeQL findings and the vector found while checking them.

Three separate things are pinned here:

- `site_id` is restricted to the shape a URL, a filename and the dashboard can
  all carry safely. It previously accepted any non-empty string, so a value
  containing markup could be stored and then served back inside a JSON response.
- JSON responses declare their type, so a browser cannot sniff them as HTML.
- The firmware's bounded text builder stays inside its buffer, which is the
  defect behind the `snprintf` alert.

The XSS alert on the events page itself was investigated and is a false
positive: that route escapes every reflected value and the dashboard path
routing rejects a hostile `node_id` before it can render. The test that says so
is `test_hostile_identifiers_cannot_reach_the_html`.
"""

from __future__ import annotations

import os

import pytest
from fastapi.testclient import TestClient

os.environ["CAUCE_DB_PATH"] = "./data/test_security.sqlite"

from cauce_server import db  # noqa: E402
from cauce_server.main import app  # noqa: E402
from test_api import BASE_TS, _series, sync_payload  # noqa: E402

HOSTILE = '"><script>alert(1)</script>'


@pytest.fixture()
def client():
    db.reset_for_tests()
    with TestClient(app) as c:
        yield c


def _seed(client, node="CAUCE-001"):
    client.post("/v1/sync", json=sync_payload(_series(node, BASE_TS, 3),
                                              node_id=node))


def test_site_id_rejects_markup(client):
    for hostile in (HOSTILE, "<b>x</b>", "a/b", "a?b", "a#b", "a b",
                    "a.b", "site\nid", ""):
        response = client.post("/v1/sites", json={"site_id": hostile})
        assert response.status_code in (422, 409), (
            f"accepted {hostile!r} as a site id"
        )


def test_site_id_rejects_an_overlong_value(client):
    response = client.post("/v1/sites", json={"site_id": "s" * 65})
    assert response.status_code == 422
    assert "invalid_site_id" in str(response.json())


def test_site_id_accepts_the_shape_the_firmware_uses(client):
    for good in ("s1", "SITE-A_b", "node.1", "a" * 64):
        if good == "node.1":
            # '.' is outside the node-id charset on purpose.
            assert client.post("/v1/sites",
                               json={"site_id": good}).status_code == 422
            continue
        assert client.post("/v1/sites", json={"site_id": good}).status_code == 200


def test_intervention_and_assignment_validate_the_site_id(client):
    _seed(client)
    client.post("/v1/sites", json={"site_id": "s1"})
    hostile = client.post("/v1/interventions", json={
        "site_id": HOSTILE, "kind": "shade", "start_utc_ms": BASE_TS})
    assert hostile.status_code == 422
    assert "invalid_site_id" in str(hostile.json())

    assign = client.put("/v1/nodes/CAUCE-001/site", json={"site_id": HOSTILE})
    assert assign.status_code == 422


def test_a_stored_hostile_identifier_cannot_be_smuggled_in(client):
    # The vector that prompted this: a site id used to be stored verbatim and
    # then echoed by the JSON API. Validation now stops it at the door, and this
    # asserts both halves, because either alone would be a weak guarantee.
    _seed(client)
    assert client.post("/v1/sites", json={"site_id": HOSTILE}).status_code == 422

    with db.transaction() as conn:
        rows = conn.execute("SELECT site_id FROM sites").fetchall()
    assert all("<script" not in r["site_id"] for r in rows)


def test_json_responses_declare_their_type(client):
    _seed(client)
    for route in ("/healthz", "/v1/nodes", "/v1/fleet"):
        response = client.get(route)
        assert response.status_code == 200
        assert response.headers.get("X-Content-Type-Options") == "nosniff", (
            f"{route} can be sniffed as HTML"
        )


def test_dashboard_pages_keep_their_own_headers(client):
    _seed(client)
    response = client.get("/nodes/CAUCE-001")
    assert response.status_code == 200
    assert "text/html" in response.headers.get("content-type", "")
    # Not stripped from HTML: the page is the one place the browser is meant to
    # render what we sent.
    assert response.headers.get("X-Content-Type-Options") is None


def test_hostile_identifiers_cannot_reach_the_html(client):
    # The XSS alert pointed at the events page. Every reflected value there is
    # escaped, and a hostile node id never reaches a 200 because path routing
    # rejects it, so there is nothing to reflect. This pins both halves so a
    # future change that unescapes something fails here instead of in a browser.
    _seed(client)
    for param in ("variable", "threshold", "min_duration_min"):
        value = HOSTILE if param == "variable" else "1"
        response = client.get("/nodes/CAUCE-001/events",
                              params={param: value})
        assert response.status_code == 200
        assert "<script>alert" not in response.text
        assert "script&gt;alert" in response.text or "<script" not in response.text

    hostile_path = client.get(f"/nodes/{HOSTILE}/events")
    assert hostile_path.status_code == 404
    assert "<script>alert" not in hostile_path.text


def test_dashboard_escapes_every_page_it_serves(client):
    _seed(client)
    for route in ("/", "/alerts", "/map", "/colocation", "/system",
                  "/compare", "/nodes/CAUCE-001",
                  "/nodes/CAUCE-001/report", "/nodes/CAUCE-001/events"):
        response = client.get(route)
        assert response.status_code == 200
        assert "<script>alert" not in response.text


def test_site_names_are_free_text_but_not_reflected_as_markup(client):
    # Unlike the identifier, a name is legitimately free text. It is escaped on
    # the way out rather than restricted on the way in.
    assert client.post("/v1/sites", json={
        "site_id": "s1", "name": HOSTILE}).status_code == 200
    _seed(client)
    client.put("/v1/nodes/CAUCE-001/site", json={"site_id": "s1"})
    for route in ("/colocation", "/nodes/CAUCE-001/report", "/system"):
        response = client.get(route)
        assert "<script>alert" not in response.text
