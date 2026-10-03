"""The LoRa gateway loop, end to end, without a radio.

The node's frames are signed with its device key, relayed verbatim by a gateway
that never holds a secret, and verified frame by frame by the central. The rows
are then read back out of SQLite.

That split is the point: the relay is trusted to deliver bytes, not to vouch for
them, so it cannot forge a measurement even though it sees every one.
"""

from __future__ import annotations

import base64
import inspect
import json
import os
import struct

import pytest
from fastapi.testclient import TestClient

os.environ["CAUCE_DB_PATH"] = "./data/test_gateway.sqlite"

from cauce_server import db  # noqa: E402
from cauce_server.db import query  # noqa: E402
from cauce_server.lora_frames import (  # noqa: E402
    HEADER_SIZE,
    RECORD_SIZE,
    crc16,
    decode_ack,
    decode_frame,
    fragment_batch,
    make_record,
    sign_frame,
    verify_frame,
)
from cauce_server.lora_gateway import GatewayError, LoRaGateway  # noqa: E402
from cauce_server.main import app  # noqa: E402
from cauce_server.signing import ED25519  # noqa: E402

RADIO_BUDGET = 115
DEVICE_KEY = "clave-dispositivo-0123456789"
OTHER_KEY = "otra-clave-del-atacante-999"

# RFC 8032 section 7.1 TEST 2, used where a real key pair is needed.
ED25519_SEED = "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb"
ED25519_PUBLIC = "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c"
# RFC 8032 section 7.1 TEST 1: a different, equally valid key pair.
OTHER_ED25519_SEED = (
    "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60")


def ed25519_frames_for(batch, batch_id=1000, seed=ED25519_SEED):
    """Frames as an Ed25519 node would transmit them."""
    return [sign_frame(f, seed, ED25519)
            for f in fragment_batch(batch, batch_id=batch_id,
                                    budget=RADIO_BUDGET)]


@pytest.fixture()
def client():
    db.reset_for_tests()
    with TestClient(app) as c:
        yield c


class LoopbackTransport:
    """Posts through the real app, so the whole server path is exercised."""

    def __init__(self, client: TestClient, path: str = "/v1/sync"):
        self.client = client
        self.path = path
        self.calls: list[tuple[bytes, dict]] = []

    def post(self, url, body, headers):
        assert url.endswith(self.path), f"unexpected target {url}"
        self.calls.append((body, dict(headers)))
        response = self.client.post(self.path, content=body, headers=headers)
        return response.status_code, response.text


def records(count, node_id="CAUCE-001", first_seq=1,
            start_ts=1787356800000):
    return [
        make_record(first_seq + i, start_ts + i * 3600000, 20.0 + i, node_id)
        for i in range(count)
    ]


def signed_frames_for(batch, batch_id=1000, key=DEVICE_KEY,
                      budget=RADIO_BUDGET):
    """Frames exactly as a node would transmit them: framed, then signed."""
    return [sign_frame(f, key)
            for f in fragment_batch(batch, batch_id=batch_id, budget=budget)]


def gateway_for(client, node_id="CAUCE-001", algorithm=None):
    gateway = LoRaGateway("/v1/sync", LoopbackTransport(client))
    if algorithm is not None:
        gateway.set_node_algorithm(node_id, algorithm)
    return gateway


def provision(client, node_id="CAUCE-001", key=DEVICE_KEY,
              algorithm=None):
    body = {"node_id": node_id, "device_key": key}
    if algorithm is not None:
        body["device_key_algorithm"] = algorithm
    return client.post("/v1/provision", json=body)


def relay_body(frames, node_id="CAUCE-001", batch_id=1000, record_count=None):
    """The body a relay would POST, built without going through the gateway."""
    return {
        "protocol_version": 1,
        "node_id": node_id,
        "transport": "lora",
        "batch_id": batch_id,
        "record_count": (len(records(record_count or len(frames)))
                         if record_count is None else record_count),
        "frames": [base64.b64encode(f).decode("ascii") for f in frames],
    }


# --- the happy path -----------------------------------------------------

