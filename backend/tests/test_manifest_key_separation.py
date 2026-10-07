"""The firmware-update key must not be the data key, and the separation must be visible.

The exposure this covers: `nodes.device_key` is symmetric for an HMAC node, so it authenticates
*as* that node as well as for it. While the manifest signature was derived from it, the
credential that could authorise a firmware image was the credential every measurement arrived
under - so compromising one compromised the other.

Two things are asserted here that are easy to get wrong in opposite directions:

- A node with a separate `manifest_key` gets a signature from *that*, not from the data key.
- A node with none still gets a valid signature, and the central says out loud that it fell
  back. Refusing would strand every deployed node on its next update, which trades a real
  exposure for a guaranteed outage.
"""

from __future__ import annotations

import hashlib
import hmac
import json
import os
from pathlib import Path

import pytest

# Before the app is imported, exactly as `test_api.py` does it. `config.settings` captures the
# environment at import time, so a test that sets `CAUCE_DB_PATH` afterwards runs against
# whatever database the first import opened - which is row 13 in the technical-debt table, and
# is why this line is here rather than in a fixture.
os.environ.setdefault("CAUCE_DB_PATH", "./data/test_manifest_key.sqlite")

from cauce_server import db  # noqa: E402
from cauce_server.config import settings  # noqa: E402
from cauce_server.main import app  # noqa: E402
from conftest import ADMIN_HEADERS  # noqa: E402
from fastapi.testclient import TestClient  # noqa: E402

_client = TestClient(app)

DEVICE_KEY = "device-secret-value-1234567890"
MANIFEST_KEY = "manifest-secret-value-0987654321"


def _manifest(node_id: str) -> dict:
    """Through the real client, so middleware, routing and rate limiting all apply.

    Calling the handler function directly skips all three, and a test that does that is testing
    a function no request ever reaches.
    """
    # The admin token, because the endpoint requires one. Reads stay open only when no token is
    # configured at all; the conftest sets one precisely so writes fail closed.
    response = _client.get("/v1/ota/manifest", params={"node_id": node_id},
                           headers=ADMIN_HEADERS)
    assert response.status_code == 200, response.text
    return response.json()


def _register(node_id: str, *, manifest_key: str | None = None) -> None:
    # Through `transaction()`, the same path the application writes with. `db` exposes no
    # module-level `execute`, which is deliberate: writes go through a transaction so a
    # failure mid-statement cannot leave a half-updated node row.
    with db.transaction() as conn:
        conn.execute(
            "INSERT OR REPLACE INTO nodes(node_id, device_key, device_key_algorithm, "
            "manifest_key, manifest_key_algorithm, first_seen_utc_ms, last_seen_utc_ms) "
            "VALUES(?,?,?,?,?,0,0)",
            (node_id, DEVICE_KEY, "hmac-sha256", manifest_key,
             "hmac-sha256" if manifest_key else None),
        )


def _expected_signature(key_material: str, release: dict) -> str:
    canonical = f"{release['version']}|{release['sha256']}|{release['url']}|{release['total_size']}"
    return hmac.new(
        hashlib.sha256(key_material.encode("utf-8")).digest(),
        canonical.encode("utf-8"),
        hashlib.sha256,
    ).hexdigest()


@pytest.fixture
def release_file(tmp_path: Path, monkeypatch) -> Path:
    path = tmp_path / "release.json"
    path.write_text(
        json.dumps({
            "version": "0.2.0",
            "sha256": "a" * 64,
            "url": "https://central.example/firmware/0.2.0.bin",
            "total_size": 1_115_984,
        }),
        encoding="utf-8",
    )
    monkeypatch.setattr(settings, "ota_releases_path", str(path), raising=False)
    return path


def test_a_separate_manifest_key_signs_with_the_manifest_key(release_file: Path):
    release = json.loads(release_file.read_text(encoding="utf-8"))
    _register("CAUCE-100", manifest_key=MANIFEST_KEY)

    body = _manifest("CAUCE-100")

    # The whole point: NOT signed with the data key.
    assert body["hmac"] == _expected_signature(MANIFEST_KEY, release)
    assert body["hmac"] != _expected_signature(DEVICE_KEY, release)


