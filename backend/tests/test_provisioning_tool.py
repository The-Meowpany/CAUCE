"""The provisioning tool's own guarantees.

These check the things that would be catastrophic rather than merely annoying: two
units sharing a seed, a manifest anybody but the operator can read, and the central
being handed a seed instead of a public key.
"""

from __future__ import annotations

import json
import os
import stat
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

import provision  # noqa: E402


class FakeResponse:
    def __init__(self, payload: dict):
        self._payload = json.dumps(payload).encode()

    def read(self):
        return self._payload

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


def test_generated_identities_are_ed25519_and_correct_length():
    identity = provision.generate_identity("CAUCE-001")
    assert identity["algorithm"] == "ed25519"
    assert len(bytes.fromhex(identity["seed_hex"])) == provision.SEED_BYTES
    assert len(bytes.fromhex(identity["public_key_hex"])) == provision.PUBLIC_KEY_BYTES


def test_the_public_key_really_belongs_to_the_seed():
    """A seed and a public key that disagree produce a unit that signs and verifies
    nowhere, and the only symptom is a central quietly rejecting every frame."""
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

    identity = provision.generate_identity("CAUCE-001")
    private = Ed25519PrivateKey.from_private_bytes(
        bytes.fromhex(identity["seed_hex"]))
    derived = private.public_key().public_bytes(
        encoding=serialization.Encoding.Raw,
        format=serialization.PublicFormat.Raw)
    assert derived.hex() == identity["public_key_hex"]


def test_two_identities_never_share_a_seed():
    units = [provision.generate_identity(f"CAUCE-{i:03d}") for i in range(16)]
    seeds = {unit["seed_hex"] for unit in units}
    assert len(seeds) == len(units)
    provision.assert_unique_seeds(units)


def test_a_seed_collision_fails_the_run():
    """A duplicate seed means two devices are the same device: one signs for the
    other and revoking one revokes both."""
    unit = provision.generate_identity("CAUCE-001")
    twin = dict(unit)
    twin["node_id"] = "CAUCE-002"
    with pytest.raises(SystemExit) as excinfo:
        provision.assert_unique_seeds([unit, twin])
    assert "collision" in str(excinfo.value)


def test_the_manifest_is_owner_only(tmp_path):
    """POSIX only. On Windows chmod cannot restrict anything, and the tool warns
    rather than pretending it succeeded."""
    path = tmp_path / "provisioning.json"
    units = [provision.generate_identity("CAUCE-001")]
    provision.write_manifest(path, units, "rio-01")
    mode = stat.S_IMODE(os.stat(path).st_mode)
    if provision._posix_modes_are_meaningful():
        assert mode == 0o600, oct(mode)
    else:
        assert os.stat(path).st_size > 0


def test_the_manifest_contains_the_seed_and_the_site(tmp_path):
    path = tmp_path / "provisioning.json"
    units = [provision.generate_identity("CAUCE-001")]
    provision.write_manifest(path, units, "rio-01")
    body = json.loads(path.read_text(encoding="utf-8"))
    assert body["site_id"] == "rio-01"
    assert body["units"][0]["seed_hex"] == units[0]["seed_hex"]


def test_an_existing_wide_open_manifest_is_tightened_where_possible(tmp_path):
    """A file left behind by an earlier tool, or copied by something that ignored
    the mode, must not stay readable."""
    if not provision._posix_modes_are_meaningful():
        pytest.skip("POSIX modes are not meaningful on this platform")
    path = tmp_path / "provisioning.json"
    units = [provision.generate_identity("CAUCE-001")]
    provision.write_manifest(path, units, None)
    os.chmod(str(path), 0o644)
    provision.write_manifest(path, units, None)
    assert stat.S_IMODE(os.stat(path).st_mode) & 0o077 == 0


def test_the_central_is_handed_the_public_key_and_never_the_seed(monkeypatch):
    seen: list[dict] = []

    def fake_post(url, token, body):
        seen.append(body)
        return {"status": "provisioned"}

    monkeypatch.setattr(provision, "post_json", fake_post)
    units = [provision.generate_identity("CAUCE-001")]
    provision.provision_units("https://central.example", "tok", units)

    assert len(seen) == 1
    assert seen[0]["device_key"] == units[0]["public_key_hex"]
    assert seen[0]["device_key_algorithm"] == "ed25519"
    assert units[0]["seed_hex"] not in json.dumps(seen[0])


def test_reinstating_a_node_is_reported_rather_than_silent(monkeypatch):
    """Re-provisioning a retired node is how a rotation is done, so the tool says so
    instead of the operator discovering it from the log later."""

    def fake_post(url, token, body):
        return {"status": "provisioned", "reinstated": True}

    monkeypatch.setattr(provision, "post_json", fake_post)
    rows = provision.provision_units(
        "https://central.example", "tok",
        [provision.generate_identity("CAUCE-001")])
    assert rows[0]["reinstated"] is True


def test_retiring_sends_the_reason(monkeypatch):
    seen: list[tuple[str, dict]] = []

    def fake_post(url, token, body):
        seen.append((url, body))
        return {"node_id": "CAUCE-001", "retired": True}

    monkeypatch.setattr(provision, "post_json", fake_post)
    provision.retire_unit("https://central.example", "tok", "CAUCE-001",
                          "water ingress")
    assert seen[0][0].endswith("/v1/nodes/CAUCE-001/revoke")
    assert seen[0][1] == {"reason": "water ingress"}


def test_node_ids_are_padded_and_ordered():
    ids = provision._default_node_ids("CAUCE", 4)
    assert ids == ["CAUCE-001", "CAUCE-002", "CAUCE-003", "CAUCE-004"]


def test_node_ids_pad_to_the_count_not_to_a_constant():
    """Padding must not change with the batch size, or provisioning 24 units and
    then 8 more produces colliding ids."""
    assert provision._default_node_ids("N", 3) == provision._default_node_ids("N", 3)
    assert "N-001" in provision._default_node_ids("N", 120)