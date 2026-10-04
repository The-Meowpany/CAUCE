"""Backup and restore, actually exercised.

An untested backup is a hypothesis. These do the whole round trip against a real
SQLite file: take the backup through the endpoint, wipe the database, restore, and
assert the data is back with its row counts.
"""

from __future__ import annotations

import os
import sqlite3
import sys
from pathlib import Path

import pytest
from fastapi.testclient import TestClient

os.environ["CAUCE_DB_PATH"] = "./data/test_backup.sqlite"

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

import restore as restore_tool  # noqa: E402
from cauce_server import db  # noqa: E402
from cauce_server.config import settings  # noqa: E402
from cauce_server.main import app  # noqa: E402
from conftest import ADMIN_HEADERS, NO_AUTH

ADMIN = {"Authorization": "Bearer admin-token"}
BASE_TS = 1787356800000


@pytest.fixture()
def client(monkeypatch):
    monkeypatch.setattr(settings, "api_token", "admin-token")
    db.reset_for_tests()
    with TestClient(app, headers=ADMIN_HEADERS) as c:
        yield c


HMAC_KEY = "clave-dispositivo-0123456789"


def send(client, count=5, first_sequence=1):
    import hashlib
    import hmac
    import json

    measurements = [
        {
            "sequence": first_sequence + i,
            "timestamp_utc_ms": BASE_TS + (first_sequence + i - 1) * 60000,
            "variable": "air_temperature",
            "value": 20.0 + i,
            "unit": "degC",
            "quality": "VALID",
        }
        for i in range(count)
    ]
    payload = {"protocol_version": 1, "node_id": "CAUCE-001",
               "measurements": measurements}
    # Signed because provisioning a device key is what switches the sync path to
    # requiring a signature. The central verifies the raw bytes, so the test sends
    # exactly what it signed rather than letting the client re-serialise.
    raw = json.dumps(payload).encode()
    headers = dict(ADMIN)
    headers["Content-Type"] = "application/json"
    headers["X-Cauce-Node"] = "CAUCE-001"
    headers["X-Cauce-Signature"] = hmac.new(
        HMAC_KEY.encode(), raw, hashlib.sha256).hexdigest()
    response = client.post("/v1/sync", content=raw, headers=headers)
    assert response.status_code == 200, response.text
    return len(measurements)


def live_db_path() -> Path:
    return Path(db.engine_path()) if hasattr(db, "engine_path") else Path(
        settings.db_path)


# --- the backup the API already takes --------------------------------------

def test_the_backup_endpoint_verifies(client):
    send(client)
    response = client.get("/v1/maintenance/backup", headers=ADMIN)
    assert response.status_code == 200, response.text
    assert response.headers["content-type"].startswith("application/x-sqlite3")

    target = Path("./data/test_backup_download.sqlite")
    target.write_bytes(response.content)
    assert restore_tool.integrity_ok(target) is None
    assert restore_tool.row_counts(target)["measurements"] == 5


def test_the_backup_needs_admin(client):
    send(client)
    assert client.get("/v1/maintenance/backup", headers=NO_AUTH).status_code == 401


# --- the round trip, for real ----------------------------------------------

def test_a_backup_restores_after_the_database_is_wiped(client):
    """The whole point. Take the backup, destroy the live data, restore, and check
    the measurements came back."""
    sent = send(client, 7)
    backup_bytes = client.get("/v1/maintenance/backup", headers=ADMIN).content
    backup = Path("./data/test_backup_roundtrip.sqlite")
    backup.write_bytes(backup_bytes)
    assert restore_tool.row_counts(backup)["measurements"] == sent

    # Destroy it the way an operator would: delete every measurement row.
    with db.transaction() as conn:
        conn.execute("DELETE FROM measurements")
    assert restore_tool.row_counts(live_db_path())["measurements"] == 0

    db.close_engine()
    restore_tool.restore(backup, live_db_path())

    assert restore_tool.row_counts(live_db_path())["measurements"] == sent
    rows = client.get("/v1/nodes/CAUCE-001/measurements", headers=ADMIN).json()
    measurements = rows.get("measurements", rows.get("items", []))
    assert len(measurements) == sent