def test_a_node_without_one_falls_back_and_still_gets_a_valid_signature(release_file: Path):
    release = json.loads(release_file.read_text(encoding="utf-8"))
    _register("CAUCE-101", manifest_key=None)

    body = _manifest("CAUCE-101")

    # Backwards compatibility, and what keeps a deployed node updating. The exposure is reduced
    # by setting the column, not by refusing the endpoint.
    assert body["hmac"] == _expected_signature(DEVICE_KEY, release)


def test_the_fallback_is_logged_rather_than_silent(caplog, release_file: Path):
    # A deployment that has been authorising firmware with its data secret is exactly the one
    # that needs to hear about it, and a warning nobody reads is not a warning.
    _register("CAUCE-102", manifest_key=None)
    with caplog.at_level("WARNING"):
        _manifest("CAUCE-102")
    assert any("manifest_key" in r.getMessage() for r in caplog.records), caplog.text


def test_a_separate_key_is_not_logged_as_a_fallback(caplog, release_file: Path):
    _register("CAUCE-103", manifest_key=MANIFEST_KEY)
    with caplog.at_level("WARNING"):
        _manifest("CAUCE-103")
    assert not any("manifest_key" in r.getMessage() for r in caplog.records), caplog.text


def test_two_nodes_with_different_manifest_keys_get_different_signatures(release_file: Path):
    # The signature is per-node, which is the point of signing per-node at all. Two nodes
    # receiving the same signature would mean the update channel was not node-specific.
    _register("CAUCE-104", manifest_key=MANIFEST_KEY)
    _register("CAUCE-105", manifest_key="another-manifest-secret-abcdef")
    assert _manifest("CAUCE-104")["hmac"] != _manifest("CAUCE-105")["hmac"]


def test_the_columns_exist_and_default_to_null():
    rows = db.query("PRAGMA table_info(nodes)")
    names = {r["name"] for r in rows}
    assert "manifest_key" in names
    assert "manifest_key_algorithm" in names
    # NULL, not an empty string: NULL distinguishes "never separated" from "deliberately
    # cleared", and the fallback keys off it.
    _register("CAUCE-106")
    row = db.query("SELECT manifest_key FROM nodes WHERE node_id=?", ("CAUCE-106",))
    assert row[0]["manifest_key"] is None


def test_a_manifest_key_shorter_than_the_data_key_minimum_is_refused():
    # The central enforces the same standard on this secret as on the data key, because the
    # whole point is that it is as carefully chosen as that one.
    response = _client.post("/v1/provision", json={
        "node_id": "CAUCE-107",
        "device_key": DEVICE_KEY,
        "manifest_key": "short",
    }, headers=ADMIN_HEADERS)
    assert response.status_code == 422, response.text
    assert response.json()["detail"] == "weak_manifest_key"


def test_provisioning_without_one_is_still_accepted():
    # Optional, because requiring it would strand every node provisioned before it existed.
    response = _client.post("/v1/provision", json={
        "node_id": "CAUCE-108",
        "device_key": DEVICE_KEY,
    }, headers=ADMIN_HEADERS)
    assert response.status_code == 200, response.text
    row = db.query("SELECT manifest_key FROM nodes WHERE node_id=?", ("CAUCE-108",))
    assert row[0]["manifest_key"] is None


def test_reprovisioning_a_node_does_not_wipe_a_manifest_key():
    # Rotation of the data key must not silently un-separate the update channel. That would be
    # the worst version of this feature: the column exists, the operator set it, and a routine
    # re-provision quietly removed the protection they had added.
    _register("CAUCE-109", manifest_key=MANIFEST_KEY)
    response = _client.post("/v1/provision", json={
        "node_id": "CAUCE-109",
        "device_key": "a-new-device-secret-abcdefghij",
    }, headers=ADMIN_HEADERS)
    assert response.status_code == 200, response.text
    row = db.query(
        "SELECT device_key, manifest_key FROM nodes WHERE node_id=?", ("CAUCE-109",))
    assert row[0]["device_key"] == "a-new-device-secret-abcdefghij"
    assert row[0]["manifest_key"] == MANIFEST_KEY
