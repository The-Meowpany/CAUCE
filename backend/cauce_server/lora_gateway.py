"""LoRa gateway: relays signed frames without ever holding a node key.

**Why the gateway holds no keys.** HMAC is symmetric: a party that can verify a
signature with a shared secret can equally produce one. So a gateway that
verifies node frames is a gateway that can forge them, and the dependency never
goes away no matter where the signature is applied.

What actually removes it is the gateway never touching the key. The node signs
each frame it transmits, the gateway relays those bytes verbatim without
checking them, and the central verifies them against the key it issued at
provisioning — which it already holds. The relay is then trusted only to
deliver bytes. It can drop frames, reorder them, or flood the central with
garbage, and none of that lets it manufacture a measurement.

The gateway still reads frame headers, because it has to know when a batch is
complete enough to forward. That is framing metadata, not data.

The loop mirrors the node's sync loop, because the two have to agree:

1. Receive frames until the batch is complete.
2. Relay the batch as one `POST /v1/sync` carrying the signed frames verbatim.
3. Acknowledge the highest sequence the central durably stored, so the node can
   advance its watermark and forget the batch.
4. Only then drop the reassembly buffer, in a `finally`, so a failed forward
   does not leak the batch.

The radio driver is the only part that still needs hardware.
"""

from __future__ import annotations

import base64
import json
from dataclasses import dataclass

from .lora_frames import (
    SIGNATURE_BYTES,
    DecodeError,
    Reassembler,
    decode_frame,
    encode_ack,
    split_signed_frame,
)

MAX_PENDING_BATCHES = 4
MAX_FRAMES_PER_BATCH = 32


class GatewayError(RuntimeError):
    pass


@dataclass
class GatewayStats:
    frames_accepted: int = 0
    frames_rejected: int = 0
    batches_forwarded: int = 0
    batches_failed: int = 0
    measurements_forwarded: int = 0
    frames_relayed: int = 0

    def as_dict(self) -> dict:
        return {
            "frames_accepted": self.frames_accepted,
            "frames_rejected": self.frames_rejected,
            "batches_forwarded": self.batches_forwarded,
            "batches_failed": self.batches_failed,
            "measurements_forwarded": self.measurements_forwarded,
            "frames_relayed": self.frames_relayed,
        }


def frame_body(frame: bytes) -> bytes:
    """The framed data with any trailing signature removed.

    A node-signed frame is the framed bytes plus a 32-byte HMAC. Reading the
    framing does not mean reading the key: the signature is verified by the
    central, not here.
    """
    try:
        return split_signed_frame(frame)[0]
    except DecodeError:
        return frame


class LoRaGateway:
    """Reassembles LoRa batches and relays the node's signed frames verbatim.

    `transport` is anything with `post(url, body, headers) -> (status, text)`.
    Injecting it is what lets the whole loop be tested against the real FastAPI
    app without opening a socket to it.

    Deliberately takes no device keys: there is no constructor argument for
    them, so the weak configuration cannot be written by accident.
    """

    def __init__(self, sync_url: str, transport):
        self.sync_url = sync_url
        self.transport = transport
        self.stats = GatewayStats()
        self._pending: dict[int, tuple[Reassembler, list[bytes]]] = {}

    # -- receive side -------------------------------------------------

    def on_frame(self, frame: bytes) -> bytes | None:
        """Handles one received frame.

        Returns the acknowledgement bytes to transmit, or None when the frame
        was not the last of its batch. A rejected frame is counted rather than
        raised: a radio delivers noise routinely, and a gateway that raised on
        every bad CRC would restart on every storm.
        """
        try:
            header = decode_frame(frame_body(frame))
        except DecodeError:
            self.stats.frames_rejected += 1
            return None

        batch_id = header["batch_id"]
        entry = self._pending.get(batch_id)
        if entry is None:
            if len(self._pending) >= MAX_PENDING_BATCHES:
                # Full: drop the oldest so a node that died mid-batch cannot pin
                # memory forever and block the nodes that are still alive.
                self._pending.pop(next(iter(self._pending)))
                self.stats.frames_rejected += 1
                return None
            entry = (Reassembler(batch_id=batch_id), [])
            self._pending[batch_id] = entry
        reassembler, frames = entry

        try:
            complete = reassembler.add(frame_body(frame))
        except DecodeError:
            self.stats.frames_rejected += 1
            self._pending.pop(batch_id, None)
            return None

        frames.append(frame)
        self.stats.frames_accepted += 1
        if not complete:
            return None

        try:
            return self.relay(frames, header)
        finally:
            self._pending.pop(batch_id, None)

    # -- forward side -------------------------------------------------

    def relay(self, frames: list[bytes], header: dict) -> bytes:
        """Forwards the frames exactly as received.

        No signature is checked and none is added. The relay authenticates
        nothing; `lora_ingest.expand_signed_frames` at the central is the only
        place a frame's provenance is decided.
        """
        if not frames:
            raise GatewayError("nothing to relay")
        node_id = header["records"][0].node_id if header["records"] else ""
        payload = {
            "protocol_version": 1,
            "node_id": node_id,
            "transport": "lora",
            "batch_id": header["batch_id"],
            "record_count": header["record_count"],
            "frames": [base64.b64encode(f).decode("ascii") for f in frames],
        }
        body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        headers = {"Content-Type": "application/json",
                   "X-CAUCE-Node": node_id,
                   # Says who is relaying. It is an admission, not a claim.
                   "X-CAUCE-Relay": "1"}

        status, text = self.transport.post(self.sync_url, body, headers)
        if status != 200:
            self.stats.batches_failed += 1
            raise GatewayError(
                f"central rejected the batch: {status} {text[:120]}"
            )

        try:
            response = json.loads(text)
        except ValueError as exc:
            self.stats.batches_failed += 1
            raise GatewayError("central returned a body that is not JSON") from exc

        acked = int(response.get("acknowledged_sequence") or 0)
        self.stats.batches_forwarded += 1
        self.stats.frames_relayed += len(frames)
        self.stats.measurements_forwarded += header["record_count"]
        return encode_ack(acked if acked else header["record_count"])

    # -- introspection ------------------------------------------------

    def pending_batches(self) -> int:
        return len(self._pending)

    def signed_frame_overhead(self) -> int:
        """Bytes a signature adds to every frame, for budget calculations."""
        return SIGNATURE_BYTES
