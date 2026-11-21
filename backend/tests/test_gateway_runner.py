"""The gateway's serial side: bridge framing, and the runner between port and relay.

The firmware bridge and this process speak the framing in
`firmware/lib/cauce_core/src/SerialBridge.cpp`. Both suites pin the same known-answer frame, so
the two implementations cannot drift apart without one of them failing.

The end-to-end case feeds frames into the runner exactly as the bridge would put them on the
wire, through the real relay and the real app, and checks that the acknowledgement written back
to the port is the one the node is waiting for.
"""

from __future__ import annotations

import struct

import pytest
from cauce_server import db, serial_bridge
from cauce_server.gateway_runner import GatewayRunner
from cauce_server.lora_frames import decode_ack, encode_ack
from cauce_server.lora_gateway import LoRaGateway
from test_lora_gateway import (  # noqa: F401  (client is a fixture)
    LoopbackTransport,
    client,
    provision,
    records,
    signed_frames_for,
)

# Same bytes as test_serial_bridge_matches_the_python_known_answer in the firmware suite.
KNOWN_ACK_FRAME = bytes.fromhex("c5a0020006a500000007a2d126")


@pytest.fixture(autouse=True)
def _leave_the_shared_database_clean():
    """Modules that run after this one and never reset (test_heat_views) must not inherit a
    node this file provisioned with a different key."""
    yield
    db.reset_for_tests()


class FakePort:
    def __init__(self) -> None:
        self.incoming = bytearray()
        self.written: list[bytes] = []

    def read(self, n: int) -> bytes:
        chunk = bytes(self.incoming[:n])
        del self.incoming[:n]
        return chunk

    def write(self, data: bytes) -> None:
        self.written.append(bytes(data))


def received(frame: bytes, rssi: int = -97) -> bytes:
    """What the bridge writes for a frame its radio heard."""
    return serial_bridge.encode(serial_bridge.RADIO_RECEIVED, struct.pack(">h", rssi) + frame)


def test_encoding_matches_the_known_answer_frame():
    assert serial_bridge.encode(serial_bridge.RADIO_TRANSMIT, encode_ack(7)) == KNOWN_ACK_FRAME


def test_the_decoder_survives_split_reads_garbage_and_a_corrupt_frame():
    decoder = serial_bridge.Decoder()
    good = serial_bridge.encode(serial_bridge.STATUS, b"CAUCE_GATEWAY region=AU915")
    corrupt = bytearray(good)
    corrupt[-1] ^= 0xFF
    stream = b"\x00\xc5boot noise" + bytes(corrupt) + good
    frames = []
    for i in range(0, len(stream), 3):
        frames.extend(decoder.feed(stream[i:i + 3]))
    assert [f.payload for f in frames] == [b"CAUCE_GATEWAY region=AU915"]
    assert decoder.errors == 1


def test_an_impossible_length_is_dropped_not_waited_for():
    decoder = serial_bridge.Decoder()
    bogus = serial_bridge.MAGIC + struct.pack(">BH", 1, 60000)
    good = serial_bridge.encode(serial_bridge.STATUS, b"ok")
    assert [f.payload for f in decoder.feed(bogus + good)] == [b"ok"]
    assert decoder.errors == 1


def test_a_batch_heard_by_the_bridge_is_stored_and_acknowledged_down_the_port(client):  # noqa: F811
    provision(client)
    gateway = LoRaGateway("/v1/sync", LoopbackTransport(client))
    port = FakePort()
    runner = GatewayRunner(gateway, port)

    for frame in signed_frames_for(records(3)):
        port.incoming.extend(received(frame))
    while runner.poll():
        pass

    assert runner.acks_sent == 1
    assert runner.last_rssi == -97
    decoded = serial_bridge.Decoder().feed(port.written[0])
    assert decoded[0].frame_type == serial_bridge.RADIO_TRANSMIT
    assert decode_ack(decoded[0].payload) == 3


def test_a_refused_batch_sends_no_acknowledgement(client):  # noqa: F811
    provision(client, node_id="CAUCE-077", key="a-different-provisioned-key-xyz")
    gateway = LoRaGateway("/v1/sync", LoopbackTransport(client))
    port = FakePort()
    runner = GatewayRunner(gateway, port)
    for frame in signed_frames_for(records(2, node_id="CAUCE-077")):
        port.incoming.extend(received(frame))
    while runner.poll():
        pass
    # The node keeps the batch and retries; acknowledging here would lose it.
    assert port.written == []
    assert runner.relay_failures == 1


def test_status_lines_are_kept_for_the_operator():
    port = FakePort()
    runner = GatewayRunner(LoRaGateway("/v1/sync", transport=None), port)
    port.incoming.extend(serial_bridge.encode(serial_bridge.STATUS, b"HEARTBEAT radio=up"))
    runner.poll()
    assert runner.last_status == "HEARTBEAT radio=up"
