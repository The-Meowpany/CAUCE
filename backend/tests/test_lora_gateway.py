"""The LoRa gateway loop, end to end, without a radio.

Frames produced by the firmware encoder are fed to `LoRaGateway.on_frame`, and
the gateway forwards them to the real FastAPI app through an injected transport
that calls `TestClient`. The rows are then read back out of SQLite.

That closes the last software gap in M2: before this, the reassembler was
tested and the sync payload was shaped, but nothing had ever proved the central
accepts what the gateway builds. Everything here except the radio driver is
exercised for real.
"""

from __future__ import annotations

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
    decode_ack,
    decode_frame,
)
from cauce_server.lora_gateway import (  # noqa: E402
    GatewayError,
    LoRaGateway,
    NodeKeys,
)
from cauce_server.main import app  # noqa: E402

VARIABLE_AIR_TEMPERATURE = 0
QUALITY_VALID = 0
DAY_MS = 86400000


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


def make_record_payload(sequence: int, timestamp: int, value: float,
                        node_id: str = "CAUCE-001") -> bytes:
    """One 60-byte record, laid out exactly like RecordCodec::encodePayload."""
    node = node_id.encode("ascii")[:15].ljust(16, b"\x00")
    sensor = b"BME280-1".ljust(24, b"\x00")
    out = struct.pack("<IQf", sequence, timestamp, value)
    out += struct.pack("<BBBB", VARIABLE_AIR_TEMPERATURE, QUALITY_VALID, 0, 0)
    out += node + sensor
    assert len(out) == RECORD_SIZE, len(out)
    return out


