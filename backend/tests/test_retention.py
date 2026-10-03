"""Retention across every table that can grow.

`purge_older_than` originally cleaned `measurements` and `agg_hourly` only.
Every table added since was left to grow without bound, and two of them are
event-driven: a flapping sensor fills `alert_log`, and diagnostics upload on
every reconnect. On a 90-day pilot that is a real disk problem, not a tidy-up.

The interesting case is `commands`. A pending or delivered command is the only
record that an operator still asked for something, so retention deletes only
settled ones; expiring a pending command would turn "not delivered yet" into
"never sent".
"""

from __future__ import annotations

import os

import pytest
from fastapi.testclient import TestClient

os.environ["CAUCE_DB_PATH"] = "./data/test_retention.sqlite"

from cauce_server import db  # noqa: E402
from cauce_server.db import query  # noqa: E402
from cauce_server.main import app  # noqa: E402
from cauce_server.retention import purge_older_than  # noqa: E402
from test_api import BASE_TS, _series, sync_payload  # noqa: E402

DAY_MS = 86400000
OLD = BASE_TS  # a fixture timestamp far enough in the past to be purged


@pytest.fixture()
def client():
    db.reset_for_tests()
    with TestClient(app) as c:
        yield c


def _seed_everything(client):
    """One row of every purgeable kind, all at an old timestamp."""
    client.post("/v1/sync", json=sync_payload(
        _series("CAUCE-001", OLD, 3), node_id="CAUCE-001"))

    with db.transaction() as conn:
        conn.execute(
            "INSERT INTO alert_log(rule_id, node_id, fired_utc_ms, message)"
            " VALUES(1,'CAUCE-001',?,?)", (OLD, "old alert"))
        conn.execute(
            "INSERT INTO node_diagnostics(node_id, received_at_utc_ms,"
            " bundle_json) VALUES('CAUCE-001',?,'{}')", (OLD,))
        conn.execute(
            "INSERT INTO sites(site_id, name, notes, is_control)"
            " VALUES('s1','s','n',0)")
        conn.execute(
            "INSERT INTO maintenance_events(site_id, kind, at_utc_ms)"
            " VALUES('s1','note',?)", (OLD,))
        conn.execute(
            "INSERT INTO commands(node_id, idempotency_key, kind, payload,"
            " created_at_utc_ms, acked_utc_ms) VALUES"
            " ('CAUCE-001','acked','request_resync','{}',?,1)", (OLD,))
        conn.execute(
            "INSERT INTO commands(node_id, idempotency_key, kind, payload,"
            " created_at_utc_ms) VALUES"
            " ('CAUCE-001','pending','request_resync','{}',?)", (OLD,))
        conn.execute(
            "INSERT INTO commands(node_id, idempotency_key, kind, payload,"
            " created_at_utc_ms) VALUES"
            " ('CAUCE-001','delivered','request_resync','{}',?)", (OLD,))
        # sync_batches is stamped with server time by /v1/sync, so an old row
        # has to be planted explicitly to exercise its purge.
        conn.execute(
            "UPDATE sync_batches SET received_at_utc_ms=?", (OLD,))


def test_retention_clears_every_time_bounded_table(client):
    _seed_everything(client)
    state = purge_older_than(1, vacuum=False)

    assert state["deleted_measurements"] >= 1
    assert state["deleted_alert_log"] == 1
    assert state["deleted_diagnostics"] == 1
    assert state["deleted_maintenance_events"] == 1
    assert state["deleted_sync_batches"] >= 1
    # Three commands, only the acked one is settled.
    assert state["deleted_commands"] == 1


def test_a_pending_command_survives_retention(client):
    _seed_everything(client)
    purge_older_than(1, vacuum=False)
    rows = query("SELECT idempotency_key FROM commands ORDER BY command_id")
    keys = [r["idempotency_key"] for r in rows]
    assert "acked" not in keys
    # Unsent and seen-but-unconfirmed are exactly the rows an operator needs.
    assert "pending" in keys
    assert "delivered" in keys


def test_orphaned_receipts_are_removed(client):
    _seed_everything(client)
    with db.transaction() as conn:
        conn.execute(
            "INSERT INTO command_receipts(node_id, command_id, state, detail,"
            " at_utc_ms) VALUES('CAUCE-001',9999,'acked','stale',?)", (OLD,))
    state = purge_older_than(1, vacuum=False)
    assert state["deleted_command_receipts"] == 1
    assert query("SELECT 1 FROM command_receipts") == []


def test_receipts_for_a_surviving_command_are_kept(client):
    _seed_everything(client)
    with db.transaction() as conn:
        row = conn.execute(
            "SELECT command_id FROM commands WHERE idempotency_key='pending'"
        ).fetchone()
        conn.execute(
            "INSERT INTO command_receipts(node_id, command_id, state, detail,"
            " at_utc_ms) VALUES('CAUCE-001',?,'delivered','',?)",
            (row["command_id"], OLD))
    purge_older_than(1, vacuum=False)
    remaining = query("SELECT 1 FROM command_receipts")
    assert len(remaining) == 1, "a receipt for a live command must not be purged"


def test_daily_buckets_are_purged_too(client):
    client.post("/v1/sync", json=sync_payload(
        _series("CAUCE-001", OLD, 3), node_id="CAUCE-001"))
    assert query("SELECT 1 FROM agg_daily") != []
    state = purge_older_than(1, vacuum=False)
    assert state["deleted_daily_buckets"] >= 1
    assert query("SELECT 1 FROM agg_daily") == []


def test_retention_reports_every_counter(client):
    _seed_everything(client)
    state = purge_older_than(1, vacuum=False)
    for key in ("deleted_measurements", "deleted_hourly_buckets",
                "deleted_daily_buckets", "deleted_alert_log",
                "deleted_diagnostics", "deleted_maintenance_events",
                "deleted_commands", "deleted_command_receipts",
                "deleted_sync_batches"):
        assert key in state, key


def test_recent_data_is_untouched(client):
    import time

    now = int(time.time() * 1000)
    client.post("/v1/sync", json=sync_payload(
        _series("CAUCE-001", now, 3), node_id="CAUCE-001"))
    purge_older_than(365, vacuum=False)
    assert len(query("SELECT 1 FROM measurements")) == 3


def test_retention_endpoint_reports_the_new_counters(client):
    _seed_everything(client)
    body = client.post("/v1/maintenance/retention", json={
        "older_than_days": 1}).json()
    assert body["deleted_alert_log"] == 1
    assert "deleted_commands" in body
    assert "deleted_daily_buckets" in body
