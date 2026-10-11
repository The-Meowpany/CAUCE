"""STATUS.md claimed the firmware could not sign Ed25519. It could.

This is here because the claim sat in the most-read line of the document - the implemented
table - and was false for as long as nobody re-read the code underneath it. `ed25519Sign` has
been implemented, wired into the challenge path and the LoRa frame path, and covered by a test
that uses the RFC 8032 section 7.1 TEST 1 seed. The sentence was stale and nothing noticed,
because STATUS.md is prose and prose does not fail a test.

So the check is executable and lives next to the file it guards. It is deliberately narrow: it
verifies that the calls exist and are reachable from the two paths, not that the
cryptography is correct - the RFC 8032 known-answer tests in the firmware suite own that, and
this file must not become a second, weaker place where signing is "verified".

If the firmware ever stops signing, this fails and the documentation has to change with it,
rather than the other way round.
"""

from __future__ import annotations

import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
STATUS = REPO / "STATUS.md"
CORE_SRC = REPO / "firmware/lib/cauce_core/src/Ed25519Points.cpp"
APP_SRC = REPO / "firmware/lib/cauce_app/src"
CODEC_SRC = REPO / "firmware/lib/cauce_core/src/LoRaBatchCodec.cpp"
TESTS = REPO / "firmware/test"


def _status_text() -> str:
    return STATUS.read_text(encoding="utf-8")


def test_the_signing_primitive_is_actually_implemented():
    """`ed25519Sign` exists and is a definition, not a declaration or a stub."""
    src = CORE_SRC.read_text(encoding="utf-8")
    m = re.search(r"bool ed25519Sign\([^)]*\)\s*\{", src)
    assert m, "ed25519Sign has no body in Ed25519Points.cpp"
    body = src[m.start():m.start() + 4000]
    # A definition that immediately returns false would satisfy a presence check.
    assert "return false;" not in body.split("ed25519Verify")[0][:600] or "ScalarMult" in body, (
        "ed25519Sign looks like it fails early rather than signing")


def test_the_challenge_path_signs():
    """NodeAuthenticator signs the challenge it was handed.

    This is the authentication path: the node proves possession of its seed by signing a
    server-issued string. If this stops happening the central cannot tell a provisioned node
    from anyone else, and the failure would be an authentication failure at ingest - which
    reads as a server problem.
    """
    src = (APP_SRC / "NodeAuthenticator.cpp").read_text(encoding="utf-8")
    assert "ed25519Sign" in src, (
        "NodeAuthenticator no longer signs the challenge; node authentication over a "
        "certificate would be broken and nothing else would say so")
    # And a refused signature has to be a distinct outcome, not a frame sent unsigned.
    assert "SignFailed" in src, (
        "a signature failure must be its own status; returning early and sending nothing "
        "unsigned is the only safe alternative, and there is no status for it")


def test_the_lora_frame_path_signs():
    """LoRaBatchCodec signs the frame when the algorithm says Ed25519."""
    src = CODEC_SRC.read_text(encoding="utf-8")
    assert "ed25519Sign" in src, (
        "the LoRa frame is no longer signed under Ed25519, so a 64-byte trailer would be "
        "carrying something that was never produced by a signature")
    # The HMAC branch must remain, or the cheap algorithm for constrained nodes is gone.
    assert "hmacSha256" in src.lower() or "HmacSha256" in src


def test_a_known_answer_vector_pins_the_cryptography():
    """The RFC 8032 seed is used somewhere in the firmware tests.

    Not the signature itself - the firmware suite owns that - but the seed appearing proves
    the signing path is exercised against the published vector rather than only against
    itself, which is the difference between a signer and a random-looking byte producer.
    """
    seed = "9d61b19deffd5a60ba844af492ec2cc4"
    hits = [p.name for p in TESTS.glob("*.cpp") if seed in p.read_text(encoding="utf-8")]
    assert hits, f"the RFC 8032 TEST 1 seed appears in no firmware test; files scanned: {list(p.name for p in TESTS.glob('*.cpp'))[:5]}"


def test_status_does_not_claim_the_firmware_cannot_sign():
    """The specific false sentence, asserted so it cannot return.

    STATUS.md is the project's source of truth and it earns that by being corrected. This
    particular line was wrong in the most damaging direction available: it understated what
    the firmware does, in a document whose entire value is that it does not.
    """
    text = _status_text().lower()
    forbidden = [
        "the firmware cannot sign ed25519",
        "firmware cannot sign ed25519 yet",
        "firmware does not sign ed25519",
    ]
    for phrase in forbidden:
        assert phrase not in text, (
            f"STATUS.md says {phrase!r}, but ed25519Sign is implemented, wired into the "
            f"challenge and the frame, and covered by an RFC 8032 vector")


def test_status_states_that_the_firmware_signs():
    """The positive claim, so the fix is not only the absence of the wrong sentence."""
    text = _status_text()
    assert re.search(r"\*\*the firmware signs\*\*", text, re.I), (
        "STATUS.md should say the firmware signs, with the two call sites named, so a reader "
        "can go and check it")
