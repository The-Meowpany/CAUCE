"""The lock file, checked against the file it is supposed to be pinning.

A lock is only useful while it agrees with its source. The failure mode is quiet: the
lock keeps installing the old tree, CI stays green, and requirements.txt has said
something else for a month. These checks are what make the drift visible at the moment it
happens rather than at the next incident review.
"""

from __future__ import annotations

import re
from pathlib import Path

import pytest

BACKEND = Path(__file__).resolve().parents[1]
REQUIREMENTS = BACKEND / "requirements.txt"
LOCK = BACKEND / "requirements.lock"


def _direct_requirements() -> dict[str, str]:
    """name -> version specifier, from requirements.txt.

    Parsed with a regex rather than with packaging.requirements on purpose: requirements.txt
    has comments and extras in it, and the point here is to compare the file a human edits
    against the generated one. A hand parser that handles this exact format is easier to
    trust than a dependency doing it.
    """
    out: dict[str, str] = {}
    for raw in REQUIREMENTS.read_text(encoding="utf-8").splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        match = re.match(r"^(?P<name>[A-Za-z0-9_.\-]+)(\[[^\]]*\])?(?P<spec>.*)$", line)
        assert match, f"cannot parse requirement line: {raw!r}"
        out[match.group("name").lower().replace("_", "-")] = (
            match.group("spec") or ">=0").strip()
    return out


def _locked() -> dict[str, str]:
    """name -> pinned version, from the lock."""
    out: dict[str, str] = {}
    for raw in LOCK.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("--hash"):
            continue
        # Requirement lines end in the backslash that continues the hash lines below.
        name, _, version = line.rstrip("\\").strip().partition("==")
        assert version, f"lock line is not pinned: {raw!r}"
        out[name.lower().replace("_", "-")] = version
    return out


def _version_tuple(value: str) -> tuple[int, ...]:
    parts = []
    for chunk in value.split(".")[:3]:
        digits = "".join(c for c in chunk if c.isdigit())
        parts.append(int(digits) if digits else 0)
    return tuple(parts)


def test_every_direct_requirement_is_pinned():
    locked = _locked()
    missing = sorted(name for name in _direct_requirements() if name not in locked)
    assert not missing, f"in requirements.txt but not in the lock: {missing}"


def test_every_pinned_version_satisfies_its_requirement():
    """The check that catches a requirements.txt bump nobody regenerated for."""
    locked = _locked()
    unsatisfied = []
    for name, spec in _direct_requirements().items():
        pinned = locked.get(name)
        if pinned is None:
            continue
        if spec.startswith(">="):
            if _version_tuple(pinned) < _version_tuple(spec[2:]):
                unsatisfied.append(f"{name}: lock has {pinned}, needs {spec}")
        elif spec.startswith("=="):
            if pinned != spec[2:]:
                unsatisfied.append(f"{name}: lock has {pinned}, needs {spec}")
        else:
            pytest.fail(f"unhandled specifier {spec!r} for {name}; extend this test "
                        f"rather than letting it pass unchecked")
    assert not unsatisfied, "the lock does not satisfy requirements.txt:\n" + "\n".join(
        unsatisfied)


def test_every_pinned_package_carries_at_least_one_hash():
    """A pin without a hash is a pin, not a lock: it names a version but still trusts
    whatever the index served for it."""
    blocks = re.split(r"(?m)^(?=[A-Za-z0-9_.\-]+==)", LOCK.read_text(encoding="utf-8"))
    unhashed = []
    for block in blocks:
        if "==" not in block:
            continue
        if "--hash=sha256:" not in block:
            unhashed.append(block.split("==")[0].strip())
    assert not unhashed, f"pinned without a hash: {unhashed}"


def test_the_lock_pins_more_than_requirements_txt_asks_for():
    """It is a closure, not a copy. If these were equal, the transitive dependencies
    would be unpinned and the whole point would be missing."""
    assert len(_locked()) > len(_direct_requirements())


def test_the_hashes_are_sha256_and_not_placeholders():
    for digest in re.findall(r"--hash=(\S+)", LOCK.read_text(encoding="utf-8")):
        assert digest.startswith("sha256:"), digest
        value = digest.split(":", 1)[1]
        assert len(value) == 64, digest
        assert re.fullmatch(r"[0-9a-f]{64}", value), digest
