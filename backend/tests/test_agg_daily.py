"""Daily aggregates.

`agg_daily` is derived from `agg_hourly` by trigger, so the interesting tests
are not "does a daily row exist" but "does it agree with the hourly buckets it
was derived from". A trigger that silently drops all but the first hour of a day
would still produce plausible-looking rows.
"""

from __future__ import annotations

import os

import pytest
from fastapi.testclient import TestClient

os.environ["CAUCE_DB_PATH"] = "./data/test_daily.sqlite"

from cauce_server import db  # noqa: E402
from cauce_server.db import query  # noqa: E402
from cauce_server.main import app  # noqa: E402
from test_api import BASE_TS, _series, sync_payload  # noqa: E402

DAY_MS = 86400000


@pytest.fixture()
def client():
    db.reset_for_tests()
    with TestClient(app) as c:
        yield c


def _fill_days(client, node_id="CAUCE-001", days=3, per_day=24, start=BASE_TS):
    total = days * per_day
    client.post("/v1/sync", json=sync_payload(
        _series(node_id, start, total, step_ms=3600000, base=20.0,
                ramp=0.1, start_seq=1),
        node_id=node_id))


def _daily(node_id="CAUCE-001"):
    return query("SELECT * FROM agg_daily WHERE node_id=? ORDER BY day_ts",
                 (node_id,))


def test_daily_rows_collapse_hours_into_days(client):
    _fill_days(client, days=3)
    rows = _daily()
    assert len(rows) == 3, "three days of hourly data must make three rows"
    for row in rows:
        assert row["cnt"] == 24


def test_daily_totals_agree_with_hourly(client):
    _fill_days(client, days=4)
    hourly = query("SELECT COUNT(*) AS buckets, SUM(cnt) AS samples"
                   " FROM agg_hourly WHERE node_id='CAUCE-001'")[0]
    daily = query("SELECT COUNT(*) AS buckets, SUM(cnt) AS samples"
                  " FROM agg_daily WHERE node_id='CAUCE-001'")[0]
    assert daily["samples"] == hourly["samples"]
    assert daily["buckets"] == 4
    assert hourly["buckets"] == 96


def test_daily_extremes_span_the_whole_day(client):
    _fill_days(client, days=2, per_day=24)
    rows = _daily()
    hours = query("SELECT MIN(min_v) AS lo, MAX(max_v) AS hi FROM agg_hourly"
                  " WHERE node_id='CAUCE-001'")[0]
    assert min(r["min_v"] for r in rows) == pytest.approx(hours["lo"])
    assert max(r["max_v"] for r in rows) == pytest.approx(hours["hi"])


def test_a_second_hour_updates_the_day_instead_of_creating_a_row(client):
    # The regression that matters: an INSERT trigger that ignores conflicts
    # keeps only the first hour and loses the other 23.
    client.post("/v1/sync", json=sync_payload(
        _series("CAUCE-001", BASE_TS, 1, step_ms=3600000, base=20.0,
                start_seq=1), node_id="CAUCE-001"))
    assert _daily()[0]["cnt"] == 1

    client.post("/v1/sync", json=sync_payload(
        _series("CAUCE-001", BASE_TS + 3600000, 1, step_ms=3600000, base=21.0,
                start_seq=2), node_id="CAUCE-001"))
    rows = _daily()
    assert len(rows) == 1
    assert rows[0]["cnt"] == 2


def test_repeated_ingest_does_not_double_count(client):
    recs = _series("CAUCE-001", BASE_TS, 6, step_ms=3600000, base=20.0)
    body = sync_payload(recs, node_id="CAUCE-001")
    client.post("/v1/sync", json=body)
    first = _daily()[0]["cnt"]
    client.post("/v1/sync", json=body)
    client.post("/v1/sync", json=body)
    assert _daily()[0]["cnt"] == first == 6


def test_daily_granularity_is_selectable_and_says_so(client):
    _fill_days(client, days=3)
    body = client.get(f"/v1/analytics/summary?node_id=CAUCE-001"
                      "&variable=air_temperature"
                      f"&from_utc_ms={BASE_TS}"
                      f"&to_utc_ms={BASE_TS + 3 * DAY_MS}"
                      "&granularity=daily").json()
    assert body["granularity"] == "daily"
    assert body["source"] == "materialized_daily"
    assert body["buckets"] == 3
    assert body["count"] == 72