def test_a_signed_batch_reaches_sqlite_and_is_acknowledged(client):
    provision(client)
    gateway = gateway_for(client)
    frames = signed_frames_for(records(3))

    acks = [gateway.on_frame(frame) for frame in frames]
    assert acks[:2] == [None, None], "only the last frame closes the batch"
    assert acks[2] is not None
    # The node learns what the central stored, not what was sent.
    assert decode_ack(acks[2]) == 3

    rows = query("SELECT * FROM measurements WHERE node_id='CAUCE-001'"
                 " ORDER BY sequence")
    assert [r["sequence"] for r in rows] == [1, 2, 3]
    assert rows[0]["value"] == pytest.approx(20.0)
    assert rows[2]["value"] == pytest.approx(22.0)

    batches = query("SELECT * FROM sync_batches WHERE node_id='CAUCE-001'")
    assert len(batches) == 1
    assert batches[0]["transport"] == "lora"
    assert batches[0]["last_sequence"] == 3


def test_the_gateway_relays_frames_verbatim(client):
    provision(client)
    gateway = gateway_for(client)
    frames = signed_frames_for(records(2))
    for frame in frames:
        gateway.on_frame(frame)

    body, headers = gateway.transport.calls[0]
    sent = json.loads(body)
    # Frames go out exactly as they arrived: nothing rebuilt, re-signed or
    # reordered on the way through.
    assert sent["frames"] == [base64.b64encode(f).decode() for f in frames]
    assert "measurements" not in sent
    assert sent["transport"] == "lora"
    assert headers["X-CAUCE-Relay"] == "1"
    assert "X-CAUCE-Signature" not in headers, (
        "a relay must never sign on a node's behalf"
    )


def test_the_gateway_takes_no_device_keys_at_all():
    """The weak configuration cannot be written by accident.

    Verifying an HMAC needs the secret, and holding the secret means being able
    to forge one, so a gateway that verifies frames is a gateway that can invent
    them. There is deliberately no constructor argument for keys.
    """
    assert list(inspect.signature(LoRaGateway.__init__).parameters) == [
        "self", "sync_url", "transport"]


# --- the central is the only verifier -----------------------------------

def test_the_central_rejects_an_unsigned_batch(client):
    provision(client)
    unsigned = fragment_batch(records(2), batch_id=1000, budget=RADIO_BUDGET)
    response = client.post("/v1/sync", json=relay_body(unsigned, record_count=2))
    assert response.status_code == 401
    assert response.json()["detail"] == "invalid_frame_signature"
    assert query("SELECT 1 FROM measurements") == []


def test_the_central_rejects_a_frame_signed_with_another_key(client):
    provision(client)
    frames = signed_frames_for(records(2), key=OTHER_KEY)
    response = client.post("/v1/sync", json=relay_body(frames, record_count=2))
    assert response.status_code == 401


def test_tampering_with_a_relayed_frame_is_caught_by_the_central(client):
    """The gateway is not trusted, so the server has to be the one checking."""
    provision(client)
    frames = signed_frames_for(records(2))
    tampered = bytearray(frames[0])
    tampered[HEADER_SIZE + 20] ^= 0xFF  # a value byte inside the record
    tampered = bytes(tampered)

    response = client.post("/v1/sync", json=relay_body(
        [tampered] + frames[1:], record_count=2))
    assert response.status_code == 401
    assert response.json()["detail"] == "invalid_frame_signature"
    assert query("SELECT 1 FROM measurements") == []


def test_an_unprovisioned_node_cannot_use_the_relay_path(client):
    frames = signed_frames_for(records(2))
    response = client.post("/v1/sync", json=relay_body(frames, record_count=2))
    assert response.status_code == 401
    assert response.json()["detail"] == "node_not_provisioned"


def test_another_nodes_records_cannot_be_relayed_under_this_name(client):
    provision(client)
    frames = signed_frames_for(records(2, node_id="CAUCE-999"))
    response = client.post("/v1/sync", json=relay_body(frames, record_count=2))
    assert response.status_code == 401
    assert response.json()["detail"] == "node_id_mismatch"


def test_an_incomplete_relayed_batch_is_refused(client):
    provision(client)
    frames = signed_frames_for(records(3))
    response = client.post("/v1/sync", json=relay_body(frames[:2], record_count=3))
    assert response.status_code == 422
    assert query("SELECT 1 FROM measurements") == []


def test_a_lying_record_count_is_refused(client):
    provision(client)
    frames = signed_frames_for(records(2))
    response = client.post("/v1/sync", json=relay_body(frames, record_count=99))
    assert response.status_code == 422


