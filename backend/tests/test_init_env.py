"""The environment file is checked before an operator finds out in a browser.

An operator following the deployment docs has to invent three credentials. Every way of getting
it wrong is quiet: an empty admin token is the *designed* behaviour and looks like a fault, a
value pasted with its quotes is a 401 that looks like a client bug, and setting the CA seed
equal to the admin token is a compromise away from minting a certificate for any node. None of
these announce themselves.

So the check exists, and these tests are here because a checker that has only ever been run
against a file it generated proves nothing - it would pass a check that had no rules in it.
Every case below is a file written by hand, broken in one specific way.
"""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

TOOL = Path(__file__).resolve().parents[1] / "tools" / "init_env.py"


def _load():
    spec = importlib.util.spec_from_file_location("init_env", TOOL)
    mod = importlib.util.module_from_spec(spec)
    sys.modules["init_env"] = mod
    spec.loader.exec_module(mod)
    return mod


init_env = _load()

GOOD_CA = "00" * 32


def write_env(path: Path, **overrides) -> Path:
    values = {
        "CAUCE_API_TOKEN": "an-admin-token-value",
        "CAUCE_SYNC_TOKEN": "a-sync-token-value",
        "CAUCE_CA_KEY": GOOD_CA,
    }
    values.update({k: v for k, v in overrides.items() if v is not None})
    lines = [f"{k}={v}" for k, v in values.items() if v is not None]
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return path


# --------------------------------------------------------------------- generation


def test_generated_values_are_distinct_and_the_right_shape():
    values = init_env.generate()
    assert len({values["CAUCE_API_TOKEN"], values["CAUCE_SYNC_TOKEN"],
                values["CAUCE_CA_KEY"]}) == 3, (
        "the three credentials must be distinct by construction, not by luck")
    # An Ed25519 seed is 32 bytes; hex is the encoding that cannot make that ambiguous.
    assert len(values["CAUCE_CA_KEY"]) == 64
    int(values["CAUCE_CA_KEY"], 16)  # raises if it is not hex


def test_generation_is_not_repeatable():
    assert init_env.generate() != init_env.generate(), (
        "the generator returned the same secrets twice, so it is not random")


def test_a_generated_file_passes_its_own_check(tmp_path):
    path = tmp_path / ".env"
    path.write_text(init_env.render(init_env.generate()), encoding="utf-8")
    assert init_env.check(path) == []


# --------------------------------------------------------------------- refusing


def test_it_will_not_overwrite_a_live_file(tmp_path):
    """Rotating the admin token, the node secret and the CA seed locks every node out.

    The file is not just configuration; it is the only copy of three credentials that
    provisioned nodes depend on. A second run that replaced them silently would be a worse
    outcome than the failure it was trying to prevent.
    """
    path = tmp_path / ".env"
    path.write_text(init_env.render(init_env.generate()), encoding="utf-8")
    before = path.read_text(encoding="utf-8")
    assert init_env.write(path, init_env.generate(), force=False) == 1
    assert path.read_text(encoding="utf-8") == before, "the file was modified anyway"


def test_force_does_overwrite_because_that_is_what_it_means(tmp_path):
    path = tmp_path / ".env"
    path.write_text(init_env.render(init_env.generate()), encoding="utf-8")
    before = path.read_text(encoding="utf-8")
    assert init_env.write(path, init_env.generate(), force=True) == 0
    assert path.read_text(encoding="utf-8") != before


def test_a_missing_file_is_reported_rather_than_passing(tmp_path):
    problems = init_env.check(tmp_path / "nope.env")
    assert problems and "does not exist" in problems[0]


# --------------------------------------------------------------------- the failure modes


def test_an_empty_admin_token_is_named_for_what_it_causes(tmp_path):
    """It is not a validation error; it is the designed behaviour.

    The message has to say so, because the operator's conclusion would otherwise be that the
    software is broken when in fact nothing is configured.
    """
    path = write_env(tmp_path / ".env", CAUCE_API_TOKEN="")
    problems = " ".join(init_env.check(path))
    assert "503" in problems and "admin_api_not_configured" in problems


def test_the_ca_seed_equal_to_the_admin_token_is_the_one_that_matters(tmp_path):
    """A certificate authority whose key is the admin credential is one compromise from
    minting a certificate for any node, which defeats the whole identity design."""
    path = write_env(tmp_path / ".env", CAUCE_API_TOKEN="shared", CAUCE_CA_KEY="shared")
    problems = " ".join(init_env.check(path))
    assert "same value" in problems and "certificate" in problems


def test_the_sync_token_equal_to_the_admin_token_is_refused(tmp_path):
    """The sync token goes into every node's flash. Equal to the admin token, a lost node
    hands over administration."""
    path = write_env(tmp_path / ".env", CAUCE_API_TOKEN="shared", CAUCE_SYNC_TOKEN="shared")
    problems = " ".join(init_env.check(path))
    assert "same value" in problems


def test_a_quoted_value_is_caught(tmp_path):
    """The most likely copy-paste fault, and the one that produces a 401 pointing at the
    client instead of at the configuration."""
    path = write_env(tmp_path / ".env", CAUCE_API_TOKEN='"quoted"')
    problems = " ".join(init_env.check(path))
    assert "quoted" in problems


def test_a_ca_seed_of_the_wrong_length_is_caught(tmp_path):
    path = write_env(tmp_path / ".env", CAUCE_CA_KEY="deadbeef")
    problems = " ".join(init_env.check(path))
    assert "64 hex characters" in problems


def test_requiring_auth_with_no_sync_token_is_caught(tmp_path):
    """Every batch refused at ingest, which reads as a firmware fault rather than as a
    configuration one."""
    path = write_env(tmp_path / ".env", CAUCE_SYNC_TOKEN="", CAUCE_SYNC_REQUIRE_AUTH="1")
    problems = " ".join(init_env.check(path))
    assert "ingest" in problems


def test_a_wildcard_bind_is_called_out(tmp_path):
    """The compose file ships loopback-only on purpose."""
    path = write_env(tmp_path / ".env", CAUCE_BIND="0.0.0.0:8000:8000")
    problems = " ".join(init_env.check(path))
    assert "CAUCE_BIND" in problems and "network" in problems


def test_loopback_bind_is_accepted(tmp_path):
    assert init_env.check(write_env(tmp_path / ".env", CAUCE_BIND="127.0.0.1:8000:8000")) == []


def test_comments_and_blank_lines_are_ignored(tmp_path):
    path = tmp_path / ".env"
    path.write_text(
        "# a comment\n\n  # indented comment\n"
        "CAUCE_API_TOKEN=a\nCAUCE_SYNC_TOKEN=b\n"
        f"CAUCE_CA_KEY={GOOD_CA}\n\n",
        encoding="utf-8")
    assert init_env.check(path) == []


def test_the_rendered_file_documents_what_rotating_each_one_breaks(tmp_path):
    """The file has to be readable months later by someone who did not generate it."""
    text = init_env.render(init_env.generate())
    assert "CAUCE_CA_KEY" in text
    assert "invalidates every certificate" in text
    assert "re-provisioning every node" in text
    assert "plaintext" in text
