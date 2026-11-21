"""/v1/analytics/series: the materialized buckets themselves, for charts.

The buckets must agree with the raw rows they summarize, a gap must stay a gap, and the
endpoint must not quietly return a different granularity from the one asked for.
"""

import pytest
from cauce_server import db
from cauce_server.main import app
from conftest import ADMIN_HEADERS
from fastapi.testclient import TestClient
from test_agg_15min import BASE_TS, HOUR, MIN, Q15, send


@pytest.fixture()
def client():
    db.reset_for_tests()
    with TestClient(app, headers=ADMIN_HEADERS) as c:
        yield c


def series(client, granularity="15min", **window):
    extra = "".join(f"&{k}={v}" for k, v in window.items())
    return client.get("/v1/analytics/series?node_id=CAUCE-001&variable=air_temperature"
                      f"&granularity={granularity}{extra}")


def test_quarter_hour_buckets_match_the_raw_rows(client):
    send(client, "CAUCE-001", 60)  # one sample a minute for an hour
    body = series(client).json()
    assert body["bucket_ms"] == Q15
    assert [b["bucket_ts"] for b in body["buckets"]] == [BASE_TS + i * Q15 for i in range(4)]
    first = body["buckets"][0]
    raw = [20.0 + (i % 5) * 0.1 for i in range(15)]
    assert first["count"] == 15
    assert first["mean"] == pytest.approx(sum(raw) / 15, abs=1e-4)
    assert first["min"] == pytest.approx(min(raw))
    assert first["max"] == pytest.approx(max(raw))


def test_a_gap_is_absent_not_zero(client):
    # 60 one-minute samples with the middle half missing: buckets 1 and 2 are empty.
    client.post("/v1/sync", json={"protocol_version": 1, "node_id": "CAUCE-001",
                                  "measurements": [{
                                      "sequence": i + 1,
                                      "timestamp_utc_ms": BASE_TS + i * MIN,
                                      "variable": "air_temperature", "value": 20.0,
                                      "unit": "degC", "quality": "VALID"}
                                      for i in range(60) if not 15 <= i < 45]})
    buckets = series(client).json()["buckets"]
    assert [b["bucket_ts"] for b in buckets] == [BASE_TS, BASE_TS + 3 * Q15]


def test_window_and_granularity(client):
    send(client, "CAUCE-001", 180)
    windowed = series(client, from_utc_ms=BASE_TS + HOUR + 5 * MIN,
                      to_utc_ms=BASE_TS + 2 * HOUR - 1).json()["buckets"]
    # The window is widened to whole buckets, never narrowed past a sample it covers.
    assert windowed[0]["bucket_ts"] == BASE_TS + HOUR
    assert windowed[-1]["bucket_ts"] == BASE_TS + HOUR + 3 * Q15
    hourly = series(client, "hourly").json()
    assert hourly["bucket_ms"] == HOUR and len(hourly["buckets"]) == 3
    assert series(client, "5min").status_code == 422


def test_calibration_applies_to_every_bucket(client):
    client.post("/v1/sites", json={"site_id": "s1"})
    send(client, "CAUCE-001", 15)
    with db.transaction() as conn:
        conn.execute("UPDATE nodes SET site_id='s1' WHERE node_id='CAUCE-001'")
    assert client.put("/v1/sites/s1/calibration", json={
        "variable": "air_temperature", "scale": -1.0, "offset": 0.0}).status_code == 200
    bucket = series(client).json()["buckets"][0]
    assert bucket["max"] == pytest.approx(-20.0)
    assert bucket["min"] == pytest.approx(-20.4)
    assert bucket["mean"] < 0