def test_a_malformed_relay_request_is_refused(client):
    provision(client)
    for body, expected in (
        ({"node_id": "CAUCE-001", "frames": []}, 422),
        ({"frames": ["AAAA"]}, 422),
        ({"node_id": "CAUCE-001", "frames": ["not base64!!"]}, 422),
        ({"node_id": "CAUCE-001", "frames": [123]}, 422),
    ):
        body.setdefault("protocol_version", 1)
        response = client.post("/v1/sync", json=body)
        assert response.status_code == expected, body


def test_too_many_frames_are_refused(client):
    # The cap is on frames per request, not on records, so it is driven by the
    # spreading factor: SF9 puts one record in each frame.
    provision(client)
    frames = signed_frames_for(records(40), budget=RADIO_BUDGET,
                               key=OTHER_KEY)
    assert len(frames) > 32
    assert client.post("/v1/sync", json=relay_body(frames)).status_code == 422


# --- delivery semantics -------------------------------------------------

def test_repeated_delivery_of_the_same_batch_is_idempotent(client):
    provision(client)
    gateway = gateway_for(client)
    frames = signed_frames_for(records(2))
    for frame in frames:
        gateway.on_frame(frame)
    # A relay that retransmits after a lost acknowledgement replays the batch;
    # the central must not double-count it.
    for frame in frames:
        gateway.on_frame(frame)

    assert len(query("SELECT 1 FROM measurements")) == 2
    assert query("SELECT COUNT(*) c FROM sync_batches")[0]["c"] == 2


def test_a_lost_fragment_leaves_the_batch_open_and_undelivered(client):
    provision(client)
    gateway = gateway_for(client)
    frames = signed_frames_for(records(3))
    gateway.on_frame(frames[0])
    gateway.on_frame(frames[2])

    assert gateway.pending_batches() == 1
    assert query("SELECT 1 FROM measurements") == []
    assert gateway.stats.batches_forwarded == 0

    ack = gateway.on_frame(frames[1])
    assert ack is not None
    assert decode_ack(ack) == 3
    assert len(query("SELECT 1 FROM measurements")) == 3
    assert gateway.pending_batches() == 0


def test_noise_is_counted_not_raised(client):
    gateway = gateway_for(client)
    assert gateway.on_frame(b"\x00\x01\x02") is None
    assert gateway.on_frame(b"") is None
    assert gateway.stats.frames_rejected == 2
    assert gateway.pending_batches() == 0


def test_stats_add_up(client):
    provision(client)
    gateway = gateway_for(client)
    for frame in signed_frames_for(records(2)):
        gateway.on_frame(frame)
    gateway.on_frame(b"\xff\xff")
    stats = gateway.stats.as_dict()
    assert stats["batches_forwarded"] == 1
    assert stats["measurements_forwarded"] == 2
    assert stats["frames_accepted"] == 2
    assert stats["frames_rejected"] == 1
    assert stats["batches_failed"] == 0
    assert stats["frames_relayed"] == 2


def test_a_rejected_forward_does_not_leak_the_batch(client):
    provision(client)
    gateway = gateway_for(client)
    # The central refuses because the node id in the records is foreign.
    frames = signed_frames_for(records(2, node_id="CAUCE-999"))
    with pytest.raises(GatewayError):
        for frame in frames:
            gateway.on_frame(frame)
    assert gateway.pending_batches() == 0, "a failed batch must not leak"


def test_frames_from_a_second_node_do_not_mix(client):
    provision(client, node_id="CAUCE-002")
    gateway = gateway_for(client)
    for frame in signed_frames_for(records(1, node_id="CAUCE-002"),
                                   batch_id=2000):
        gateway.on_frame(frame)
    rows = query("SELECT node_id FROM measurements")
    assert [r["node_id"] for r in rows] == ["CAUCE-002"]



# --- Ed25519 nodes -------------------------------------------------------

def test_an_ed25519_node_reaches_sqlite_and_is_acknowledged(client):
    response = provision(client, key=ED25519_PUBLIC, algorithm=ED25519)
    assert response.status_code == 200
    assert response.json()["device_key_algorithm"] == ED25519
    assert response.json()["signature_bytes"] == 64

    gateway = gateway_for(client, algorithm=ED25519)
    frames = ed25519_frames_for(records(3))
    acks = [gateway.on_frame(frame) for frame in frames]
    assert decode_ack(acks[-1]) == 3
    assert [r["sequence"] for r in query(
        "SELECT sequence FROM measurements ORDER BY sequence")] == [1, 2, 3]