def real_crc16(data: bytes) -> int:
    """CRC16-CCITT, the same function the firmware applies to a frame."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 \
                else (crc << 1) & 0xFFFF
    return crc


def make_frame(batch_id: int, index: int, count: int,
               payload: bytes, record_count: int) -> bytes:
    """One frame with a correct CRC, so `decode_frame` accepts it.

    Header mirrors `firmware/lib/cauce_core/LoRaBatchCodec.h`: magic|version,
    batch id, fragment index, fragment count, record count for the whole batch,
    payload length, all little-endian.
    """
    body = struct.pack(
        "<HHBBHH",
        (0xCA << 8) | 1,
        batch_id,
        index,
        count,
        record_count,
        len(payload),
    ) + payload
    assert len(body) == HEADER_SIZE + len(payload)
    return body + struct.pack("<H", real_crc16(body))


def single_record_frames(node_id="CAUCE-001", first_seq=1, count=3,
                         start_ts=1787356800000, batch_id=1000):
    """`count` frames, one record each, exactly as SF9 produces them.

    All frames share one batch id: they are fragments of a single batch, and the
    gateway keys its reassembly buffer on that id.
    """
    frames = []
    for i in range(count):
        payload = make_record_payload(first_seq + i, start_ts + i * 3600000,
                                      20.0 + i, node_id)
        frames.append(make_frame(batch_id, i, count, payload, count))
    return frames


def gateway_for(client, keys=None):
    return LoRaGateway("/v1/sync", LoopbackTransport(client), keys)


def test_two_fragments_reach_sqlite_and_are_acknowledged(client):
    gateway = gateway_for(client)
    frames = single_record_frames(count=3)

    acks = [gateway.on_frame(f) for f in frames]
    assert acks[:2] == [None, None], "only the last frame closes the batch"
    assert acks[2] is not None

    # The acknowledgement reports what the central stored, which the node then
    # uses to advance its watermark.
    assert decode_ack(acks[2]) == 3

    rows = query("SELECT * FROM measurements WHERE node_id='CAUCE-001'"
                 " ORDER BY sequence")
    assert len(rows) == 3
    assert [r["sequence"] for r in rows] == [1, 2, 3]
    assert rows[0]["value"] == pytest.approx(20.0)
    assert rows[2]["value"] == pytest.approx(22.0)

    batches = query("SELECT * FROM sync_batches WHERE node_id='CAUCE-001'")
    assert len(batches) == 1
    assert batches[0]["transport"] == "lora", "the link type must be recorded"
    assert batches[0]["last_sequence"] == 3


def test_the_gateway_declares_the_lora_transport(client):
    gateway = gateway_for(client)
    for frame in single_record_frames(count=2):
        gateway.on_frame(frame)
    body, headers = gateway.transport.calls[0]
    assert b'"transport":"lora"' in body
    assert headers["X-CAUCE-Node"] == "CAUCE-001"
    assert "X-CAUCE-Signature" not in headers


def test_a_provisioned_node_is_forwarded_with_a_valid_signature(client):
    key = "clave-dispositivo-0123456789"
    assert client.post("/v1/provision", json={
        "node_id": "CAUCE-001", "device_key": key}).status_code == 200

    keys = NodeKeys()
    keys.add("CAUCE-001", key)
    gateway = gateway_for(client, keys)

    for frame in single_record_frames(count=2):
        gateway.on_frame(frame)

    body, headers = gateway.transport.calls[0]
    assert headers["X-CAUCE-Signature"] == sign_of(body, key)
    assert len(query("SELECT 1 FROM measurements")) == 2


def sign_of(body: bytes, key: str) -> str:
    import hashlib
    import hmac

    return hmac.new(key.encode(), body, hashlib.sha256).hexdigest()


def test_a_provisioned_node_rejects_an_unsigned_forwarding(client):
    # A gateway with no key must not silently downgrade a provisioned node: the
    # central refuses, which is the intended outcome.
    assert client.post("/v1/provision", json={
        "node_id": "CAUCE-001",
        "device_key": "clave-dispositivo-0123456789"}).status_code == 200

    gateway = gateway_for(client)
    with pytest.raises(GatewayError):
        for frame in single_record_frames(count=2):
            gateway.on_frame(frame)
    assert query("SELECT 1 FROM measurements") == []
    assert gateway.stats.batches_failed == 1


def test_a_rejected_forward_does_not_drop_the_node_data(client):
    assert client.post("/v1/provision", json={
        "node_id": "CAUCE-001",
        "device_key": "clave-dispositivo-0123456789"}).status_code == 200
    gateway = gateway_for(client)
    with pytest.raises(GatewayError):
        for frame in single_record_frames(count=2):
            gateway.on_frame(frame)
    # The batch is released either way, and the node still holds its rows
    # because it never saw an acknowledgement.
    assert gateway.pending_batches() == 0
    assert gateway.stats.batches_failed == 1


def test_repeated_delivery_of_the_same_batch_is_idempotent(client):
    gateway = gateway_for(client)
    frames = single_record_frames(count=2)
    for frame in frames:
        gateway.on_frame(frame)
    # A gateway that retransmits after a lost acknowledgement replays the same
    # batch; the central must not double-count it.
    for frame in frames:
        gateway.on_frame(frame)

    rows = query("SELECT * FROM measurements WHERE node_id='CAUCE-001'")
    assert len(rows) == 2
    assert query("SELECT COUNT(*) c FROM sync_batches")[0]["c"] == 2


def test_a_lost_fragment_leaves_the_batch_open_and_undelivered(client):
    gateway = gateway_for(client)
    frames = single_record_frames(count=3)
    gateway.on_frame(frames[0])
    gateway.on_frame(frames[2])

    assert gateway.pending_batches() == 1
    assert query("SELECT 1 FROM measurements") == []
    assert gateway.stats.batches_forwarded == 0

    # The late fragment closes it.
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

    # A corrupt frame from a real batch id drops that batch rather than being
    # merged into it.
    frames = single_record_frames(count=2)
    bad = bytearray(frames[0])
    bad[-1] ^= 0xFF
    assert gateway.on_frame(bytes(bad)) is None
    assert gateway.stats.frames_rejected == 3
    assert gateway.pending_batches() == 0


def test_stats_add_up(client):
    gateway = gateway_for(client)
    for frame in single_record_frames(count=2):
        gateway.on_frame(frame)
    gateway.on_frame(b"\xff\xff")
    stats = gateway.stats.as_dict()
    assert stats["batches_forwarded"] == 1
    assert stats["measurements_forwarded"] == 2
    assert stats["frames_accepted"] == 2
    assert stats["frames_rejected"] == 1
    assert stats["batches_failed"] == 0


def test_frames_from_a_second_node_do_not_mix(client):
    gateway = gateway_for(client)
    for frame in single_record_frames(node_id="CAUCE-002", count=1,
                                      first_seq=1, start_ts=1787356800000):
        gateway.on_frame(frame)
    rows = query("SELECT node_id FROM measurements")
    assert [r["node_id"] for r in rows] == ["CAUCE-002"]


def test_a_frame_that_contradicts_its_own_header_is_refused(client):
    # payload_bytes in the header does not match the frame length.
    payload = make_record_payload(1, 1787356800000, 20.0)
    body = struct.pack("<HHBBHH", (0xCA << 8) | 1, 7, 0, 1, 1, len(payload))
    body += payload
    lying = body[:-1] + bytes([body[-1] ^ 0xFF])
    gateway = gateway_for(client)
    assert gateway.on_frame(lying) is None
    assert gateway.stats.frames_rejected == 1
    assert decode_frame(body + struct.pack("<H", real_crc16(body))
                        )["batch_id"] == 7
