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
    encode_frame,
    encode_record,
    fragment_batch,
    make_record,
    max_payload_bytes,
    records_per_frame,
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


def test_python_encoder_reproduces_the_firmware_vectors():
    """The reverse of the pinned check, now that an encoder exists.

    `FRAME0`/`FRAME1` were produced by the C++ `LoRaBatchEncoder`. If the Python
    encoder rebuilds them byte for byte, then a gateway written in either
    language reads the same node, and the replay harness in
    `test_lora_gateway.py` can stand in for real firmware.
    """
    recipe = [
        make_record(1, 1787356800000, 20.0),
        make_record(2, 1787356860000, 21.0),
    ]
    frames = fragment_batch(recipe, batch_id=4242, budget=115)
    assert len(frames) == 2
    assert frames[0] == FRAME0
    assert frames[1] == FRAME1


def test_record_round_trips_through_the_encoder():
    original = make_record(
        sequence=99, timestamp_utc_ms=1787356800000, value=-3.25,
        node_id="CAUCE-009", sensor_id="BME280-2",
        variable="relative_humidity", quality="SUSPECT",
        reason_bits=5, time_uncertain=True)
    restored = decode_record(encode_record(original))
    assert restored == original


def test_identifier_padding_is_bounded_not_overflowing():
    # A longer id is truncated to the field width rather than shifting the rest
    # of the record, which would corrupt every later field.
    long_id = make_record(1, 1, 1.0, node_id="X" * 64, sensor_id="Y" * 64)
    payload = encode_record(long_id)
    assert len(payload) == RECORD_SIZE
    restored = decode_record(payload)
    assert restored.node_id == "X" * 15
    assert restored.sensor_id == "Y" * 23


def test_an_unrepresentable_variable_is_carried_as_unknown():
    payload = encode_record(make_record(1, 1, 1.0, variable="photon_flux"))
    assert decode_record(payload).variable == "unknown"


def test_records_per_frame_matches_the_firmware_table():
    assert records_per_frame(222) == 3
    assert records_per_frame(115) == 1
    assert records_per_frame(71) == 0
    assert records_per_frame(51) == 0


def test_fragmentation_refuses_an_impossible_budget():
    records = [make_record(1, 1, 1.0)]
    with pytest.raises(DecodeError):
        fragment_batch(records, batch_id=1, budget=51)
    with pytest.raises(DecodeError):
        fragment_batch([], batch_id=1, budget=115)


def test_frames_reassemble_to_the_records_that_went_in():
    records = [make_record(i + 1, 1787356800000 + i * 3600000, 20.0 + i)
               for i in range(7)]
    for budget, expected in ((115, 7), (222, 3)):
        frames = fragment_batch(records, batch_id=7, budget=budget)
        assert len(frames) == expected
        reassembler = Reassembler(batch_id=7)
        for frame in frames:
            assert reassembler.add(frame) is not None or reassembler.complete
        assert reassembler.complete
        # Out-of-order arrival must still produce the original sequence.
        rebuilt = reassembler.records()
        assert [r.sequence for r in rebuilt] == [r.sequence for r in records]


def test_encode_frame_matches_a_hand_built_frame():
    record = make_record(1, 1787356800000, 20.0)
    frame = encode_frame(5, 0, 1, [record])
    decoded = decode_frame(frame)
    assert decoded["batch_id"] == 5
    assert decoded["fragment_count"] == 1
    assert decoded["records"][0] == record
    assert struct.unpack("<H", frame[-2:])[0] == crc16(frame[:-2])


def test_acknowledgement_round_trips_and_validates():
    assert decode_ack(encode_ack(0xDEADBEEF)) == 0xDEADBEEF
    assert decode_ack(encode_ack(0)) == 0
    broken = bytearray(encode_ack(7))
    broken[5] ^= 0x01
    with pytest.raises(DecodeError):
        decode_ack(bytes(broken))
    with pytest.raises(DecodeError):
        decode_ack(b"\x00\x00\x00")


def test_acknowledgement_matches_the_vectors_the_firmware_parses():
    """Cross-language check for the ack, not just the data frames.

    `test_lora_batch.py` pins the uplink frames; this pins the downlink
    acknowledgement. Without it the two implementations could disagree on byte
    order or checksum convention and every test on both sides would still pass,
    because each would only ever exercise its own encoder.
    """
    vectors = {
        0x00000000: "a500000000a5",
        0x00000001: "a500000001a4",
        0x00001092: "a50000109227",
        0xFFFFFFFF: "a5ffffffffa5",
        0xDEADBEEF: "a5deadbeef87",
    }
    for sequence, expected in vectors.items():
        produced = encode_ack(sequence)
        assert produced.hex() == expected, (
            f"ack for {sequence:#010x} drifted from the firmware vector"
        )
        assert decode_ack(produced) == sequence


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
