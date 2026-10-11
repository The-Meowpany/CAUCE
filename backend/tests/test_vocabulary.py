"""One concept, one term - and `channel` is not it.

This repository calls a measured quantity a *variable*, and it has done so everywhere before
today: the firmware's `Variable` enum and `variableName()`, the `variable` column in
`measurements`, the CSV header, and `DATA_MODEL.md`.

Then the presentation registry was written calling them channels, which collided with
`alert_rules.channel` - and that column has nothing to do with measurement at all. It is the
delivery route, validated against `CHANNELS = ("webhook", "telegram")`. A rule can be a webhook
alert or a telegram alert; it is not an "air_temperature channel". Reading one where the other
was meant produces a query that looks plausible and answers the wrong question.

The cost of the collision is not a crash. It is a reader - including the one who wrote the
registry - reaching for `rule["channel"]` when they mean a measurement, or assuming an alert can
be raised on TVOC because the word appears next to it in a file. Both are quiet.

So the term is asserted rather than agreed. A test about vocabulary is unusual, and this one
exists because the vocabulary has already drifted once inside a single session, in code that
passed every other check.
"""

from __future__ import annotations

import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
REGISTRY = REPO / "backend/cauce_server/variables.py"
ALERTS = REPO / "backend/cauce_server/alerts.py"

# Words that legitimately mean something else here and must not be confused with a variable.
UNAFFECTED = {
    # delivery routes for an alert
    "webhook", "telegram", "target",
}


def test_the_registry_does_not_call_a_measurement_a_channel():
    """Prose only. The code uses `variable` in every identifier already; this is the comments."""
    text = REGISTRY.read_text(encoding="utf-8")
    offenders = [
        line.strip()
        for line in text.splitlines()
        if re.search(r"\bchannels?\b", line, re.I)
    ]
    assert not offenders, (
        "the presentation registry calls measured quantities 'channels', which is what "
        "alert_rules.channel means something else by. One concept, one term:\n  "
        + "\n  ".join(offenders))


def test_alert_channel_is_delivery_and_nothing_else():
    """The other half of the collision: confirm what `channel` legitimately means."""
    text = ALERTS.read_text(encoding="utf-8")
    match = re.search(r'CHANNELS\s*=\s*\(([^)]*)\)', text)
    assert match, "the delivery-channel list moved, which is the other half of this collision"
    values = set(re.findall(r'"([^"]+)"', match.group(1)))
    assert values == {"webhook", "telegram"}, (
        f"alert delivery channels are {values}; if a measurement variable ever appears here, "
        f"the two meanings have collided in the worst possible place")

    # And the evaluation must not be reading a measurement out of that column.
    body = text[text.index("def evaluate_heat_rules"):text.index("def evaluate_stale_rules")]
    assert "rule[\"channel\"]" not in body and "rule['channel']" not in body, (
        "heat evaluation is reading the delivery column as if it were a measurement")


def test_the_whole_repository_still_calls_them_variables():
    """The rest of the codebase never adopted the second word, and should not."""
    checks = {
        "firmware/lib/cauce_core/include/cauce/core/Types.h": "enum class Variable",
        "backend/cauce_server/db.py": "variable TEXT",
    }
    for rel, needle in checks.items():
        assert needle in (REPO / rel).read_text(encoding="utf-8"), (
            f"{rel} no longer declares the concept as {needle!r}; the registry's terminology "
            f"was taken from this and should be taken from it again")


def test_the_registry_and_the_database_agree_on_every_name():
    """The registry is the single declaration, so a name in one and not the other is a bug."""
    schema = (REPO / "backend/cauce_server/db.py").read_text(encoding="utf-8")
    names = re.findall(r"def variable_name|variable\s*=\s*[\"']([a-z_]+)[\"']", schema)
    if not names:
        # The database stores the variable name as data rather than constraining it, which is
        # the design that let `illuminance` sit in the model with no driver. Nothing to compare
        # against, and that absence is itself the documented reason the registry has to exist.
        return
    unknown = set(names) - set(re.findall(r'VariableInfo\("([a-z_]+)"', REGISTRY.read_text(
        encoding="utf-8")))
    assert not unknown, f"the database mentions {unknown} and the registry does not"


def test_no_identifier_in_the_registry_collides_with_a_delivery_channel():
    """A last pass on names, since identifiers cannot be caught by reading prose."""
    text = REGISTRY.read_text(encoding="utf-8")
    for name in UNAFFECTED:
        assert not re.search(rf"\b(def |class )?{name}\b\s*=", text), (
            f"the registry defines something called {name!r}, which is an alert delivery "
            f"channel in this codebase")
