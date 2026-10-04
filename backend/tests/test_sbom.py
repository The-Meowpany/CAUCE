"""The SBOM's own guarantees.

An SBOM nobody can check is a file. These check that it describes the environment
that is actually installed rather than the one that was requested, which is the
distinction that makes it worth generating at all.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

import sbom as sbom_tool  # noqa: E402


def test_the_sbom_is_valid_cyclonedx():
    doc = sbom_tool.build()
    assert doc["bomFormat"] == "CycloneDX"
    assert doc["specVersion"] == "1.5"
    assert doc["components"], "an SBOM with no components is worse than none"


def test_every_component_has_a_name_a_version_and_a_purl():
    for component in sbom_tool.build()["components"]:
        assert component["name"], component
        assert component["version"], component
        assert component["purl"].startswith("pkg:pypi/"), component


def test_the_purl_version_matches_the_component_version():
    for component in sbom_tool.build()["components"]:
        assert component["purl"].endswith(f"@{component['version']}"), component


def test_cryptography_is_present_because_it_verifies_every_signature():
    """The one dependency whose version matters. A vulnerability in it is a
    vulnerability in every node's identity check."""
    names = {c["name"].lower() for c in sbom_tool.build()["components"]}
    assert "cryptography" in names


def test_the_sbom_describes_the_installed_environment_not_the_request():
    """requirements.txt says what was asked for. Only the installed distribution
    says what is there. The difference is routine, and it is the whole reason to
    generate an SBOM rather than read a requirements file.

    Scoped to the direct dependencies rather than every distribution in the
    interpreter: walking all of them takes minutes, and the claim being made is
    about what the SBOM tool can see, not about pip's full inventory.
    """
    from importlib import metadata

    doc = sbom_tool.build()
    described = {c["name"].lower() for c in doc["components"]}
    for name in sbom_tool.DIRECT:
        try:
            metadata.distribution(name)
        except metadata.PackageNotFoundError:
            continue
        assert name in described, f"{name} is installed but absent from the SBOM"


def test_the_pin_check_passes_for_this_environment():
    # If this fails, the pinned floor in the tool no longer matches reality and the
    # pin is either stale or the environment drifted. Either way it should be
    # noticed here rather than in a vulnerability scan weeks later.
    assert sbom_tool.check_pins(sbom_tool.build()) == []


def test_an_unsatisfied_pin_is_reported():
    """The tripwire has to actually trip, or it is decoration."""
    doc = {"components": [{"name": "cryptography", "version": "3.0"}]}
    problems = sbom_tool.check_pins(doc)
    assert problems
    assert "cryptography" in problems[0]


def test_a_missing_pinned_dependency_is_reported():
    problems = sbom_tool.check_pins({"components": []})
    assert problems
    assert "NOT installed" in problems[0]


def test_the_serial_number_is_stable_for_the_same_project():
    """Rebuilding the same tree must produce the same serial number, so two SBOMs
    can be diffed instead of compared by reading them."""
    first = sbom_tool.build("cauce-central", "1.0.0")
    second = sbom_tool.build("cauce-central", "1.0.0")
    assert first["serialNumber"] == second["serialNumber"]


def test_a_different_version_gets_a_different_serial_number():
    assert (sbom_tool.build("cauce-central", "1.0.0")["serialNumber"] !=
            sbom_tool.build("cauce-central", "1.0.1")["serialNumber"])


def test_direct_and_transitive_are_distinguished():
    """Which dependencies the code imports, and which arrived alongside them. An
    operator reading a vulnerability advisory needs to know which list to look at."""
    doc = sbom_tool.build()
    properties = {p["name"]: p["value"] for p in doc["metadata"]["properties"]}
    assert "cryptography" in properties["cauce:direct-dependencies"]
    assert "fastapi" in properties["cauce:direct-dependencies"]
    assert int(properties["cauce:transitive-dependency-count"]) > 0


def test_version_tuples_handle_two_and_four_part_versions():
    assert sbom_tool._version_tuple("42.0.5") == (42, 0, 5)
    assert sbom_tool._version_tuple("43.0.0") > sbom_tool._version_tuple("42.9.9")
    # A pre-release suffix must not be read as a lower number than it is.
    assert sbom_tool._version_tuple("45.0.1") >= sbom_tool._version_tuple("42.0.0")


def test_it_writes_a_file(tmp_path, capsys):
    out = tmp_path / "nested" / "central.cdx.json"
    assert sbom_tool.main(["--out", str(out), "--strict"]) == 0
    doc = json.loads(out.read_text(encoding="utf-8"))
    assert doc["bomFormat"] == "CycloneDX"
    assert "wrote" in capsys.readouterr().out


def test_strict_mode_fails_on_a_bad_pin(monkeypatch):
    monkeypatch.setattr(sbom_tool, "check_pins",
                        lambda doc: ["cryptography is too old"])
    assert sbom_tool.main(["--out", str(Path("sbom/x.json"))]) == 0
    assert sbom_tool.main(["--out", str(Path("sbom/x.json")), "--strict"]) == 1