def test_auto_stays_hourly_below_the_daily_threshold(client):
    _fill_days(client, days=10)
    body = client.get(f"/v1/analytics/summary?node_id=CAUCE-001"
                      "&variable=air_temperature"
                      f"&from_utc_ms={BASE_TS}"
                      f"&to_utc_ms={BASE_TS + 10 * DAY_MS}").json()
    # Ten days is long enough for hourly but nowhere near the 120-day mark, so
    # daily would hide intra-day peaks nobody asked to lose.
    assert body["granularity"] == "hourly"


def test_auto_switches_to_daily_past_the_threshold(client):
    days = 130
    # One sample per day, so the window really is 130 days long.
    client.post("/v1/sync", json=sync_payload(
        _series("CAUCE-001", BASE_TS, days, step_ms=DAY_MS, base=20.0,
                ramp=0.01, start_seq=1), node_id="CAUCE-001"))
    body = client.get(f"/v1/analytics/summary?node_id=CAUCE-001"
                      "&variable=air_temperature"
                      f"&from_utc_ms={BASE_TS}"
                      f"&to_utc_ms={BASE_TS + days * DAY_MS}").json()
    assert body["granularity"] == "daily"
    assert body["source"] == "materialized_daily"
    assert body["buckets"] == days


def test_daily_agrees_with_raw_on_mean(client):
    _fill_days(client, days=5, per_day=8, start=BASE_TS)
    window = (f"&from_utc_ms={BASE_TS}&to_utc_ms={BASE_TS + 5 * DAY_MS}")
    daily = client.get(f"/v1/analytics/summary?node_id=CAUCE-001"
                       f"&variable=air_temperature{window}"
                       "&granularity=daily").json()
    raw = client.get(f"/v1/analytics/summary?node_id=CAUCE-001"
                     f"&variable=air_temperature{window}"
                     "&granularity=raw").json()
    assert daily["count"] == raw["count"]
    assert daily["mean"] == pytest.approx(raw["mean"], abs=0.001)


def test_daily_applies_calibration(client):
    client.post("/v1/sites", json={"site_id": "s1"})
    _fill_days(client, days=3)
    with db.transaction() as conn:
        conn.execute("UPDATE nodes SET site_id='s1' WHERE node_id='CAUCE-001'")
    client.put("/v1/sites/s1/calibration", json={
        "variable": "air_temperature", "scale": 1.0, "offset": -2.0,
        "uncertainty": 0.5, "uncertainty_kind": "co_location_spread"})

    body = client.get(f"/v1/analytics/summary?node_id=CAUCE-001"
                      "&variable=air_temperature"
                      f"&from_utc_ms={BASE_TS}"
                      f"&to_utc_ms={BASE_TS + 3 * DAY_MS}"
                      "&granularity=daily").json()
    assert body["granularity"] == "daily"
    assert body["calibrated"]["mean"] == pytest.approx(body["mean"] - 2.0)
    assert body["calibration"]["uncertainty"] == 0.5


def test_invalid_granularity_is_rejected(client):
    assert client.get("/v1/analytics/summary?node_id=CAUCE-001"
                      "&variable=air_temperature&granularity=weekly"
                      ).status_code == 422
    assert client.get("/v1/analytics/summary?node_id=CAUCE-001"
                      "&variable=air_temperature&granularity=yearly"
                      ).status_code == 422


def test_daily_falls_back_to_raw_when_buckets_are_missing(client):
    _fill_days(client, days=3)
    with db.transaction() as conn:
        conn.execute("DELETE FROM agg_daily WHERE node_id='CAUCE-001'")
    body = client.get(f"/v1/analytics/summary?node_id=CAUCE-001"
                      "&variable=air_temperature"
                      f"&from_utc_ms={BASE_TS}"
                      f"&to_utc_ms={BASE_TS + 3 * DAY_MS}").json()
    assert body["granularity"] == "raw"


def test_healthz_reports_the_daily_table(client):
    body = client.get("/healthz").json()
    assert body["tables"]["agg_daily"] == 0
