"""The documented test counts must be the counts that actually run.

The README quickstart said `python -m pytest tests -q  # 540 tests` in both languages while the
suite was at 674. It survived every count correction because each correction replaced the
*current* number, and 540 had not been current for a long time - nothing ever compared the
document against a real run, so a stale figure from before the count-tracking started was
invisible to it.

Counting tests by importing them is not reliable across a C++/Python boundary, so this reads the
number the suite itself reports and requires every documented count to be one of the real ones.
A document that quotes a number no run has ever produced fails, which is the only property that
matters here: the alternative is trusting a figure someone typed.
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]


def _documented_counts() -> list[tuple[str, int]]:
    """The backend count, read off the pytest quickstart line only.

    Scoped to that one line on purpose: the same comment block quotes the firmware count on the
    `pio test` line, and matching `# <n> tests` anywhere in the file picks that up too and then
    demands it equal a Python collection. The two suites are different languages and different
    counts; conflating them is how the firmware number got rewritten to the backend's.
    """
    found: list[tuple[str, int]] = []
    for name in ("README.md", "README.es.md"):
        for line in (REPO / name).read_text(encoding="utf-8").splitlines():
            if "python -m pytest" not in line:
                continue
            match = re.search(r"#\s*(\d{3,4})", line)
            if match:
                found.append((name, int(match.group(1))))
    return found


def test_the_quickstart_counts_match_a_real_run() -> None:
    documented = _documented_counts()
    assert documented, "no documented test count was found; the check below would pass vacuously"

    proc = subprocess.run(
        [sys.executable, "-m", "pytest", "tests", "-q", "--co", "-q"],
        cwd=REPO / "backend",
        capture_output=True,
        text=True,
    )
    output = proc.stdout + proc.stderr
    # `--co -q` ends with one "<file>: <n>" line per file rather than a summary line, so the
    # total is their sum. Both that and an explicit summary are accepted because pytest's
    # format has changed across versions and neither spelling should make this fail.
    per_file = [
        int(n)
        for n in re.findall(r"^tests[/\\]\S+?\.py:\s*(\d+)\s*$", output, re.M)
    ]
    summary = re.search(r"(\d+)\s+tests? collected", output)
    if summary:
        real = int(summary.group(1))
    elif per_file:
        real = sum(per_file)
    else:
        raise AssertionError(
            "could not read a test count out of pytest collection; this check is refusing to "
            "pass rather than asserting nothing")
    assert real > 0, "pytest collection reported zero tests, which is not a plausible suite"

    stale = [(name, count) for name, count in documented if count != real]
    assert not stale, (
        f"the quickstart documents {stale} but collecting the suite yields {real}. "
        f"A reader who trusts the number is told to expect a run that will not match.")


def test_both_readmes_quote_the_same_number() -> None:
    en = dict(_documented_counts())
    es = dict(_documented_counts())
    assert set(en) == set(es) and en == es, (
        f"the two READMEs disagree about the suite size: en={en} es={es}")


def test_the_firmware_count_in_the_quickstart_is_present() -> None:
    """`pio test -e native  # 461 tests` is quoted in both languages and must not drift apart."""
    en = (REPO / "README.md").read_text(encoding="utf-8")
    es = (REPO / "README.es.md").read_text(encoding="utf-8")
    en_fw = re.search(r"pio test -e native\s+#\s*(\d+)", en)
    es_fw = re.search(r"pio test -e native\s+#\s*(\d+)", es)
    assert en_fw and es_fw, "the native test count disappeared from a quickstart"
    assert en_fw.group(1) == es_fw.group(1), (
        f"the firmware count differs between languages: en={en_fw.group(1)} "
        f"es={es_fw.group(1)}")