def test_a_restore_preserves_the_node_and_its_key(client):
    # An HMAC node, because that is the only algorithm the JSON Wi-Fi sync path can
    # currently authenticate. An Ed25519 node syncs through the frame path. The
    # point of this test is that the key row survives, not which algorithm it holds.
    key = HMAC_KEY
    provisioned = client.post("/v1/provision", headers=ADMIN, json={
        "node_id": "CAUCE-001",
        "device_key": key,
    })
    assert provisioned.status_code == 200, provisioned.text
    send(client, 3)
    backup = Path("./data/test_backup_node.sqlite")
    backup.write_bytes(
        client.get("/v1/maintenance/backup", headers=ADMIN).content)

    with db.transaction() as conn:
        conn.execute("DELETE FROM measurements")
    db.close_engine()
    restore_tool.restore(backup, live_db_path())

    rows = db.query("SELECT device_key FROM nodes WHERE node_id='CAUCE-001'")
    assert rows, "the node row did not survive the restore"
    assert rows[0]["device_key"] == key


# --- refusals --------------------------------------------------------------

def test_a_corrupt_backup_is_refused(tmp_path):
    bad = tmp_path / "corrupt.sqlite"
    bad.write_bytes(b"this is not a database" * 100)
    with pytest.raises(SystemExit) as excinfo:
        restore_tool.restore(bad, tmp_path / "target.sqlite")
    assert "refusing" in str(excinfo.value).lower()


def test_an_empty_file_is_refused(tmp_path):
    empty = tmp_path / "empty.sqlite"
    empty.write_bytes(b"")
    with pytest.raises(SystemExit):
        restore_tool.restore(empty, tmp_path / "target.sqlite")


def test_a_backup_missing_a_table_is_refused(tmp_path):
    """A database that is a valid SQLite file but not a CAUCE database. Restoring it
    would leave a server that starts and answers every request with an empty table."""
    partial = tmp_path / "partial.sqlite"
    conn = sqlite3.connect(partial)
    conn.execute("CREATE TABLE nodes(node_id TEXT PRIMARY KEY)")
    conn.commit()
    conn.close()
    with pytest.raises(SystemExit) as excinfo:
        restore_tool.restore(partial, tmp_path / "target.sqlite")
    assert "missing tables" in str(excinfo.value)


def test_a_restore_that_would_lose_rows_is_refused(client):
    """A backup with fewer measurements than the live database is refused by
    default. Refusing is the right default: the operator can pass --yes, but they
    have to notice."""
    send(client, 9)
    small = Path("./data/test_backup_small.sqlite")
    small.write_bytes(
        client.get("/v1/maintenance/backup", headers=ADMIN).content)

    # Add more data after the backup was taken, so the backup is now the smaller
    # one. Distinct sequences: the central dedupes by (node_id, sequence), so
    # resending 1..9 would be dropped and the live count would not grow.
    send(client, 9, first_sequence=10)
    assert restore_tool.row_counts(live_db_path())["measurements"] == 18
    assert restore_tool.row_counts(small)["measurements"] == 9

    with pytest.raises(SystemExit) as excinfo:
        db.close_engine()
        restore_tool.restore(small, live_db_path())
    assert "fewer rows" in str(excinfo.value).lower()

    # And --yes does it, which is the point of making the refusal overridable.
    db.close_engine()
    restore_tool.restore(small, live_db_path(), assume_yes=True)
    assert restore_tool.row_counts(live_db_path())["measurements"] == 9


def test_a_dry_run_changes_nothing(client, capsys):
    send(client, 4)
    backup = Path("./data/test_backup_dryrun.sqlite")
    backup.write_bytes(
        client.get("/v1/maintenance/backup", headers=ADMIN).content)
    before = restore_tool.row_counts(live_db_path())

    assert restore_tool.main([
        "--backup", str(backup), "--db", str(live_db_path()), "--dry-run"]) == 0

    assert restore_tool.row_counts(live_db_path()) == before
    assert "dry run" in capsys.readouterr().out


def test_a_refused_restore_leaves_the_live_database_alone(client):
    send(client, 5)
    good = Path("./data/test_backup_good.sqlite")
    good.write_bytes(
        client.get("/v1/maintenance/backup", headers=ADMIN).content)
    before = restore_tool.row_counts(live_db_path())

    bad = Path("./data/test_backup_bad.sqlite")
    bad.write_bytes(b"garbage" * 50)
    with pytest.raises(SystemExit):
        restore_tool.restore(bad, live_db_path())

    assert restore_tool.row_counts(live_db_path()) == before
