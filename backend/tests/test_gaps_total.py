"""The gap cap is bounded, and now the bound is measurable.

`gaps_truncated` used to say "there were more" without saying how much more, so a
caller could not tell a site with 21 gaps from one with 21,000 - and the field was
derived from `len(gaps) >= MAX_GAPS_REPORTED`, which also reported truncation when
exactly twenty gaps existed and nothing was dropped.
"""

from __future__ import annotations

import os

import pytest
from fastapi.testclient import TestClient

os.environ["CAUCE_DB_PATH"] = "./data/test_gaps_total.sqlite"

from cauce_server import db  # noqa: E402
from cauce_server.coverage import MAX_GAPS_REPORTED, node_coverage  # noqa: E402
from cauce_server.main import app  # noqa: E402

BASE_TS = 1787356800000
HOUR = 3600 * 1000


@pytest.fixture()
def client():
    db.reset_for_tests()
    with TestClient(app) as c:
        yield c


def send(client, timestamps, node_id="CAUCE-001", variable="air_temperature"):
    measurements = [
        {
            "sequence": i + 1,
            "timestamp_utc_ms": ts,
            "variable": variable,
            "value": 20.0,
            "unit": "degC",
            "quality": "VALID",
        }
        for i, ts in enumerate(timestamps)
    ]
    response = client.post("/v1/sync", json={
        "protocol_version": 1, "node_id": node_id, "measurements": measurements,
    })
    assert response.status_code == 200, response.text


def test_no_data_does_not_claim_truncation():
    """The old field said truncated at exactly twenty, having dropped nothing. With
    no data at all there is at most the one synthetic whole-window gap."""
    client = _client
    report = node_coverage("CAUCE-001", "air_temperature", BASE_TS,
                           BASE_TS + 24 * HOUR, 3600 * 1000)
    assert report["gaps_truncated"] is False
    assert report["gap_count_total"] >= report["gap_count_reported"]
    assert report["gap_count_reported"] <= MAX_GAPS_REPORTED


def test_exactly_the_cap_is_not_truncation():
    """The old field said truncated at exactly twenty, having dropped nothing."""
    client = _client
    # Nineteen interior gaps plus the two synthetic ends.
    timestamps = []
    cursor = BASE_TS
    for _ in range(19):
        timestamps.append(cursor)
        cursor += 5 * HOUR
    timestamps.append(cursor)
    send(client, timestamps)
    report = node_coverage("CAUCE-001", "air_temperature", BASE_TS,
                             cursor + 5 * HOUR, 3600 * 1000)
    assert report["gap_count_total"] <= MAX_GAPS_REPORTED + 2
    assert report["gaps_truncated"] is False


def test_a_truncated_report_says_how_many_were_dropped():
    client = _client
    timestamps = []
    cursor = BASE_TS
    for _ in range(MAX_GAPS_REPORTED + 12):
        timestamps.append(cursor)
        cursor += 5 * HOUR
    timestamps.append(cursor)
    send(client, timestamps)
    report = node_coverage("CAUCE-001", "air_temperature", BASE_TS,
                             cursor + 5 * HOUR, 3600 * 1000)

    assert report["gaps_truncated"] is True
    assert report["gap_count_reported"] == MAX_GAPS_REPORTED
    assert report["gap_count_total"] > report["gap_count_reported"]
    # The point of the whole change: the caller can tell 21 from 21,000.
    assert report["gap_count_total"] - report["gap_count_reported"] >= 12


def test_the_total_never_undercounts_the_reported_gaps():
    client = _client
    timestamps = [BASE_TS, BASE_TS + 5 * HOUR, BASE_TS + 30 * HOUR]
    send(client, timestamps)
    report = node_coverage("CAUCE-001", "air_temperature", BASE_TS,
                             BASE_TS + 40 * HOUR, 3600 * 1000)
    assert report["gap_count_total"] >= report["gap_count_reported"]
    assert report["gaps_truncated"] is False


_client = None


@pytest.fixture(autouse=True)
def _bind_client(request):
    """Hands each test the same TestClient the fixture above builds."""
    global _client
    db.reset_for_tests()
    with TestClient(app) as c:
        _client = c
        yield c