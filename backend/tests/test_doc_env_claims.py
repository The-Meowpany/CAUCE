"""A documented environment variable must be one the code actually reads.

`docs/en/API.md` and `docs/es/API.md` both carried a curl example that authenticated with
`$CAUCE_TOKEN`. That name exists nowhere else in the repository - not in config.py, not in the
provisioner, not in the deployment docs - and the two names that do exist are
`CAUCE_API_TOKEN` and `CAUCE_SYNC_TOKEN`. A reader who had followed any other document in the
repository would have `CAUCE_API_TOKEN` set, pasted the command, and sent an empty Bearer
header, because `$CAUCE_TOKEN` expands to nothing.

The node's own admin token is a different secret again: it lives in the device config as
`adminTokenSha256` and is never read from the environment. The example now says so.

This test is deliberately about environment variables only. It came out of an audit that also
produced two false positives - documented node routes were compared against the central's
routes, and `/api/v1/config` and `/api/v1/diagnostics` were reported missing when both are
served by the firmware's ApiRouter. Both APIs are live at once under different prefixes, and a
check that conflates them is worse than no check.
"""

from __future__ import annotations

import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

# Environment names the source reads. Derived from the code rather than listed, so a variable
# added to config.py is automatically accepted without touching this file.
_SOURCE_SUFFIXES = {".py", ".cpp", ".h", ".hpp", ".ps1", ".ini", ".toml", ".yml", ".yaml"}


def _markdown_docs() -> list[Path]:
    return [
        p
        for p in REPO.rglob("*.md")
        if ".git" not in p.parts and ".pio" not in p.parts and "node_modules" not in p.parts
    ]


def _variables_read_by_code() -> set[str]:
    found: set[str] = set()
    for path in REPO.rglob("*"):
        if not path.is_file() or path.suffix not in _SOURCE_SUFFIXES:
            continue
        if ".git" in path.parts or ".pio" in path.parts or "__pycache__" in path.parts:
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        found.update(re.findall(r"CAUCE_[A-Z0-9_]+", text))
    return found


def test_documented_environment_variables_exist_in_the_code():
    """Every `CAUCE_*` a document names must be one the source reads."""
    known = _variables_read_by_code()
    unknown: list[tuple[str, str]] = []
    for doc in _markdown_docs():
        text = doc.read_text(encoding="utf-8", errors="replace")
        for name in re.findall(r"\bCAUCE_[A-Z0-9_]+\b", text):
            if name not in known:
                unknown.append((doc.relative_to(REPO).as_posix(), name))
    assert not unknown, (
        "these documents name environment variables the code never reads:\n"
        + "\n".join(f"  {doc}: {name}" for doc, name in sorted(set(unknown)))
        + "\nEither the variable was renamed and the documents did not follow, or the example "
          "invents one. A reader who follows it sends an empty secret.")


def test_the_two_token_variables_keep_their_distinct_meanings():
    """`CAUCE_API_TOKEN` and `CAUCE_SYNC_TOKEN` guard different services.

    Merging them is a security change and must be deliberate; this is here so that a merge
    cannot happen by a search-and-replace across the documentation.
    """
    known = _variables_read_by_code()
    assert "CAUCE_API_TOKEN" in known
    assert "CAUCE_SYNC_TOKEN" in known


def test_the_node_api_doc_says_which_token_it_wants():
    """The node's admin token is not the central's, and the example has to say so."""
    for lang in ("en", "es"):
        text = (REPO / "docs" / lang / "API.md").read_text(encoding="utf-8")
        assert "$NODE_ADMIN_TOKEN" in text, (
            f"docs/{lang}/API.md lost the explicit placeholder for the node's own admin token")
        assert "adminTokenSha256" in text, (
            f"docs/{lang}/API.md must name where the node stores the token hash, so a reader "
            f"can tell which secret the curl example is asking for")


def test_both_api_documents_carry_the_same_examples():
    """The two API documents are mirrors; an example present in one and not the other drifts."""
    en = (REPO / "docs/en/API.md").read_text(encoding="utf-8")
    es = (REPO / "docs/es/API.md").read_text(encoding="utf-8")
    for pattern, label in (
        (r"http://192\.168\.4\.1/api/v1/status", "status example"),
        (r"http://192\.168\.4\.1/api/v1/export", "export example"),
        (r"http://192\.168\.4\.1/api/v1/config", "config example"),
    ):
        assert re.search(pattern, en), f"the English document lost its {label}"
        assert re.search(pattern, es), f"the Spanish document lost its {label}"
