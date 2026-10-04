"""15-minute aggregates: the tier between raw and hourly.

The property under test is not "the table exists" but that the tier is only
chosen when it is actually a saving and when it holds enough of the data. Both
failure modes are silent: an aggregate that returns more rows than the raw table
it replaced costs time and teaches nothing, and one that is missing samples
reports a mean over a subset that looks exactly like a real change.
"""

import os

import pytest
from fastapi.testclient import TestClient

os.environ["CAUCE_DB_PATH"] = "./data/test_agg15.sqlite"

from cauce_server import db  # noqa: E402
from cauce_server.db import query  # noqa: E402
from cauce_server.main import app  # noqa: E402
from conftest import ADMIN_HEADERS

BASE_TS = 1787356800000  # aligned to a UTC hour
MIN = 60 * 1000
Q15 = 15 * MIN
HOUR = 60 * MIN


@pytest.fixture()
def client():
    db.reset_for_tests()
    with TestClient(app, headers=ADMIN_HEADERS) as c:
        yield c


def send(client, node_id, count, start=BASE_TS, step=MIN, value=20.0):
    measurements = []
    for i in range(count):
        measurements.append({
            "sequence": i + 1,
            "timestamp_utc_ms": start + i * step,
            "variable": "air_temperature",
            "value": value + (i % 5) * 0.1,
            "unit": "degC",
            "quality": "VALID",
        })
    return client.post("/v1/sync", json={
        "protocol_version": 1, "node_id": node_id,
        "measurements": measurements,
    })


def summary(client, from_ms, to_ms, granularity="auto"):
    return client.get(
        f"/v1/analytics/summary?node_id=CAUCE-001&variable=air_temperature"
        f"&from_utc_ms={from_ms}&to_utc_ms={to_ms}"
        f"&granularity={granularity}").json()


class TestTableIsFed:
    def test_every_sample_lands_in_a_quarter_hour_bucket(self, client):
        send(client, "CAUCE-001", 10)
        rows = query("SELECT * FROM agg_15min WHERE node_id='CAUCE-001'")
        assert sum(r["cnt"] for r in rows) == 10

    def test_bucket_boundaries_are_aligned_to_fifteen_minutes(self, client):
        # One sample at :00, one at :14:59, one at :15:00 of the same hour.
        for i, ts in enumerate((BASE_TS, BASE_TS + 14 * MIN + 59_000,
                                BASE_TS + 15 * MIN)):
            client.post("/v1/sync", json={
                "protocol_version": 1, "node_id": "CAUCE-001",
                "measurements": [{
                    "sequence": i + 1, "timestamp_utc_ms": ts,
                    "variable": "air_temperature", "value": 20.0,
                    "unit": "degC", "quality": "VALID"}]})
        rows = query("SELECT bucket_ts, cnt FROM agg_15min"
                     " WHERE node_id='CAUCE-001' ORDER BY bucket_ts")
        assert [r["bucket_ts"] for r in rows] == [
            BASE_TS, BASE_TS + Q15]
        assert [r["cnt"] for r in rows] == [2, 1]

    def test_a_null_value_is_not_counted(self, client):
        client.post("/v1/sync", json={
            "protocol_version": 1, "node_id": "CAUCE-001",
            "measurements": [{
                "sequence": 1, "timestamp_utc_ms": BASE_TS,
                "variable": "air_temperature", "value": None,
                "unit": "degC", "quality": "MISSING"}]})
        rows = query("SELECT * FROM agg_15min WHERE node_id='CAUCE-001'")
        assert rows == []

    def test_min_max_and_sum_agree_with_the_raw_rows(self, client):
        send(client, "CAUCE-001", 8, value=10.0)
        row = query("SELECT * FROM agg_15min WHERE node_id='CAUCE-001'")[0]
        raw = query("SELECT COUNT(*) c, MIN(value) lo, MAX(value) hi,"
                    " SUM(value) s FROM measurements")[0]
        assert row["cnt"] == raw["c"]
        assert row["min_v"] == pytest.approx(raw["lo"])
        assert row["max_v"] == pytest.approx(raw["hi"])
        assert row["sum"] == pytest.approx(raw["s"])


