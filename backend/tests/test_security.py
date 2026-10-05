"""Regressions for the CodeQL findings and the vectors found while checking them.

Five separate things are pinned here:

- `site_id` is restricted to the shape a URL, a filename and the dashboard can
  all carry safely. It previously accepted any non-empty string, so a value
  containing markup could be stored and then served back inside a JSON response.
- JSON responses declare their type, so a browser cannot sniff them as HTML.
- The firmware's bounded text builder stays inside its buffer, which is the
  defect behind the `snprintf` alert.
- `variable` and `sensor_id` are restricted at ingest, because they are stored
  verbatim and rendered by the dashboard.
- JSON embedded in a `<script>` block is escaped for that context, because
  `json.dumps` does not make a string safe to paste inside HTML.

The XSS alert on the events page was a false positive about *reflection*: that
route escapes every reflected value, and FastAPI rejects a hostile `threshold`
or `days` with a JSON 422 before the handler runs, so a number never reaches
the page as anything but a number. `test_hostile_identifiers_cannot_reach_the_html`
says so.

Checking that alert is what turned up a real one hiding behind it. The tests
above only ever checked values that came *in* through the URL. Nobody had checked
values that went *in through a node* and came back out through a page, and that
path was wide open: a `variable` of `</script><script>alert(1)</script>` was
accepted by `/v1/sync` and rendered as markup on `/nodes/{id}`, because
`json.dumps` output was placed inside a `<script>` element unescaped. The two
tests at the bottom of this file are that finding, and the lesson with it: the
suite tested the wrong boundary and called it covered.
"""

from __future__ import annotations

import os

import pytest
from fastapi.testclient import TestClient

os.environ["CAUCE_DB_PATH"] = "./data/test_security.sqlite"

from cauce_server import db  # noqa: E402
from cauce_server.main import app  # noqa: E402
from conftest import ADMIN_HEADERS
from test_api import BASE_TS, _series, sync_payload  # noqa: E402

HOSTILE = '"><script>alert(1)</script>'


@pytest.fixture()
def client():
    db.reset_for_tests()
    with TestClient(app, headers=ADMIN_HEADERS) as c:
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


# --- the stored vector, which is the one that was actually exploitable -------


def _sync_variable(client, name, sequence=1):
    return client.post("/v1/sync", json=sync_payload([
        dict(_series("CAUCE-001", BASE_TS, 1)[0], variable=name, sequence=sequence),
    ], node_id="CAUCE-001"))


def test_sync_refuses_a_variable_name_that_can_carry_markup(client):
    """The input half. `variable` was stored verbatim, so a node could put markup in
    the database that the central's own dashboard then rendered."""
    for hostile in ('</script><script>alert(1)</script>', '"><svg onload=alert(1)>',
                    "x'onerror='alert(1)", 'a b', 'a' * 65, 'var\\name', ''):
        response = _sync_variable(client, hostile)
        assert response.status_code == 422, (hostile, response.status_code)
        assert response.json()["detail"] == "invalid_variable"


def test_sync_refuses_a_hostile_sensor_id_too(client):
    """`sensor_id` is rendered on the same pages and was equally unrestricted."""
    response = client.post("/v1/sync", json=sync_payload([
        dict(_series("CAUCE-001", BASE_TS, 1)[0], sensor_id="</script><script>x</script>"),
    ], node_id="CAUCE-001"))
    assert response.status_code == 422
    assert response.json()["detail"] == "invalid_sensor_id"


def test_sync_accepts_the_variable_shapes_the_firmware_sends(client):
    """A character class, not an allowlist: the protocol is meant to grow new
    variables, and this must not refuse a legitimate one to close the hole."""
    for name in ("air_temperature", "relative_humidity", "pressure", "soil_moisture",
                 "air.temperature", "AIR-TEMP", "v2"):
        assert _sync_variable(client, name).status_code == 200, name


