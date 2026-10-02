"""Known-answer tests for the LoRa wire format.

`FRAME0` and `FRAME1` below were produced by the firmware encoder
(`LoRaBatchEncoder`) and are pinned here so a change on either side shows up as
a failing test rather than as a gateway that silently stops accepting nodes.

Regenerate them with the same recipe: two records, sequences 1 and 2,
timestamps 1787356800000 and 1787356860000, values 20.0 and 21.0,
air_temperature/VALID, node CAUCE-001, sensor BME280-1, batch id 4242, one
record per frame at the SF9 budget of 115 bytes.
"""

import struct

import pytest
from cauce_server.lora_frames import (
    HEADER_SIZE,
    OVERHEAD,
    RECORD_SIZE,
    DecodeError,
    Reassembler,
    batch_to_sync_payload,
    crc16,
    decode_ack,
    decode_frame,
    decode_record,
    encode_ack,
    max_payload_bytes,
)

FRAME0 = bytes.fromhex(
    "01ca9210000202003c00010000000064c426a00100000000a0410000000043415543452d30303100000000000000424d453238302d3100000000000000000000000000000000d616"
)
FRAME1 = bytes.fromhex(
    "01ca9210010202003c0002000000604ec526a00100000000a8410000000043415543452d30303100000000000000424d453238302d31000000000000000000000000000000006d43"
)

assert len(FRAME0) == 72, "pinned vector must be exactly one frame"
assert len(FRAME1) == 72, "pinned vector must be exactly one frame"


def test_frames_from_the_firmware_decode_here():
    """The cross-language contract: C++ bytes in, Python records out."""
    first = decode_frame(FRAME0)
    assert first["batch_id"] == 4242
    assert first["fragment_index"] == 0
    assert first["fragment_count"] == 2
    assert first["record_count"] == 2
    assert len(first["records"]) == 1

    record = first["records"][0]
    assert record.sequence == 1
    assert record.timestamp_utc_ms == 1787356800000
    assert record.value == pytest.approx(20.0)
    assert record.variable == "air_temperature"
    assert record.quality == "VALID"
    assert record.node_id == "CAUCE-001"
    assert record.sensor_id == "BME280-1"
    assert record.time_uncertain is False

    second = decode_frame(FRAME1)["records"][0]
    assert second.sequence == 2
    assert second.timestamp_utc_ms == 1787356860000
    assert second.value == pytest.approx(21.0)


def test_frame_geometry_is_what_the_docstring_claims():
    assert OVERHEAD == HEADER_SIZE + 2 == 12
    assert RECORD_SIZE == 60
    assert len(FRAME0) == OVERHEAD + RECORD_SIZE == 72
    assert len(FRAME1) == OVERHEAD + RECORD_SIZE == 72


def test_two_fragments_reassemble_into_a_sync_payload():
    reassembler = Reassembler(batch_id=4242)
    assert reassembler.add(FRAME1) is False  # out of order on purpose
    assert reassembler.complete is False
    assert reassembler.add(FRAME0) is True
    assert reassembler.complete

    records = reassembler.records()
    assert [r.sequence for r in records] == [1, 2]

    payload = batch_to_sync_payload(records)
    assert payload["node_id"] == "CAUCE-001"
    assert payload["protocol_version"] == 1
    assert len(payload["measurements"]) == 2
    assert payload["measurements"][0]["sequence"] == 1
    assert payload["measurements"][1]["value"] == pytest.approx(21.0)
    # The payload must be directly postable to the central.
    for measurement in payload["measurements"]:
        assert measurement["quality"] in {"VALID", "SUSPECT", "INVALID"}


def test_incomplete_batch_yields_nothing():
    reassembler = Reassembler(batch_id=4242)
    reassembler.add(FRAME0)
    with pytest.raises(DecodeError):
        reassembler.records()


def test_corrupt_frames_are_refused():
    with pytest.raises(DecodeError):
        decode_frame(bytes([0x00]) + FRAME0[1:])
    with pytest.raises(DecodeError):
        decode_frame(FRAME0[:-1] + bytes([FRAME0[-1] ^ 0xFF]))
    with pytest.raises(DecodeError):
        decode_frame(FRAME0[:40])
    with pytest.raises(DecodeError):
        decode_frame(b"")


def test_a_fragment_from_another_batch_is_refused():
    reassembler = Reassembler(batch_id=999)
    with pytest.raises(DecodeError):
        reassembler.add(FRAME0)


def test_a_duplicate_fragment_is_refused():
    reassembler = Reassembler(batch_id=4242)
    reassembler.add(FRAME0)
    with pytest.raises(DecodeError):
        reassembler.add(FRAME0)


def test_a_bad_frame_does_not_poison_the_batch():
    reassembler = Reassembler(batch_id=4242)
    with pytest.raises(DecodeError):
        reassembler.add(bytes([0x00]) + FRAME0[1:])
    assert reassembler.complete is False
    # The surviving fragment is still accepted, and the batch only completes
    # once the real missing one turns up.
    assert reassembler.add(FRAME1) is False
    assert reassembler.complete is False
    assert reassembler.add(FRAME0) is True
    assert len(reassembler.records()) == 2


def test_acknowledgement_round_trips_and_validates():
    assert decode_ack(encode_ack(0xDEADBEEF)) == 0xDEADBEEF
    assert decode_ack(encode_ack(0)) == 0
    broken = bytearray(encode_ack(7))
    broken[5] ^= 0x01
    with pytest.raises(DecodeError):
        decode_ack(bytes(broken))
    with pytest.raises(DecodeError):
        decode_ack(b"\x00\x00\x00")


def test_spreading_factor_budgets_match_the_firmware():
    assert max_payload_bytes(7) == 222
    assert max_payload_bytes(8) == 222
    assert max_payload_bytes(9) == 115
    # SF10 is 51 bytes: below one record, so it must be refused rather than
    # truncating a measurement in half.
    for factor in (10, 11, 12):
        with pytest.raises(DecodeError):
            max_payload_bytes(factor)


def test_record_rejects_a_wrong_sized_payload():
    # One byte short of a record: a truncated measurement must not be accepted
    # as a valid one with a missing field.
    with pytest.raises(DecodeError):
        decode_record(FRAME0[HEADER_SIZE : HEADER_SIZE + RECORD_SIZE - 1])
    with pytest.raises(DecodeError):
        decode_record(FRAME0[HEADER_SIZE : HEADER_SIZE + RECORD_SIZE + 1])


def test_crc_matches_the_frame_it_protects():
    assert crc16(FRAME0[:-2]) == (FRAME0[-2] | (FRAME0[-1] << 8))
    assert struct.unpack_from("<HBBHH", FRAME0, 2) == (4242, 0, 2, 2, 60)