class TestGranularitySelection:
    def test_a_window_of_a_few_hours_uses_15min(self, client):
        # 4 hours at one sample a minute is 240 raw rows; 15-minute buckets
        # make it 16.
        send(client, "CAUCE-001", 240)
        body = summary(client, BASE_TS, BASE_TS + 4 * HOUR)
        assert body["granularity"] == "15min"
        assert body["source"] == "materialized_15min"
        # `count` is samples summarised, not buckets: the point of the tier
        # is that 240 raw rows answer the question without being read.
        assert body["count"] == 240
        assert query("SELECT COUNT(*) c FROM agg_15min")[0]["c"] == 16

    def test_explicit_15min_is_honoured(self, client):
        send(client, "CAUCE-001", 240)
        body = summary(client, BASE_TS, BASE_TS + 4 * HOUR, "15min")
        assert body["granularity"] == "15min"

    def test_a_very_short_window_stays_raw(self, client):
        # Thirty minutes is under the minimum: folding 30 rows into 2 buckets
        # saves nothing and loses resolution.
        send(client, "CAUCE-001", 30)
        body = summary(client, BASE_TS, BASE_TS + 30 * MIN)
        assert body["granularity"] == "raw"

    def test_a_long_window_does_not_use_15min(self, client):
        """The tier must not fire where it would return more rows than raw."""
        send(client, "CAUCE-001", 6 * 24 * 60)
        body = summary(client, BASE_TS, BASE_TS + 6 * 24 * HOUR)
        assert body["granularity"] == "raw"

    def test_an_unknown_granularity_is_refused(self, client):
        send(client, "CAUCE-001", 10)
        assert client.get(
            "/v1/analytics/summary?node_id=CAUCE-001"
            "&variable=air_temperature&granularity=fortnightly").status_code \
            == 422

    def test_the_15min_mean_matches_the_raw_mean(self, client):
        send(client, "CAUCE-001", 240)
        raw = summary(client, BASE_TS, BASE_TS + 4 * HOUR, "raw")
        q15 = summary(client, BASE_TS, BASE_TS + 4 * HOUR, "15min")
        assert q15["mean"] == pytest.approx(raw["mean"])
        assert q15["min"] == pytest.approx(raw["min"])
        assert q15["max"] == pytest.approx(raw["max"])


class TestIncompleteAggregatesAreRefused:
    def test_auto_falls_back_when_the_buckets_are_purged(self, client):
        send(client, "CAUCE-001", 240)
        with db.transaction() as conn:
            conn.execute("DELETE FROM agg_15min WHERE node_id='CAUCE-001'")
        body = summary(client, BASE_TS, BASE_TS + 4 * HOUR)
        assert body["granularity"] == "raw", (
            "must not report 15-minute stats when the buckets were purged")

    def test_auto_falls_back_when_too_many_buckets_are_missing(self, client):
        send(client, "CAUCE-001", 240)
        with db.transaction() as conn:
            # Drop all but one bucket: 239 of 240 samples gone.
            conn.execute("DELETE FROM agg_15min WHERE node_id='CAUCE-001'"
                         " AND bucket_ts > ?", (BASE_TS,))
        body = summary(client, BASE_TS, BASE_TS + 4 * HOUR)
        assert body["granularity"] == "raw"

    def test_explicit_15min_still_answers_when_purged(self, client):
        """An explicit request is honoured; `auto` is the one that judges."""
        send(client, "CAUCE-001", 240)
        with db.transaction() as conn:
            conn.execute("DELETE FROM agg_15min WHERE node_id='CAUCE-001'")
        body = summary(client, BASE_TS, BASE_TS + 4 * HOUR, "15min")
        assert body["granularity"] == "15min"
        assert body["count"] == 0


class TestRetentionAndHealth:
    def test_retention_purges_the_15min_table_too(self, client):
        """A table the purge cannot reach grows without bound.

        The counter is the evidence: an aggregate table that is fed on every
        insert and never pruned is a slower leak than the raw one it summarises.
        """
        old = BASE_TS - 40 * 24 * HOUR
        send(client, "CAUCE-001", 240, start=old)
        assert query("SELECT COUNT(*) c FROM agg_15min")[0]["c"] > 0

        report = client.post("/v1/maintenance/retention",
                             json={"older_than_days": 30}).json()
        assert report["deleted_quarter_hourly_buckets"] == 16
        assert query("SELECT COUNT(*) c FROM agg_15min")[0]["c"] == 0

    def test_retention_leaves_recent_buckets_alone(self, client):
        # Anchored to the wall clock, not to BASE_TS: the fixtures sit in the
        # past, so "recent" has to mean recent relative to when the purge runs.
        import time

        now = (int(time.time() * 1000) // HOUR) * HOUR
        send(client, "CAUCE-001", 240, start=now - 4 * HOUR)
        client.post("/v1/maintenance/retention",
                    json={"older_than_days": 30})
        assert query("SELECT COUNT(*) c FROM agg_15min")[0]["c"] == 16

    def test_healthz_reports_the_table(self, client):
        body = client.get("/healthz").json()
        assert body["tables"]["agg_15min"] == 0
