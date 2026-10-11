"""The documentation index must list the documents that actually exist.

The release gate already proves that every English document has a Spanish counterpart. That
check walks the filesystem, and it passed for two documents that were already translated - so
the Spanish index kept omitting them while the gate reported everything in parity. It verified
something adjacent to the thing that matters: that the files exist, not that the index points
at them.

Both indices are tables. A reader arrives through the table, so the table is the surface that
has to be right, and a directory listing nobody reads is not evidence of anything.

The first version of this file also found a link whose label said `..\\..\backend\tools\\pki.py`
while the href said `../backend/tools/pki.py` - one level short, so it resolved to
`docs/backend/tools/pki.py` and 404ed on a file that does exist. A link to a real file can
still be a broken link, which is why resolution is checked and not just presence.
"""

from __future__ import annotations

import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
EN_INDEX = REPO / "docs/en/DOCUMENTATION_INDEX.md"
ES_INDEX = REPO / "docs/es/DOCUMENTATION_INDEX.md"


def _indexed_targets(path: Path) -> set[str]:
    """Every distinct file each index links to, relative to the repo root."""
    targets: set[str] = set()
    for href in re.findall(r"\]\(([^)]+)\)", path.read_text(encoding="utf-8")):
        if href.startswith(("http://", "https://", "mailto:")):
            continue
        target = href.split("#", 1)[0]
        if not target.endswith(".py"):
            # Only the tool table entries vary in kind; the .md links are covered by the
            # parity check the gate already runs.
            continue
        targets.add((path.parent / target).resolve().relative_to(REPO).as_posix())
    return targets


def _table_documents(path: Path) -> set[str]:
    """The .md documents named as rows of the index table."""
    found: set[str] = set()
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.startswith("|") or "---" in line:
            continue
        for href in re.findall(r"\]\(([^)]+\.md)\)", line):
            found.add(Path(href.split("#", 1)[0]).name)
    return found


def test_both_indices_resolve_every_link_they_contain():
    """A link to a file that exists can still point one level short of it."""
    for index in (EN_INDEX, ES_INDEX):
        for href in re.findall(r"\]\(([^)]+)\)", index.read_text(encoding="utf-8")):
            if href.startswith(("http://", "https://", "mailto:")):
                continue
            target = href.split("#", 1)[0]
            if not target:
                continue
            resolved = (index.parent / target).resolve()
            assert resolved.exists(), (
                f"{index.name} links to {href!r}, which resolves to "
                f"{resolved.relative_to(REPO).as_posix() if REPO in resolved.parents or resolved == REPO else resolved} "
                f"and does not exist")


def test_the_two_indices_index_the_same_documents():
    """Parity of the tables, which is what the gate's directory walk could not see."""
    en = _table_documents(EN_INDEX)
    es = _table_documents(ES_INDEX)
    assert en == es, (
        f"the indexes disagree about which documents exist.\n"
        f"  only in EN: {sorted(en - es)}\n"
        f"  only in ES: {sorted(es - en)}")


def test_the_two_indices_link_the_same_tool():
    en = _indexed_targets(EN_INDEX)
    es = _indexed_targets(ES_INDEX)
    assert en == es, (
        f"the tool table entries disagree.\n"
        f"  only in EN: {sorted(en - es)}\n"
        f"  only in ES: {sorted(es - en)}")


def test_every_indexed_document_is_present_in_both_languages():
    """Indexed means present. A row pointing at a missing file is worse than no row."""
    for index, language in ((EN_INDEX, "en"), (ES_INDEX, "es")):
        for name in sorted(_table_documents(index)):
            assert (REPO / "docs" / language / name).exists(), (
                f"{index.name} indexes {name}, which is not in docs/{language}/")


def test_no_index_still_claims_a_document_is_english_only():
    """These were translated and the index kept saying otherwise.

    The gate's counterpart check passed while both rows still carried "English only",
    because the files it was checking did exist - the sentence was the stale part, and no
    check looked at it.
    """
    for index in (EN_INDEX, ES_INDEX):
        text = index.read_text(encoding="utf-8")
        assert "English only" not in text, (
            f"{index.name} still marks a document English-only; if it has been translated "
            f"the label is wrong, and if it has not, the translation is missing")
        assert "solo en inglés" not in text, (
            f"{index.name} still marks a document as English-only in Spanish")


def test_the_index_points_at_status_as_the_source_of_truth():
    """Every fix here is a fix to a pointer; the pointers have to still point."""
    for index, other in ((EN_INDEX, "es"), (ES_INDEX, "en")):
        assert f"../{other}/DOCUMENTATION_INDEX.md" in index.read_text(encoding="utf-8"), (
            f"{index.name} lost the link to its {other} mirror")
