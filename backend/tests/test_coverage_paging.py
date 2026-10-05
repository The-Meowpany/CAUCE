"""Paging through the gaps of a coverage window.

The window can hold far more gaps than one response should carry. Reporting the twenty
longest and saying only that there were more leaves a caller unable to reach the rest,
so the list is pageable. Two properties are worth protecting, and this file checks them
separately because each has its own way of being wrong:

- a total order in SQL. `ORDER BY delta DESC` alone is not one: SQLite may return
  equal-length gaps in any order, so LIMIT/OFFSET across pages can drop a gap and repeat
  another. The tiebreak on `ts` is what makes the order total.
- a last page that says so. `gaps_truncated` was `gap_count_total > gap_count_reported`,
  which is correct only at offset zero and reports "more available" forever on a set that
  has already been fully read, so a caller following it would loop until it ran out of
  patience rather than until it ran out of gaps.
"""

from __future__ import annotations

import os

import pytest
from fastapi.testclient import TestClient

os.environ["CAUCE_DB_PATH"] = "./data/test_coverage_paging.sqlite"

from cauce_server import db  # noqa: E402
from cauce_server.coverage import MAX_GAPS_PER_PAGE, node_coverage  # noqa: E402
from cauce_server.main import app  # noqa: E402
from conftest import ADMIN_HEADERS

BASE_TS = 1787356800000
HOUR = 3600 * 1000
MINUTE = 60 * 1000


@pytest.fixture()
def client():
    db.reset_for_tests()
    with TestClient(app, headers=ADMIN_HEADERS) as c:
        yield c


