"""Firmware releases signed with an offline Ed25519 key.

The central forwards the signature and cannot produce one. The known-answer vector here is the
one firmware/test/test_ota.cpp verifies with the node's own Ed25519, so the release tool and the
firmware cannot drift apart on the canonical string without one suite failing.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest
from test_api import client  # noqa: F401  (fixture)

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

import sign_release  # noqa: E402

RFC8032_TEST1_SEED = bytes.fromhex(
    "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60")
VECTOR = {"version": "0.2.0", "sha256": "ab" * 32,
          "url": "http://central.local/fw/cauce-0.2.0.bin", "total_size": 1234567}
VECTOR_SIGNATURE = (
    "315204c4d4d30f3c7154293b76c52a59e569fdfca8a35156899fe8426e4ccc72"
    "fca3a4b2b003bedbfbe55457f57fb02a9c55e84e377d12b6627b89e7ff516504")


def test_the_release_tool_matches_the_firmware_known_answer():
    assert sign_release.public_key_hex(RFC8032_TEST1_SEED) == (
        "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a")
    assert sign_release.sign(dict(VECTOR), RFC8032_TEST1_SEED) == VECTOR_SIGNATURE


def test_keygen_and_sign_round_trip(tmp_path):
    key_path = tmp_path / "release-key.json"
    release_path = tmp_path / "latest.json"
    release_path.write_text(json.dumps(VECTOR), encoding="utf-8")
    assert sign_release.main(["keygen", "--out", str(key_path)]) == 0
    assert sign_release.main(["sign", "--key", str(key_path), "--release",
                              str(release_path)]) == 0
    signed = json.loads(release_path.read_text(encoding="utf-8"))
    record = json.loads(key_path.read_text(encoding="utf-8"))

    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey

    Ed25519PublicKey.from_public_bytes(bytes.fromhex(record["public_key_hex"])).verify(
        bytes.fromhex(signed["ed25519_signature"]),
        sign_release.canonical(signed).encode("utf-8"))
    # A second keygen onto the same file would silently replace the key every node trusts.
    with pytest.raises(SystemExit):
        sign_release.main(["keygen", "--out", str(key_path)])


def test_the_manifest_forwards_a_well_formed_signature_and_drops_anything_else(
        client, monkeypatch, tmp_path):  # noqa: F811
    from cauce_server.config import settings

    path = tmp_path / "releases.json"
    monkeypatch.setattr(settings, "ota_releases_path", str(path))

    path.write_text(json.dumps({**VECTOR, "ed25519_signature": VECTOR_SIGNATURE}),
                    encoding="utf-8")
    body = client.get("/v1/ota/manifest").json()
    assert body["ed25519_signature"] == VECTOR_SIGNATURE

    path.write_text(json.dumps({**VECTOR, "ed25519_signature": "not-a-signature"}),
                    encoding="utf-8")
    assert client.get("/v1/ota/manifest").json()["ed25519_signature"] is None