def test_the_central_rejects_an_ed25519_frame_signed_with_another_key(client):
    provision(client, key=ED25519_PUBLIC, algorithm=ED25519)
    frames = ed25519_frames_for(records(2), seed=OTHER_ED25519_SEED)
    response = client.post("/v1/sync", json=relay_body(frames, record_count=2))
    assert response.status_code == 401
    assert response.json()["detail"] == "invalid_frame_signature"
    assert query("SELECT 1 FROM measurements") == []


def test_an_hmac_node_cannot_be_downgraded_onto_the_ed25519_path(client):
    """Relabelling a frame must not pick the verification the attacker prefers.

    The algorithm comes from provisioning, so an HMAC frame presented to an
    Ed25519 node is checked as Ed25519 and fails. Without that, an attacker
    could send the cheaper construction and let the server believe it.
    """
    provision(client, key=ED25519_PUBLIC, algorithm=ED25519)
    frames = signed_frames_for(records(2))
    plain = fragment_batch(records(1), batch_id=1000,
                      budget=RADIO_BUDGET)[0]
    assert len(frames[0]) - len(plain) == 32
    assert client.post("/v1/sync", json=relay_body(
        frames, record_count=2)).status_code == 401


def test_an_ed25519_node_cannot_be_downgraded_onto_the_hmac_path(client):
    provision(client, key=DEVICE_KEY)
    frames = ed25519_frames_for(records(2))
    assert client.post("/v1/sync", json=relay_body(
        frames, record_count=2)).status_code == 401
    assert query("SELECT 1 FROM measurements") == []


def test_tampering_with_an_ed25519_frame_is_caught_by_the_central(client):
    provision(client, key=ED25519_PUBLIC, algorithm=ED25519)
    frames = ed25519_frames_for(records(2))
    tampered = bytearray(frames[0])
    tampered[HEADER_SIZE + 20] ^= 0xFF
    response = client.post("/v1/sync", json=relay_body(
        [bytes(tampered)] + frames[1:], record_count=2))
    assert response.status_code == 401
    assert response.json()["detail"] == "invalid_frame_signature"


def test_provisioning_refuses_an_ed25519_key_of_the_wrong_length(client):
    for bad in ("", "abcd", ED25519_PUBLIC[:-2], ED25519_PUBLIC + "00"):
        response = provision(client, key=bad, algorithm=ED25519)
        assert response.status_code == 422, bad
        assert response.json()["detail"] == "invalid_ed25519_public_key"


def test_provisioning_refuses_an_unknown_algorithm(client):
    response = client.post("/v1/provision", json={
        "node_id": "CAUCE-001", "device_key": DEVICE_KEY,
        "device_key_algorithm": "rot13"})
    assert response.status_code == 422
    assert response.json()["detail"] == "unknown_signature_algorithm"


def test_provisioning_defaults_to_hmac_and_reports_it(client):
    body = provision(client).json()
    assert body["device_key_algorithm"] == "hmac-sha256"
    assert body["signature_bytes"] == 32


def test_reprovisioning_switches_the_algorithm(client):
    provision(client)
    assert client.post("/v1/sync", json=relay_body(
        signed_frames_for(records(2)), record_count=2)).status_code == 200

    provision(client, key=ED25519_PUBLIC, algorithm=ED25519)
    # The old HMAC frames stop verifying the moment the algorithm changes,
    # rather than continuing to work against the new key.
    assert client.post("/v1/sync", json=relay_body(
        signed_frames_for(records(2, first_seq=3)),
        record_count=2)).status_code == 401
    assert client.post("/v1/sync", json=relay_body(
        ed25519_frames_for(records(2)), record_count=2)).status_code == 200

# --- the frame format the replay harness depends on ---------------------

def test_the_replay_harness_matches_the_firmware_layout(client):
    frames = signed_frames_for(records(1), batch_id=4242)
    body = frames[0][:-32]
    decoded = decode_frame(body)
    assert decoded["batch_id"] == 4242
    assert decoded["record_count"] == 1
    assert decoded["fragment_count"] == 1
    assert len(body) == HEADER_SIZE + RECORD_SIZE + 2
    assert struct.unpack("<H", body[-2:])[0] == crc16(body[:-2])
    assert verify_frame(frames[0], DEVICE_KEY)
    assert not verify_frame(frames[0], OTHER_KEY)
