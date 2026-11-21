"""Framing for the USB serial link to the LoRa gateway bridge.

The mirror of `firmware/lib/cauce_core/src/SerialBridge.cpp`, byte for byte:

    magic 0xC5 0xA0 | type (1) | length (2, big-endian) | payload | CRC-16/CCITT-FALSE (2)

The CRC covers type, length and payload. The decoder resynchronises on the next magic after
garbage, which is what a bridge that resets mid-write leaves behind.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass

from .lora_frames import crc16

MAGIC = b"\xc5\xa0"
MAX_PAYLOAD = 300

RADIO_RECEIVED = 0x01
RADIO_TRANSMIT = 0x02
STATUS = 0x03


def encode(frame_type: int, payload: bytes) -> bytes:
    if len(payload) > MAX_PAYLOAD:
        raise ValueError(f"payload of {len(payload)} bytes exceeds {MAX_PAYLOAD}")
    body = struct.pack(">BH", frame_type, len(payload)) + payload
    return MAGIC + body + struct.pack(">H", crc16(body))


@dataclass
class BridgeFrame:
    frame_type: int
    payload: bytes

    def radio_frame(self) -> tuple[int, bytes]:
        """For a received radio frame: (RSSI in dBm, the LoRa bytes)."""
        if self.frame_type != RADIO_RECEIVED or len(self.payload) < 2:
            raise ValueError("not a received radio frame")
        (rssi,) = struct.unpack(">h", self.payload[:2])
        return rssi, self.payload[2:]


class Decoder:
    """Streaming decoder: feed it whatever the port returned, get whole frames back."""

    def __init__(self) -> None:
        self._buffer = bytearray()
        self.errors = 0

    def feed(self, data: bytes) -> list[BridgeFrame]:
        self._buffer.extend(data)
        frames: list[BridgeFrame] = []
        while True:
            start = self._buffer.find(MAGIC)
            if start < 0:
                # Keep a trailing first-magic byte: the second may be in the next read.
                keep = 1 if self._buffer[-1:] == MAGIC[:1] else 0
                del self._buffer[:len(self._buffer) - keep]
                return frames
            del self._buffer[:start]
            if len(self._buffer) < 5:
                return frames
            frame_type, length = struct.unpack(">BH", self._buffer[2:5])
            if length > MAX_PAYLOAD:
                self.errors += 1
                del self._buffer[:2]
                continue
            total = 5 + length + 2
            if len(self._buffer) < total:
                return frames
            body = bytes(self._buffer[2:5 + length])
            (crc,) = struct.unpack(">H", self._buffer[5 + length:total])
            if crc != crc16(body):
                self.errors += 1
                # Skip only the magic: the real frame may start inside what looked like one.
                del self._buffer[:2]
                continue
            frames.append(BridgeFrame(frame_type, body[3:]))
            del self._buffer[:total]