def test_a_stored_variable_cannot_break_out_of_a_script_block(client):
    """The output half, and the one that actually fires.

    `json.dumps` does not make a string safe inside `<script>`: the HTML parser looks for
    the literal `</script>` regardless of the JSON quoting, so a stored value containing it
    closes the block and the rest is parsed as markup.

    The node is given ONLY hostile variables, and that detail is load-bearing. The page
    charts a preferred set when the node has known variables, which keeps an unrecognised
    name out of the chart and out of the script block entirely. The original finding only
    reproduced on a node with no known variable, which is also why it survived a test suite
    that checked this route with a normally seeded node.

    Written straight to the database rather than through /v1/sync, because the sink has to
    be safe on its own and not only while every writer is careful.
    """
    _seed(client)
    with db.transaction() as conn:
        conn.execute("DELETE FROM measurements WHERE variable='air_temperature'")
        for index, hostile in enumerate((
                "</script><script>alert(1)</script>",
                "x\"onerror=\"alert(1)",
                "'><svg onload=alert(1)>",
        )):
            conn.execute(
                "INSERT OR REPLACE INTO measurements(node_id, sequence, sensor_id,"
                " timestamp_utc_ms, variable, value, unit, quality, reason_bits,"
                " time_uncertain) VALUES(?,?,?,?,?,?,?,?,?,?)",
                ("CAUCE-001", 700 + index, "BME280-1", BASE_TS + index * 60000,
                 hostile, 21.0, "C", "VALID", 0, 0),
            )

    page = client.get("/nodes/CAUCE-001")
    assert page.status_code == 200
    assert "</script><script>alert(1)" not in page.text
    assert '"><svg onload=alert(1)>' not in page.text
    # Asserted positively: "not present" would also pass if the fix were dropping the
    # data, and a chart that silently loses a series is its own bug.
    assert "\\u003c/script\\u003e\\u003cscript\\u003ealert(1)" in page.text

    for route in ("/compare?a=CAUCE-001&b=CAUCE-001", "/colocation", "/map",
                  "/nodes/CAUCE-001/events", "/nodes/CAUCE-001/report"):
        response = client.get(route)
        assert response.status_code == 200, (route, response.status_code)
        assert "</script><script>alert(1)" not in response.text, route


def test_a_hostile_number_never_reaches_the_events_page(client):
    """The premise behind CodeQL's reflected-XSS alert on this route, pinned as a test so
    the next person does not have to re-derive it from a stack trace.

    `threshold: float` means FastAPI rejects a non-numeric value with a JSON 422 before the
    handler runs. So the handler is never reached with a hostile value, and the number the
    template receives can only be a float. This is why that alert is a false positive - and
    the assertion on content-type matters as much as the status code, because a 422 rendered
    as HTML would be the same vulnerability reached by a different route.
    """
    _seed(client)
    for payload in ('<script>alert(1)</script>', '32.0" onfocus="alert(1)',
                    "32'><img src=x onerror=alert(1)>"):
        response = client.get("/nodes/CAUCE-001/events", params={"threshold": payload})
        assert response.status_code == 422, (payload, response.status_code)
        assert "application/json" in response.headers.get("content-type", ""), payload
        assert "text/html" not in response.headers.get("content-type", ""), payload


def test_form_values_are_escaped_even_when_the_type_already_guarantees_safety():
    """The belt to the validation's braces.

    `{thr}` and `{dur}` land inside HTML attributes. Escaping a float is redundant today,
    and it stays that way only for as long as the annotation holds. If someone widens
    `threshold` to a string, this test is what notices that the page is no longer escaped -
    not a browser, and not a reviewer reading the diff.
    """
    from cauce_server.dashboard import _form_value

    assert _form_value(32.0) == "32.0"
    assert _form_value(60) == "60"
    # What it is actually for: a value that could carry markup does not, even in an attribute.
    # html.escape escapes the double quote by default, which is the case that matters
    # here: the value sits inside an attribute.
    assert _form_value('x" onfocus="alert(1)') == 'x&quot; onfocus=&quot;alert(1)'
    assert "<" not in _form_value("<b>")
    assert ">" not in _form_value("<b>")


def test_the_events_page_escapes_the_values_it_reflects(client):
    """End to end, with the legitimate values a browser actually sends."""
    _seed(client)
    response = client.get("/nodes/CAUCE-001/events",
                          params={"threshold": "32.5", "min_duration_min": "90"})
    assert response.status_code == 200
    assert 'value="32.5"' in response.text
    assert 'value="90"' in response.text
    assert "&quot;" not in response.text.split("<main")[0], "an attribute was broken out of"


def _sync_quality(client, quality, sequence=1):
    return client.post("/v1/sync", json=sync_payload([
        dict(_series("CAUCE-001", BASE_TS, 1)[0], quality=quality, sequence=sequence),
    ], node_id="CAUCE-001"))


