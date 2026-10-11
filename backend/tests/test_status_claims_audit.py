"""Audit STATUS.md's negative claims against the code.

STATUS.md is the project's source of truth and it earns that by being corrected. Twice in one
day a line in the implemented table was false in the direction that matters: the document said
the firmware could not sign Ed25519 (it can) and that `main.cpp` never built the LoRa
transport (it does). Both outlived their changes, both sat in the most-read table in the
repository, and neither was caught - because prose does not fail a test.

So the claims that can be checked against code are checked against code. Only the ones with a
concrete, greppable subject are here; a sentence about hardware or about a decision is left
alone, because asserting on prose nobody can execute is how this file becomes the problem it is
replacing.

What this deliberately does NOT do: verify the cryptography, the hardware, or any measurement.
Those belong to the firmware suite, the bench, and nothing else. This file only asks whether
STATUS.md still matches the repository it describes.
"""

from __future__ import annotations

import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
STATUS = REPO / "STATUS.md"
MAIN_CPP = REPO / "firmware/src/main.cpp"

# Claims that are checkable, each as (the phrase STATUS must not contain, why, the pattern
# that proves the opposite is true). A phrase is only rejected when the code clearly has the
# thing - the point is to catch "not wired" outliving the wiring, not to second-guess prose.
CLAIMS: list[tuple[str, str, str, str]] = [
    (
        "the firmware cannot sign ed25519",
        "firmware/src does not build a LoRa transport",
        r"new cauce::app::LoRaSyncTransport",
        "main.cpp",
    ),
    (
        "no product firmware constructs the lora transport at all",
        "firmware/src does not build a LoRa transport",
        r"new cauce::app::LoRaSyncTransport",
        "main.cpp",
    ),
]


def _status() -> str:
    return STATUS.read_text(encoding="utf-8")


def _main() -> str:
    return MAIN_CPP.read_text(encoding="utf-8")


def test_status_contains_no_known_false_negative_claim():
    """Each phrase that has actually been wrong, asserted absent.

    One test rather than a loop, so a failure names the claim that regressed instead of
    pointing at a parameter.
    """
    text = _status().lower()
    for phrase, _why, _pat, _where in CLAIMS:
        assert phrase not in text, (
            f"STATUS.md says {phrase!r}, but the code has the thing it denies. "
            f"This sentence has been wrong before and a reader would have been misled.")


def test_every_rejected_claim_is_actually_contradicted_by_the_code():
    """The guard only works if the code really has what it denies.

    Without this, deleting the check would make the test above pass while proving nothing -
    which is the failure mode this whole file exists to prevent.
    """
    main = _main()
    for phrase, _why, pattern, where in CLAIMS:
        assert re.search(pattern, main), (
            f"the claim {phrase!r} is no longer rejected, but {where} does not match "
            f"{pattern!r} either - the table has drifted from the code in a new way and "
            f"needs a human to read it")


def test_status_names_the_lora_wiring_that_exists():
    """Positive form, so the fix is not only the absence of a wrong sentence."""
    text = _status()
    assert re.search(r"main\.cpp` builds\s*\n?`?Sx1276Radio", text), (
        "STATUS.md should say main.cpp builds the radio and transport behind the config "
        "flag, so a reader can go and check it")
    assert "never driven from `loop()`" in text or "never driven from" in text, (
        "constructed is not transmitting; the document must still say the sync is never "
        "driven from loop(), or the correction reads as the radio working")


def test_the_firmware_and_the_document_agree_on_signing():
    """The Ed25519 claim, stated positively, with both call sites named."""
    text = _status()
    assert re.search(r"\*\*the firmware signs\*\*", text, re.I)
    for call_site in ("NodeAuthenticator.cpp", "LoRaBatchCodec.cpp"):
        assert call_site in text, (
            f"{call_site} should be named as a signing call site; an unnamed "
            f"correction is one nobody can check")


def test_no_phrase_says_the_firmware_does_not_sign():
    text = _status().lower()
    for phrase in (
        "the firmware cannot sign ed25519",
        "firmware does not sign ed25519",
        "the firmware cannot sign",
    ):
        assert phrase not in text, f"STATUS.md still says {phrase!r}"


def test_the_hardware_claims_are_still_there():
    """The corrections above must not have quietly turned into overstatement.

    This is the direction that would actually be harmful. The boot loop is the single most
    important finding in the project and it has to survive any amount of documentation editing
    that claims the software is further along than it is.
    """
    text = _status()
    assert "does not run on real hardware" in text, (
        "the hardware finding has been edited out of STATUS.md; that sentence is the reason "
        "the repository is trustworthy and it cannot be quietly removed")
    assert "rst:0x8" in text, (
        "the watchdog reset reason is the evidence for the hardware finding; without it the "
        "claim is an assertion rather than a measurement")
