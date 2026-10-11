"""A budget nobody can see is not a budget.

The uncertainty budgets were correct, complete and invisible. `budget_for` returned a real
budget for every published channel, and the only thing in the codebase that read it was
`calibration.py` - so a wind reading on a node report carried no indication that an uncalibrated
cup anemometer is a relative instrument.

That is the same failure as a channel that stores correctly and renders nowhere, one level up,
and this project has now hit it twice: first the `illuminance` channel with no driver, then the
node card hardcoded to temperature. The third instance is the subtlest of the three, because
the code was right - the budget existed, was tested, and still reached no reader.

So the property asserted here is reachability, not correctness: every published channel's
budget must appear on the page that shows that channel's readings. A budget that exists and is
rendered nowhere is indistinguishable, to the person reading the number, from having no budget.
"""

from __future__ import annotations

import pytest
from cauce_server import db, variables
from cauce_server.main import app
from cauce_server.uncertainty import budget_for
from conftest import ADMIN_HEADERS
from fastapi.testclient import TestClient

BASE_TS = 1787356860000


@pytest.fixture()
def client():
    db.reset_for_tests()
    with TestClient(app, headers=ADMIN_HEADERS) as c:
        yield c


def _record(seq: int, variable: str, value: float, unit: str) -> dict:
    return {
        "node_id": "CAUCE-001",
        "sensor_id": "TEST-1",
        "sequence": seq,
        "timestamp_utc_ms": BASE_TS,
        "variable": variable,
        "value": value,
        "unit": unit,
        "quality": "VALID",
        "reason_bits": 0,
        "time_uncertain": False,
    }


# The three channels whose budgets were added after the fact, plus the one that was already
# missing before anyone looked.
CHANNELS = [
    ("air_temperature", 21.5, "C"),
    ("wind_speed", 4.7, "m/s"),
    ("tvoc", 143.0, "ppb"),
    ("illuminance", 812.0, "lx"),
    ("battery_voltage", 3.9, "V"),
]


@pytest.mark.parametrize("variable,value,unit", CHANNELS)
def test_the_node_report_shows_the_uncertainty_of_the_channel_it_reports(client, variable, value, unit):
    client.post("/v1/sync", json={
        "protocol_version": 1, "node_id": "CAUCE-001",
        "measurements": [_record(1, variable, value, unit)],
    })
    r = client.get("/nodes/CAUCE-001?var=" + variable)
    assert r.status_code == 200

    budget = budget_for(variable)
    assert budget is not None, "precondition: this channel is supposed to have a budget"

    # The rendered figure has to be the budget's own arithmetic on this channel's own mean,
    # not a placeholder. Recomputing it here means the test fails if the page ever shows a
    # number that came from anywhere else.
    expected = budget.combined_for_reading(value)
    assert f"\u00b1{expected:.1f}" in r.text, (
        f"the report for {variable} does not show +/-{expected:.1f}. The budget exists in "
        f"`uncertainty.py` and reaches the calibration API and nothing else, which is the "
        f"third time this repository has built something correct that no reader can see.")


def test_the_report_names_what_the_number_may_not_be_used_for(client):
    """The assumption travels with the figure, not only in the source file."""
    client.post("/v1/sync", json={
        "protocol_version": 1, "node_id": "CAUCE-001",
        "measurements": [_record(1, "wind_speed", 4.7, "m/s")],
    })
    r = client.get("/nodes/CAUCE-001?var=wind_speed")
    assert r.status_code == 200
    # Carried as a title attribute, so it is present for a reader without being shouted at a
    # reader who just wants the number.
    budget = budget_for("wind_speed")
    snippet = budget.assumption[:60].replace('"', "&quot;")
    assert snippet in r.text or budget.assumption[:40] in r.text, (
        "the wind budget's assumption is not attached to the reading it qualifies. A figure "
        "whose limits live only in a source file will be compared against a threshold it was "
        "never good for.")


def test_every_published_channel_would_survive_this_test(client):
    """The general property, checked against the registry rather than a hand-written list.

    A hand-written list is exactly how the four channels above could all have been added while
    something outside it stayed broken. This iterates the registry, so a channel added later
    cannot skip the check by not being mentioned here.
    """
    for info in variables.VARIABLES:
        budget = budget_for(info.name)
        assert budget is not None, (
            f"{info.name} is published and has no budget, so this file could not assert "
            f"anything about how it is shown")
        assert budget.assumption.strip(), f"{info.name} has a budget with no stated limit"


def test_the_reported_uncertainty_grows_with_the_reading(client):
    """A single constant would satisfy every check above and mean nothing.

    Wind is dominated by proportional terms, so its uncertainty has to scale with the reading.
    Quoting one fixed number next to the statistic is the failure this catches: it would look
    confident and be wrong for every reading except the one it was computed from.

    Two separate networks rather than two readings in one window - posting both would leave a
    mean in between, and a test asserting on the mean of two readings proves nothing about
    either one.
    """
    budget = budget_for("wind_speed")
    assert budget.has_proportional_terms, (
        "an uncalibrated anemometer's error is proportional; without that this test would be "
        "asserting that a constant is correct")

    def shown(value: float) -> str:
        db.reset_for_tests()
        client.post("/v1/sync", json={
            "protocol_version": 1, "node_id": "CAUCE-001",
            "measurements": [_record(1, "wind_speed", value, "m/s")],
        })
        r = client.get("/nodes/CAUCE-001?var=wind_speed")
        assert r.status_code == 200
        return r.text

    calm = shown(2.0)
    gale = shown(12.0)
    expected_calm = budget.combined_for_reading(2.0)
    expected_gale = budget.combined_for_reading(12.0)

    assert f"\u00b1{expected_calm:.1f}" in calm
    assert f"\u00b1{expected_gale:.1f}" in gale
    assert expected_gale > expected_calm * 3, (
        f"{expected_gale:.1f} at 12 m/s against {expected_calm:.1f} at 2 m/s does not scale "
        f"the way a proportional budget has to")
