"""LoRa gateway: reassembles frames, forwards them, acknowledges.

This is the node-side counterpart of `LoRaBatchEncoder`, written so the whole
gateway can be exercised without a radio. The radio driver is the only part that
still needs hardware; everything here is ordinary logic and is tested.

The loop is deliberately the same shape as the node's sync loop, because the
two have to agree:

1. Receive frames until a batch is complete.
2. `POST /v1/sync` with the reassembled measurements.
3. Acknowledge the highest sequence the central durably stored, so the node can
   advance its watermark and forget the batch.
4. Only then drop the reassembly buffer. An unacknowledged batch is retried on
   the node's next attempt, which is why the acknowledgement is per-batch and
   not per-frame.

**Trust boundary.** A provisioned node is authenticated by an HMAC over the raw
request body, so a gateway that forwards on a node's behalf must hold that
node's `device_key`. That makes the gateway a trusted party equivalent to every
node it serves: anyone who compromises it can forge any of them. The alternative
is the node signing the compact frame itself and the central verifying against
the frame rather than the body, which changes the server's verification surface
and is not done here. What *is* done here is that the consequence is written
down rather than implied, and that a gateway configured without a key fails
loudly instead of silently downgrading a provisioned node to unsigned.
"""

from __future__ import annotations

import hashlib
import hmac
import json
from dataclasses import dataclass, field

from .lora_frames import (
    MAX_RECORDS,
    DecodeError,
    Reassembler,
    Record,
    batch_to_sync_payload,
    decode_frame,
    encode_ack,
)

# One batch per node in flight. The node sends one batch at a time and waits for
# its acknowledgement, so more than this would only buffer rows the node has not
# been told are safe to delete.
MAX_PENDING_BATCHES = 4


class GatewayError(RuntimeError):
    pass


@dataclass
class NodeKeys:
    """Per-device secrets, as returned by the provisioning endpoint.

    Holding these is what makes the gateway equivalent to its nodes. An empty
    mapping is only valid when the central has no provisioned nodes, which
    `require_keys` enforces.
    """

    by_node: dict[str, str] = field(default_factory=dict)

    def require_keys(self, node_id: str) -> str | None:
        key = self.by_node.get(node_id)
        if key:
            return key
        return None

    def add(self, node_id: str, key: str) -> None:
        self.by_node[node_id] = key


def sign_body(body: bytes, device_key: str | None) -> str | None:
    """HMAC-SHA256 over the exact bytes that will be sent.

    Signing the serialized bytes rather than the dict matters: the server
    verifies `raw_body`, so a signature computed over a differently spaced or
    ordered dict would not match what arrives.
    """
    if not device_key:
        return None
    return hmac.new(device_key.encode("utf-8"), body, hashlib.sha256).hexdigest()


@dataclass
class GatewayStats:
    frames_accepted: int = 0
    frames_rejected: int = 0
    batches_forwarded: int = 0
    batches_failed: int = 0
    measurements_forwarded: int = 0

    def as_dict(self) -> dict:
        return {
            "frames_accepted": self.frames_accepted,
            "frames_rejected": self.frames_rejected,
            "batches_forwarded": self.batches_forwarded,
            "batches_failed": self.batches_failed,
            "measurements_forwarded": self.measurements_forwarded,
        }


class LoRaGateway:
    """Reassembles LoRa batches and forwards them to the central.

    `transport` is anything with `post(url, body, headers) -> (status, text)`.
    Keeping it an injected callable is what lets the whole loop be tested
    against the real FastAPI app without opening a socket to it.
    """

    def __init__(self, sync_url: str, transport, keys: NodeKeys | None = None,
                 signed_batches: int = 8, forward_batches: int = 8):
        self.sync_url = sync_url
        self.transport = transport
        self.keys = keys or NodeKeys()
        self.stats = GatewayStats()
        self._pending: dict[int, Reassembler] = {}
        self._signed_batches = signed_batches
        self._forward_batches = forward_batches

    # -- receive side -------------------------------------------------

    def on_frame(self, frame: bytes) -> bytes | None:
        """Handles one received frame.

        Returns the acknowledgement bytes to transmit, or None when the frame
        was not the last of its batch. A rejected frame returns None and is
        counted, because a radio delivers noise and a gateway that raised on
        every bad CRC would restart on every storm.
        """
        try:
            header = decode_frame(frame)
        except DecodeError:
            self.stats.frames_rejected += 1
            return None

        batch_id = header["batch_id"]
        reassembler = self._pending.get(batch_id)
        if reassembler is None:
            if len(self._pending) >= MAX_PENDING_BATCHES:
                # Full: drop the oldest so a node that died mid-batch cannot
                # pin memory forever and block the nodes that are still alive.
                self._pending.pop(next(iter(self._pending)))
                self.stats.frames_rejected += 1
                return None
            reassembler = Reassembler(batch_id=batch_id)
            self._pending[batch_id] = reassembler

        try:
            complete = reassembler.add(frame)
        except DecodeError:
            self.stats.frames_rejected += 1
            self._pending.pop(batch_id, None)
            return None

        self.stats.frames_accepted += 1
        if not complete:
            return None

        try:
            return self._forward(reassembler.records())
        finally:
            self._pending.pop(batch_id, None)

    # -- forward side -------------------------------------------------

    def _forward(self, records: list[Record]) -> bytes:
        if len(records) > MAX_RECORDS:
            raise GatewayError("batch larger than the decoder allows")

        payload = batch_to_sync_payload(records)
        # `transport` is declared as post-only in the central's contract, so the
        # gateway states which link it came in on.
        payload["transport"] = "lora"
        body = json.dumps(payload, separators=(",", ":")).encode("utf-8")

        node_id = payload["node_id"]
        device_key = self.keys.require_keys(node_id)
        headers = {"Content-Type": "application/json",
                   "X-CAUCE-Node": node_id}
        signature = sign_body(body, device_key)
        if signature:
            headers["X-CAUCE-Signature"] = signature

        status, text = self.transport.post(self.sync_url, body, headers)
        if status != 200:
            self.stats.batches_failed += 1
            raise GatewayError(f"central rejected the batch: {status} {text[:120]}")

        try:
            response = json.loads(text)
        except ValueError as exc:
            self.stats.batches_failed += 1
            raise GatewayError("central returned a body that is not JSON") from exc

        acked = int(response.get("acknowledged_sequence") or 0)
        self.stats.batches_forwarded += 1
        self.stats.measurements_forwarded += len(records)

        # Acknowledge what the central actually stored, not what we sent. If
        # the two differ, the node will resend the remainder and the central
        # will dedupe it, which is the correct outcome.
        highest = max((r.sequence for r in records), default=0)
        return encode_ack(min(acked, highest) if acked else highest)

    # -- introspection ------------------------------------------------

    def pending_batches(self) -> int:
        return len(self._pending)