def test_sync_refuses_a_quality_the_firmware_cannot_produce(client):
    """The input half of the `quality` XSS.

    `quality` was checked for presence and nothing else, and it reaches an HTML class
    attribute. A node could sync `x" onmouseover="alert(1)` and the central returned
    `<td class="q-x" onmouseover="alert(1)">`, which fires without a click.
    """
    for hostile in ('x" onmouseover="alert(1)', '"><script>alert(1)</script>',
                    "x'><img src=x onerror=alert(1)>", "valid", "Valid", "", " "):
        response = _sync_quality(client, hostile)
        assert response.status_code == 422, (hostile, response.status_code)
        assert response.json()["detail"] == "invalid_quality"


def test_sync_accepts_every_quality_the_firmware_emits(client):
    """Pinned against the firmware enum rather than against my memory of it.

    `cauce::core::qualityName` in firmware/lib/cauce_core/src/Types.cpp returns eight
    values. An earlier version of the server-side list had four of them, which would have
    refused honest readings from a node that sends ESTIMATED or UNKNOWN. This test is what
    catches that class of mistake, and it names the firmware function it mirrors.
    """
    from cauce_server.api import FIRMWARE_QUALITIES

    assert FIRMWARE_QUALITIES == frozenset({
        "VALID", "CALIBRATED", "UNCALIBRATED", "ESTIMATED",
        "SUSPECT", "INVALID", "MISSING", "UNKNOWN",
    })
    for quality in sorted(FIRMWARE_QUALITIES):
        assert _sync_quality(client, quality).status_code == 200, quality


def test_a_stored_quality_cannot_break_out_of_the_class_attribute(client):
    """The output half, seeded past the ingest check because the sink must stand alone.

    Coverage and analytics filter on a subset of qualities, so a hostile value lands only
    in the quality-breakdown tables and the node-list line - which is where it was found.
    """
    _seed(client)
    hostile = 'x" onmouseover="alert(1)'
    with db.transaction() as conn:
        conn.execute(
            "INSERT OR REPLACE INTO measurements(node_id, sequence, sensor_id,"
            " timestamp_utc_ms, variable, value, unit, quality, reason_bits,"
            " time_uncertain) VALUES(?,?,?,?,?,?,?,?,?,?)",
            ("CAUCE-001", 800, "BME280-1", BASE_TS, "air_temperature", 21.0, "C",
             hostile, 0, 0),
        )
    for route in ("/nodes/CAUCE-001", "/nodes/CAUCE-001/report", "/"):
        page = client.get(route)
        assert page.status_code == 200, (route, page.status_code)
        assert 'class="q-x" onmouseover=' not in page.text, route
        assert "<script>alert(1)</script>" not in page.text, route


def test_the_quality_class_attribute_cannot_be_broken_out_of(client):
    """The specific shape of the bug: a quote in a class value, not a tag.

    Asserted on the quote alone. The `=` inside `onmouseover=` survives as inert text and
    that is correct - without a quote to close the attribute it cannot become an attribute,
    so asserting on it would be asserting something other than the property that matters.
    """
    from cauce_server.dashboard import _form_value

    escaped = _form_value('x" onmouseover="alert(1)')
    assert '"' not in escaped, "a raw quote would close the attribute"
    assert escaped.count("&quot;") == 2, "both quotes should be entity-encoded"
    assert not any(char in escaped for char in "<>"), "angle brackets survive too"

def test_json_for_script_escapes_what_a_json_string_does_not():
    """Unit level, so the reason the helper exists is stated where it is implemented."""
    import json

    from cauce_server.dashboard import _json_for_script

    encoded = _json_for_script({"label": "</script>", "unit": "a & b"})
    assert "</script>" not in encoded
    assert "<" not in encoded and ">" not in encoded and "&" not in encoded
    assert "\\u003c" in encoded and "\\u0026" in encoded
    # Still valid JSON that decodes back to the original value.
    assert json.loads(encoded) == {"label": "</script>", "unit": "a & b"}


def test_json_for_script_escapes_the_javascript_line_terminators():
    """U+2028 and U+2029 end a statement in JavaScript but not in JSON, so a raw one is a
    syntax error that silently kills the rest of the block.

    Written with explicit escapes rather than the literal characters: U+2028 is invisible in
    a source file, so a test containing it literally cannot be reviewed, and a reviewer who
    cannot see the character cannot check the test.
    """
    from cauce_server.dashboard import _json_for_script

    separator = "\u2028"
    paragraph = "\u2029"
    encoded = _json_for_script({"v": f"a{separator}b{paragraph}c"})
    assert separator not in encoded
    assert paragraph not in encoded
    assert "\\u2028" in encoded
    assert "\\u2029" in encoded
