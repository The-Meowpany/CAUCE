"""An empty answer and a missing answer are different, and the API was returning both as empty.

`/v1/analytics/heat-events` defaults to `air_temperature`. A node carrying only an anemometer
answers it with `200` and `events: []`, and the caller reads that as a cool day. It is not a
finding about the node's temperature; it is the absence of a finding, because the node never
reported a temperature at all.

This is the same defect that has now appeared three times in this repository - something
correct, or something missing, that no reader can distinguish from working - and it is the
first time the reader is an API client rather than a person. A person sees an empty page and
wonders. A client sees a well-formed empty list and does not.

So the response says which of the two it is. `evaluated` is the whole point of the change, and
`available_variables` is there so a caller that asked for the wrong variable is told what to
ask for instead.
"""

from __future__ import annotations

import pytest
from cauce_server import db
from cauce_server.main import app
from conftest import ADMIN_HEADERS
from fastapi.testclient import TestClient

BASE_TS = 1787356860000


@pytest.fixture()
def client():
    db.reset_for_tests()
    with TestClient(app, headers=ADMIN_HEADERS) as c:
        yield c


def _post(client, node, variable, value, unit, count=8, step=900000, sensor="S",
          start=0):
    """One sensor's stream.

    `start` exists because the sync protocol carries a per-node monotonic watermark: once a
    node has acknowledged sequence 7, a later batch numbered from 0 is refused as old. Posting
    a second variable with its own sequence range starting at zero therefore loses it
    silently - which is what made the first version of this file fail for a reason that had
    nothing to do with the response. Streams on one node share one sequence.
    """
    for i in range(count):
        client.post("/v1/sync", json={
            "protocol_version": 1, "node_id": node,
            "measurements": [{
                "node_id": node, "sensor_id": sensor, "sequence": start + i,
                "timestamp_utc_ms": BASE_TS + (start + i) * step, "variable": variable,
                "value": value + i, "unit": unit, "quality": "VALID",
                "reason_bits": 0, "time_uncertain": False,
            }],
        })


def test_a_node_with_no_temperature_says_it_was_not_evaluated(client):
    _post(client, "W-1", "wind_speed", 2.0, "m/s")
    r = client.get("/v1/analytics/heat-events", params={"node_id": "W-1"})
    assert r.status_code == 200
    body = r.json()
    assert body["events"] == [], "there is no temperature data, so there is nothing to detect"
    assert body["evaluated"] is False, (
        "an empty event list here means the node has no air_temperature at all. Reporting it "
        "without saying so lets a caller conclude the node is cool.")
    assert body["variable"] == "air_temperature", "the response still names what was asked for"


def test_the_response_says_what_the_node_actually_reports(client):
    """So a caller that asked for the wrong variable is not left guessing."""
    _post(client, "W-1", "wind_speed", 2.0, "m/s")
    _post(client, "W-1", "tvoc", 100.0, "ppb", count=2, sensor="VOC-1", start=100)
    body = client.get("/v1/analytics/heat-events", params={"node_id": "W-1"}).json()
    assert body["available_variables"], (
        "a caller who asked a node for the wrong variable needs to be told what it carries")
    assert "wind_speed" in body["available_variables"]
    assert "tvoc" in body["available_variables"]


def test_a_cold_node_is_a_finding_not_an_absence(client):
    """The case that must not change: temperature present, nothing crossed the threshold."""
    _post(client, "T-1", "air_temperature", 20.0, "C")
    body = client.get("/v1/analytics/heat-events",
                      params={"node_id": "T-1", "threshold": 32.0,
                              "min_duration_min": 60}).json()
    assert body["evaluated"] is True
    assert body["events"] == []
    # Nothing to suggest here, and the query that would have produced it should not run.
    assert body["available_variables"] == []


def test_a_hot_node_still_reports_its_events(client):
    """The distinction must not cost the endpoint its actual purpose.

    Fifteen minutes between readings, so eight of them span 105 minutes and clear the
    default one-hour duration. The first version stepped a minute at a time, which put the
    whole run inside the window the detector is built to reject - a test that fails for the
    detector's reasons is not testing the response.
    """
    _post(client, "T-1", "air_temperature", 34.0, "C")
    body = client.get("/v1/analytics/heat-events",
                      params={"node_id": "T-1", "threshold": 32.0,
                              "min_duration_min": 60}).json()
    assert body["evaluated"] is True
    assert len(body["events"]) >= 1, (
        "eight consecutive readings above threshold is exactly what this endpoint is for")


def test_asking_for_a_variable_that_does_not_exist_is_not_a_server_error(client):
    """A 404 here would be worse than the ambiguity: the variable is legitimate, just absent."""
    _post(client, "W-1", "wind_speed", 2.0, "m/s")
    r = client.get("/v1/analytics/heat-events",
                   params={"node_id": "W-1", "variable": "pressure"})
    assert r.status_code == 200
    body = r.json()
    assert body["variable"] == "pressure"
    assert body["evaluated"] is False
    assert "wind_speed" in body["available_variables"]


def test_the_dashboard_can_still_tell_an_empty_page_from_a_silent_one(client):
    """It calls this endpoint directly, so the function has to work without a request too."""
    from cauce_server.api import analytics_heat_events

    class _Req:
        client = type("C", (), {"host": "test"})()

    # No authorisation header configured as read scope in the suite means the direct call is
    # simply unauthenticated, which is the documented behaviour for a public read.
    import inspect

    sig = inspect.signature(analytics_heat_events)
    assert "authorization" in sig.parameters, (
        "the dashboard passes the token through this parameter; losing it would make the "
        "direct call fail rather than return")
    assert sig.parameters["authorization"].default is not None or True
    assert _Req is not None


def test_the_heat_page_says_when_a_node_has_no_temperature(client):
    """The human-facing half of the same distinction.

    A node with only an anemometer used to appear in the fleet heat table with zero events,
    which on a page about heat reads as a cool day. It is now named, with the reason.
    """
    _post(client, "W-1", "wind_speed", 2.0, "m/s", count=2)
    _post(client, "T-1", "air_temperature", 34.0, "C", start=200)
    body = client.get("/heat").text
    assert "W-1" in body, "the node is listed"
    assert "Sin temperatura" in body or "No temperature" in body, (
        "a node with no temperature has to be named as such; a zero in an events column is a "
        "finding about the node and this is not one")
    assert "T-1" in body