def send(client, timestamps, node_id="PAGER-001"):
    measurements = [
        {
            "sequence": i + 1,
            "timestamp_utc_ms": ts,
            "variable": "air_temperature",
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


def seed(client, lengths, node_id="PAGER-001"):
    """A measurement every `BASE_TS`, then one after each gap in `lengths`.

    Returns the timestamp of the last measurement. Gaps are deliberately mixed and
    repeated, because an unstable sort only shows up when there is something to be
    unstable about: every gap here has the same length as at least one other.
    """
    timestamps = [BASE_TS]
    cursor = BASE_TS
    for length in lengths:
        cursor += length
        timestamps.append(cursor)
    send(client, timestamps, node_id)
    return cursor


def page(node_id, to_ms, offset, limit):
    return node_coverage(node_id, "air_temperature", BASE_TS, to_ms, MINUTE,
                         gap_offset=offset, gap_limit=limit)


def test_paging_returns_every_gap_exactly_once(client):
    """Twenty-five gaps over three pages: nothing dropped, nothing repeated."""
    lengths = [
        5, 5, 9, 3, 7, 5, 11, 4, 6, 6, 13,
        4, 8, 5, 10, 3, 7, 5, 12, 6, 4, 9, 5, 7, 5,
    ]
    last = seed(client, [n * MINUTE for n in lengths])

    seen_starts = []
    durations = []
    pages = 0
    while True:
        result = page("PAGER-001", last, pages * 10, 10)
        seen_starts.extend(g["start_utc_ms"] for g in result["gaps"])
        durations.extend(g["duration_ms"] for g in result["gaps"])
        if not result["gaps_truncated"]:
            break
        pages += 1
        assert pages < 10, "paging never terminated"

    assert pages == 2, f"expected three pages, walked {pages + 1}"
    # Identity is the start timestamp, not the duration. Two gaps of the same length are
    # perfectly normal, so asserting on durations would flag correct paging as a duplicate
    # bug - which is what the first version of this test did.
    assert len(seen_starts) == len(set(seen_starts)), "paging repeated a gap"
    assert sorted(durations) == sorted(n * MINUTE for n in lengths)


def test_the_last_page_says_nothing_is_left(client):
    """The infinite-loop guard: the final page must report no truncation."""
    last = seed(client, [4 * MINUTE] * 12, "PAGER-002")

    result = page("PAGER-002", last, 10, 10)
    assert len(result["gaps"]) == 2
    assert result["gaps_truncated"] is False
    assert result["gap_offset"] == 10
    assert result["gap_count_total"] == 12


def test_offset_past_the_end_returns_nothing_and_claims_nothing_more(client):
    last = seed(client, [6 * MINUTE, 6 * MINUTE], "PAGER-003")

    result = page("PAGER-003", last, 99, 10)
    assert result["gaps"] == []
    assert result["gaps_truncated"] is False
    assert result["gap_count_total"] == 2


def test_longest_gap_is_the_windows_longest_not_the_pages(client):
    """Paging must not make the site's worst outage look like it is improving.

    The single worst gap sorts onto page one, so a caller reading page two would
    otherwise conclude the worst outage shrank just because they asked for more.
    """
    last = seed(client, [60 * MINUTE, 3 * MINUTE, 3 * MINUTE, 3 * MINUTE], "PAGER-004")

    first = page("PAGER-004", last, 0, 2)
    second = page("PAGER-004", last, 2, 2)
    assert first["longest_gap_ms"] == 60 * MINUTE
    assert second["longest_gap_ms"] == 60 * MINUTE


def test_the_default_page_still_reports_the_twenty_longest(client):
    """Existing callers pass no paging arguments and must see unchanged behaviour."""
    # Every length is over the two-minute gap threshold, so all thirty really are gaps.
    lengths = [float(34 - i) * MINUTE for i in range(30)]
    last = seed(client, lengths, "PAGER-005")

    result = node_coverage("PAGER-005", "air_temperature", BASE_TS, last, MINUTE)

    assert len(result["gaps"]) == 20
    assert result["gap_count_total"] == 30
    assert result["gaps_truncated"] is True
    durations = [g["duration_ms"] for g in result["gaps"]]
    assert durations == sorted(durations, reverse=True)


def test_a_page_cannot_be_used_to_drain_the_whole_window(client):
    """`gap_limit` is capped, or pagination would work and then be pointless."""
    last = seed(client, [5 * MINUTE] * 8, "PAGER-006")

    result = page("PAGER-006", last, 0, 100_000)
    assert result["gap_limit"] == MAX_GAPS_PER_PAGE
    assert len(result["gaps"]) <= MAX_GAPS_PER_PAGE


def test_the_trailing_gap_is_reported_on_later_pages(client):
    """It has no rank in the SQL order, so it is exposed rather than appended blindly.

    Appending it to every page would break a caller's loop: each page would carry one more
    gap than was asked for, and the extra one would repeat on every page after.
    """
    last = seed(client, [5 * MINUTE] * 6, "PAGER-007")
    # A window running well past the last measurement, so a trailing gap exists.
    to_ms = last + 30 * MINUTE

    second = page("PAGER-007", to_ms, 4, 4)
    assert second["trailing_gap"] is not None
    assert second["trailing_gap"]["duration_ms"] == 30 * MINUTE
    assert all(g["duration_ms"] != 30 * MINUTE for g in second["gaps"])

    # On page one it is merged in and ranked, so the counted total includes it.
    first = page("PAGER-007", to_ms, 0, 4)
    assert first["gap_count_total"] == 7
    assert first["trailing_gap"] is not None
    assert 30 * MINUTE in [g["duration_ms"] for g in first["gaps"]]


def test_no_data_at_all_still_reports_the_one_whole_window_gap(client):
    """The empty branch must not raise, and must keep its existing shape."""
    send(client, [BASE_TS], "PAGER-008")
    result = page("PAGER-008", BASE_TS + 24 * HOUR, 0, 20)

    assert result["gap_count_total"] >= 1
    assert result["gap_count_reported"] >= 1
    assert result["gaps_truncated"] is False


def test_a_negative_offset_is_refused_by_the_endpoint(client):
    """The endpoint validates it, so a bad request is a 422 and not a 404 or a 500."""
    seed(client, [5 * MINUTE], "PAGER-010")
    response = client.get(
        "/v1/nodes/PAGER-010/coverage",
        params={"gap_offset": -5, "expected_interval_ms": MINUTE,
                "from_utc_ms": BASE_TS, "to_utc_ms": BASE_TS + HOUR},
    )
    assert response.status_code == 422, response.text
    assert response.json()["detail"] == "invalid_gap_offset"


def test_the_endpoint_pages_and_echoes_the_position(client):
    last = seed(client, [3 * MINUTE] * 9, "PAGER-009")

    response = client.get(
        "/v1/nodes/PAGER-009/coverage",
        params={"gap_offset": 4, "gap_limit": 4,
                "expected_interval_ms": MINUTE,
                "from_utc_ms": BASE_TS, "to_utc_ms": last},
    )
    assert response.status_code == 200, response.text
    body = response.json()
    assert body["gap_offset"] == 4
    assert body["gap_limit"] == 4
    assert len(body["gaps"]) == 4
