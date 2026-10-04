"""Accepts a relayed narrowband batch and turns it into measurements.

A gateway does not hold node keys, so it cannot sign the JSON body the way the
direct Wi-Fi path does. Instead it relays the frames the node signed, and the
central verifies each one against the key it issued at provisioning.

This is what restores the device-authenticity property for the narrowband path:
the relay is trusted to deliver bytes, not to vouch for them. It can drop frames
or misreport framing metadata, but altering a record invalidates the signature
and the batch is rejected here.
"""

from __future__ import annotations

import base64

from fastapi import HTTPException

from . import revocation
from .db import query
from .lora_frames import (
    MAX_RECORDS,
    DecodeError,
    Reassembler,
    decode_frame,
    split_signed_frame,
)
from .signing import SignatureError, verify_frame


def _node_key_material(node_id: str) -> tuple[str | None, str | None]:
    """The node's key and the algorithm that key belongs to.

    Raises `NodeRetired` when the node's identity has been retired, before the key
    is even read. A retired node with a perfectly valid signature must not be
    ingested, and the cheapest place to say so is where the key would have come
    from.

    Both values come from provisioning, never from the request. A frame cannot
    choose its own verification algorithm, because that would let an attacker
    relabel an HMAC frame as Ed25519 and have the central run whichever check it
    liked.
    """
    revocation.require_not_retired(node_id)

    rows = query(
        "SELECT device_key, device_key_algorithm FROM nodes WHERE node_id=?",
        (node_id,),
    )
    if not rows:
        return None, None
    return rows[0]["device_key"], rows[0]["device_key_algorithm"]


def expand_signed_frames(payload: dict) -> dict:
    """Verifies and decodes a relayed batch, returning a normal sync payload.

    Raises 401 when a frame does not verify and 422 when the batch is
    structurally wrong. Reassembly happens before verification only far enough
    to know which frames belong together; every frame's signature is checked
    individually and none is trusted because a sibling verified.
    """
    node_id = payload.get("node_id")
    if not isinstance(node_id, str) or not node_id:
        raise HTTPException(status_code=422, detail="missing_node_id")

    raw_frames = payload.get("frames")
    if not isinstance(raw_frames, list) or not raw_frames:
        raise HTTPException(status_code=422, detail="missing_frames")
    if len(raw_frames) > 32:
        raise HTTPException(status_code=422, detail="too_many_frames")

    device_key, algorithm = _node_key_material(node_id)
    if not device_key:
        # Without a key there is nothing to verify against, and accepting the
        # batch anyway would be exactly the downgrade the frame signature
        # exists to prevent.
        raise HTTPException(status_code=401, detail="node_not_provisioned")

    frames: list[bytes] = []
    for encoded in raw_frames:
        if not isinstance(encoded, str):
            raise HTTPException(status_code=422, detail="invalid_frame")
        try:
            frame = base64.b64decode(encoded, validate=True)
        except Exception as exc:
            raise HTTPException(status_code=422,
                                detail="invalid_frame") from exc
        if not verify_frame(frame, device_key, algorithm):
            raise HTTPException(status_code=401, detail="invalid_frame_signature")
        frames.append(frame)

    # The header is read for grouping only; the bytes that carry records were
    # verified above and are not re-read from the relay in any trusted way.
    try:
        first = decode_frame(_body_of(frames[0], algorithm))
    except DecodeError as exc:
        raise HTTPException(status_code=422, detail="unparsable_frame") from exc

    batch_id = first["batch_id"]
    reassembler = Reassembler(batch_id=batch_id)
    try:
        for frame in frames:
            reassembler.add(_body_of(frame, algorithm))
    except DecodeError as exc:
        raise HTTPException(status_code=422, detail="bad_reassembly") from exc
    if not reassembler.complete:
        raise HTTPException(status_code=422, detail="incomplete_batch")

    records = reassembler.records()
    if len(records) > MAX_RECORDS:
        raise HTTPException(status_code=422, detail="batch_too_large")
    # Every record must claim the node the key belongs to. Without this a node
    # could present another node's frames under its own name.
    foreign = [r.node_id for r in records if r.node_id != node_id]
    if foreign:
        raise HTTPException(status_code=401, detail="node_id_mismatch")

    announced = payload.get("record_count")
    if isinstance(announced, int) and announced != len(records):
        raise HTTPException(status_code=422, detail="record_count_mismatch")

    return {
        "protocol_version": payload.get("protocol_version", 1),
        "node_id": node_id,
        "transport": "lora",
        "measurements": [r.as_measurement() for r in records],
    }


def _body_of(frame: bytes, algorithm: str | None = None) -> bytes:
    """Strips the trailing signature so structural checks see the framed data.

    The trailer length depends on the node's algorithm, so this cannot assume
    32 bytes and get the right answer for an Ed25519 node.
    """
    try:
        return split_signed_frame(frame, algorithm)[0]
    except SignatureError:
        return frame
