#!/usr/bin/env python3
"""Produce an SBOM for the CAUCE central.

WHAT AN SBOM IS FOR HERE, SPECIFICALLY

`cryptography` is the dependency that matters. It is the one that verifies Ed25519
signatures and terminates nothing, and a vulnerability in it is a vulnerability in
every node's identity check. An SBOM that lists it with a resolved version is what
lets an operator answer "are we affected" in minutes instead of by reading a
requirements file and guessing what pip actually installed.

This produces CycloneDX 1.5 because it is the format scanners actually consume.
Producing a format nobody reads is the same as producing none.

    python tools/sbom.py --out sbom/central.cdx.json

It reads the INSTALLED distribution, not requirements.txt. The difference is the
whole point: requirements.txt says what was asked for, the environment says what is
there, and those differ routinely.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from importlib import metadata
from pathlib import Path

# Components whose version or presence is worth failing the build over. Not a
# vulnerability database - this is a tripwire for "the pinned dependency stopped
# being what we pinned".
PINNED = {"cryptography": "42"}

# Direct dependencies, i.e. what the code imports rather than what arrived
# alongside something else.
DIRECT = {
    "fastapi",
    "uvicorn",
    "cryptography",
    "pydantic",
    "httpx",
    "pytest",
}


def _component(name: str) -> dict | None:
    try:
        dist = metadata.distribution(name)
    except metadata.PackageNotFoundError:
        return None

    version = dist.version
    entry: dict = {
        "type": "library",
        "name": name,
        "version": version,
        "purl": f"pkg:pypi/{name}@{version}",
    }
    # A hash of the distribution, when the metadata exposes one. Useful for
    # answering "is this the same artefact we deployed last month".
    digest = None
    for candidate in dist.files or []:
        if str(candidate).endswith(".dist-info/RECORD") or \
                str(candidate).endswith(".egg-info/PKG-INFO"):
            try:
                data = (dist.locate_file(candidate)).read_bytes()
                digest = hashlib.sha256(data).hexdigest()
            except OSError:
                pass
            break
    if digest:
        entry["hashes"] = [{"alg": "SHA-256", "content": digest}]
    return entry


_CACHE: dict[tuple[str, str], dict] = {}


def build(project_name: str = "cauce-central", project_version: str = "0.0.0") -> dict:
    """The SBOM for the current environment.

    Cached because it does not change while the process runs, and walking 228
    distributions takes long enough that an uncached build() makes the test suite
    crawl. `cached=False` forces a rebuild for the tests that need one.
    """
    key = (project_name, project_version)
    if key in _CACHE:
        return _CACHE[key]
    doc = _build_uncached(project_name, project_version)
    _CACHE[key] = doc
    return doc


def _build_uncached(project_name: str, project_version: str) -> dict:
    installed = sorted(
        {dist.metadata["Name"] for dist in metadata.distributions()
         if dist.metadata["Name"]},
        key=str.lower,
    )
    components = [c for c in (_component(n) for n in installed) if c]

    direct, transitive = [], []
    for component in components:
        (direct if component["name"].lower() in DIRECT else transitive).append(
            f"{component['name']}@{component['version']}")

    return {
        "bomFormat": "CycloneDX",
        "specVersion": "1.5",
        "serialNumber": f"urn:uuid:{_uuid_for(project_name, project_version)}",
        "version": 1,
        "metadata": {
            "component": {
                "type": "application",
                "name": project_name,
                "version": project_version,
            },
            "properties": [
                {"name": "cauce:direct-dependencies",
                 "value": ", ".join(direct)},
                {"name": "cauce:transitive-dependency-count",
                 "value": str(len(transitive))},
            ],
        },
        "components": components,
    }


def _uuid_for(name: str, version: str) -> str:
    """A stable serial number derived from name and version.

    Stable rather than random on purpose: rebuilding the same tree should produce
    the same SBOM, so two artefacts can be diffed instead of compared by hand.
    """
    digest = hashlib.sha256(f"{name}@{version}".encode()).hexdigest()
    return f"{digest[0:8]}-{digest[8:12]}-{digest[12:16]}-{digest[16:20]}-{digest[20:32]}"


def check_pins(sbom: dict) -> list[str]:
    """Problems worth failing on. Printed rather than raised, so the caller decides."""
    problems = []
    versions = {c["name"].lower(): c["version"] for c in sbom["components"]}
    for name, minimum in PINNED.items():
        found = versions.get(name)
        if found is None:
            problems.append(f"{name} is NOT installed but is pinned to >={minimum}")
        elif _version_tuple(found) < _version_tuple(minimum):
            problems.append(f"{name} {found} is older than the pinned {minimum}")
    return problems


def _version_tuple(value: str) -> tuple[int, ...]:
    parts = []
    for chunk in value.split(".")[:3]:
        digits = "".join(c for c in chunk if c.isdigit())
        parts.append(int(digits) if digits else 0)
    return tuple(parts)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Emit a CycloneDX SBOM.")
    parser.add_argument("--out", help="write here instead of stdout")
    parser.add_argument("--project-name", default="cauce-central")
    parser.add_argument("--project-version", default="0.0.0")
    parser.add_argument("--strict", action="store_true",
                        help="exit non-zero when a pin is unsatisfied")
    args = parser.parse_args(argv)

    try:
        import cauce_server  # noqa: F401
        args.project_version = getattr(cauce_server, "__version__",
                                       args.project_version)
    except Exception:  # pragma: no cover - running outside the package
        pass

    sbom = build(args.project_name, args.project_version)
    body = json.dumps(sbom, indent=2, sort_keys=True) + "\n"

    if args.out:
        path = Path(args.out)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(body, encoding="utf-8")
        print(f"wrote {path} with {len(sbom['components'])} components")
    else:
        sys.stdout.write(body)

    problems = check_pins(sbom)
    for problem in problems:
        print(f"PIN PROBLEM: {problem}", file=sys.stderr)
    if problems and args.strict:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())