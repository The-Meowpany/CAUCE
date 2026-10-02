"""Reference decoder for the LoRa batch wire format.

This is the other half of `firmware/lib/cauce_core/LoRaBatchCodec.h`: a node
packs a batch into binary frames, a gateway receives them and has to turn them
back into something the central accepts. Writing the decoder here rather than
only on the node is what makes the format testable without a radio, and it
doubles as the gateway implementation for `ROADMAP.md` M2.

Layout, all integers little-endian, mirroring `RecordCodec`:

    frame   0..1   magic 0xCA | version 1
            2..3   batch_id
            4      fragment_index
            5      fragment_count
            6..7   record_count (whole batch, not this fragment)
            8..9   payload_bytes
            10..   record payloads, 60 bytes each
            last 2 CRC16-CCITT over everything before it

    payload 0..3   sequence
            4..11  timestamp_utc_ms
            12..15 float32 value
            16     variable
            17     quality
            18     reason_bits
            19     time_uncertain
            20..35 node_id, 16 bytes, NUL padded
            36..59 sensor_id, 24 bytes, NUL padded

The duplication with the C++ encoder is deliberate: a gateway has to be able to
be wrong independently, and a shared implementation would hide that.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field

MAGIC = 0xCA
VERSION = 1
HEADER_SIZE = 10
TRAILER_SIZE = 2
RECORD_SIZE = 60
OVERHEAD = HEADER_SIZE + TRAILER_SIZE

MAX_FRAGMENTS = 16
MAX_RECORDS = 64

VARIABLES = {
    0: "air_temperature",
    1: "relative_humidity",
    2: "pressure",
    3: "illuminance",
    4: "battery_voltage",
}
QUALITIES = {
    0: "VALID",
    1: "CALIBRATED",
    2: "UNCALIBRATED",
    3: "ESTIMATED",
    4: "SUSPECT",
    5: "INVALID",
    6: "MISSING",
}


class DecodeError(ValueError):
    """A frame that must not be merged into a batch."""


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


@dataclass
class Record:
    sequence: int
    timestamp_utc_ms: int
    value: float
    variable: str
    quality: str
    reason_bits: int
    time_uncertain: bool
    node_id: str
    sensor_id: str

    def as_measurement(self) -> dict:
        """The shape `POST /v1/sync` expects."""
        return {
            "node_id": self.node_id,
            "sensor_id": self.sensor_id,
            "sequence": self.sequence,
            "timestamp_utc_ms": self.timestamp_utc_ms,
            "variable": self.variable,
            "value": self.value,
            "quality": self.quality,
            "reason_bits": self.reason_bits,
            "time_uncertain": self.time_uncertain,
        }


def _cstr(raw: bytes) -> str:
    return raw.split(b"\x00", 1)[0].decode("utf-8", errors="replace")


def decode_record(payload: bytes) -> Record:
    if len(payload) != RECORD_SIZE:
        raise DecodeError(f"record payload is {len(payload)} bytes, expected {RECORD_SIZE}")
    (sequence, timestamp, value) = struct.unpack_from("<IQf", payload, 0)
    variable, quality, reason_bits, time_uncertain = struct.unpack_from("<BBBB", payload, 16)
    node_id = _cstr(payload[20:36])
    sensor_id = _cstr(payload[36:60])
    return Record(
        sequence=sequence,
        timestamp_utc_ms=timestamp,
        value=value,
        variable=VARIABLES.get(variable, "unknown"),
        quality=QUALITIES.get(quality, "UNKNOWN"),
        reason_bits=reason_bits,
        time_uncertain=time_uncertain != 0,
        node_id=node_id,
        sensor_id=sensor_id,
    )


def decode_frame(frame: bytes) -> dict:
    """Decodes one frame into its header and record payloads.

    Raises DecodeError rather than returning a partial result: a frame the
    receiver cannot trust must not be merged, because merging a bad fragment
    silently corrupts the whole batch.
    """
    if len(frame) < OVERHEAD + RECORD_SIZE:
        raise DecodeError(f"frame too short: {len(frame)} bytes")

    magic_version = frame[0] | (frame[1] << 8)
    if (magic_version >> 8) != MAGIC:
        raise DecodeError(f"bad magic: {magic_version >> 8:#04x}")
    if magic_version & 0xFF != VERSION:
        raise DecodeError(f"bad version: {magic_version & 0xFF}")

    batch_id, fragment_index, fragment_count, record_count, payload_bytes = (
        struct.unpack_from("<HBBHH", frame, 2)
    )
    if payload_bytes == 0 or payload_bytes % RECORD_SIZE != 0:
        raise DecodeError(f"payload_bytes {payload_bytes} is not a record multiple")
    if len(frame) != HEADER_SIZE + payload_bytes + TRAILER_SIZE:
        raise DecodeError(
            f"length {len(frame)} contradicts payload_bytes {payload_bytes}"
        )
    if fragment_count == 0 or fragment_index >= fragment_count:
        raise DecodeError(f"fragment {fragment_index} of {fragment_count}")
    if fragment_count > MAX_FRAGMENTS:
        raise DecodeError(f"too many fragments: {fragment_count}")
    if record_count == 0 or record_count > MAX_RECORDS:
        raise DecodeError(f"record_count out of range: {record_count}")

    trailer = frame[-2] | (frame[-1] << 8)
    if crc16(frame[:-TRAILER_SIZE]) != trailer:
        raise DecodeError("CRC mismatch")

    payload = frame[HEADER_SIZE : HEADER_SIZE + payload_bytes]
    return {
        "batch_id": batch_id,
        "fragment_index": fragment_index,
        "fragment_count": fragment_count,
        "record_count": record_count,
        "records": [decode_record(payload[i : i + RECORD_SIZE])
                    for i in range(0, payload_bytes, RECORD_SIZE)],
    }


@dataclass
class Reassembler:
    """Collects fragments of one batch and yields the records when complete."""

    batch_id: int
    fragments: dict = field(default_factory=dict)
    fragment_count: int = 0
    record_count: int = 0

    def add(self, frame: bytes) -> bool:
        """Adds a frame. Returns True once the batch is complete."""
        decoded = decode_frame(frame)
        if decoded["batch_id"] != self.batch_id:
            raise DecodeError(
                f"fragment for batch {decoded['batch_id']}, expected {self.batch_id}"
            )
        if self.fragment_count and decoded["fragment_count"] != self.fragment_count:
            raise DecodeError("fragment count disagrees with the batch")
        if decoded["fragment_index"] in self.fragments:
            raise DecodeError(
                f"duplicate fragment {decoded['fragment_index']}"
            )
        self.fragment_count = decoded["fragment_count"]
        self.record_count = decoded["record_count"]
        self.fragments[decoded["fragment_index"]] = decoded["records"]
        return len(self.fragments) == self.fragment_count

    @property
    def complete(self) -> bool:
        return bool(self.fragment_count) and len(self.fragments) == self.fragment_count

    def records(self) -> list[Record]:
        if not self.complete:
            raise DecodeError("batch is incomplete")
        out: list[Record] = []
        for index in range(self.fragment_count):
            out.extend(self.fragments[index])
        if len(out) != self.record_count:
            raise DecodeError(
                f"reassembled {len(out)} records, header promised {self.record_count}"
            )
        return out


def batch_to_sync_payload(records: list[Record]) -> dict:
    """Turns reassembled records into a `/v1/sync` body."""
    node_ids = {r.node_id for r in records}
    if len(node_ids) != 1:
        raise DecodeError(f"a batch must come from one node, got {sorted(node_ids)}")
    return {
        "protocol_version": 1,
        "node_id": node_ids.pop(),
        "measurements": [r.as_measurement() for r in records],
    }


def decode_ack(frame: bytes) -> int:
    """Decodes the gateway acknowledgement frame."""
    if len(frame) != 6 or frame[0] != 0xA5:
        raise DecodeError("not an acknowledgement frame")
    expected = frame[0] ^ frame[1] ^ frame[2] ^ frame[3] ^ frame[4]
    if frame[5] != expected:
        raise DecodeError("acknowledgement checksum mismatch")
    return struct.unpack_from(">I", frame, 1)[0]


def encode_ack(sequence: int) -> bytes:
    """Builds the acknowledgement a gateway sends back."""
    body = struct.pack(">I", sequence & 0xFFFFFFFF)
    return bytes([0xA5]) + body + bytes([0xA5 ^ body[0] ^ body[1] ^ body[2] ^ body[3]])


def max_payload_bytes(spreading_factor: int) -> int:
    """Usable LoRa payload for a spreading factor, in bytes.

    SF10 and below cannot carry a single 60-byte record, so they are refused
    rather than silently truncating.
    """
    limits = {7: 222, 8: 222, 9: 115}
    budget = limits.get(spreading_factor, 0)
    if budget < OVERHEAD + RECORD_SIZE:
        raise DecodeError(
            f"SF{spreading_factor} carries {budget} B, "
            f"below the {OVERHEAD + RECORD_SIZE} B a record needs"
        )
    return budget
