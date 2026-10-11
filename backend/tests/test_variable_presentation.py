"""A sensor that reports a variable must be visible, not merely stored.

Storage has been generic for a long time: a measurement is a `(node, variable, value, unit)`
row, so a node could report wind speed, the central would accept it, the value would be in the
database, and the dashboard would show nothing. Twenty-three references to `air_temperature`
in `dashboard.py` is what that costs - the same outcome as never building the feature, except
it looks like it works.

These tests are about the second half. Each one posts a variable the card, the map legend or the
node report did not know about before, and asserts it reaches the page. That is the property
that was missing, so it is the one that has to hold.

`variables.py` is the registry that makes it hold: one declaration per variable, and the legend,
the default selection, the colour and the display name all read from it rather than from four
places that have to be found by hand.
"""

from __future__ import annotations

import pytest
from cauce_server import db, variables
from cauce_server.main import app
from conftest import ADMIN_HEADERS
from fastapi.testclient import TestClient


@pytest.fixture()
def client():
    db.reset_for_tests()
    with TestClient(app, headers=ADMIN_HEADERS) as c:
        yield c


# `make_record` and `sync_payload` live in test_api.py; importing across test modules by
# package path does not work here, so the two helpers are restated rather than shared, which
# keeps this file readable on its own.
BASE_TS = 1787356800000


def make_record(seq: int, ts_ms: int, value: float = 21.0) -> dict:
    return {
        "node_id": "CAUCE-001",
        "sensor_id": "TEST-1",
        "sequence": seq,
        "timestamp_utc_ms": ts_ms,
        "variable": "air_temperature",
        "value": value,
        "unit": "C",
        "quality": "VALID",
        "reason_bits": 0,
        "time_uncertain": False,
    }


def sync_payload(records: list[dict], node_id: str = "CAUCE-001") -> dict:
    for r in records:
        r["node_id"] = node_id
    return {"protocol_version": 1, "node_id": node_id, "measurements": records}


NEW_CHANNELS = ["wind_speed", "illuminance", "tvoc"]

UNITS = {"wind_speed": "m/s", "illuminance": "lx", "tvoc": "ppb"}
VALUES = {"wind_speed": 4.7, "illuminance": 812.0, "tvoc": 143.0}


def _post_channel(client, variable: str) -> None:
    rec = make_record(2, BASE_TS, VALUES[variable])
    rec["variable"] = variable
    rec["unit"] = UNITS[variable]
    client.post("/v1/sync", json=sync_payload([make_record(1, BASE_TS, 21.5), rec]))


@pytest.mark.parametrize("variable", NEW_CHANNELS)
def test_a_node_card_shows_a_channel_it_did_not_used_to_know(client, variable):
    _post_channel(client, variable)
    r = client.get("/")
    assert r.status_code == 200
    # The value, and the name that tells a reader what they are looking at.
    assert f"{VALUES[variable]:.1f}" in r.text
    assert variables.label_for(variable, "es") in r.text


@pytest.mark.parametrize("variable", NEW_CHANNELS)
def test_the_node_report_draws_a_series_for_a_new_channel(client, variable):
    _post_channel(client, variable)
    r = client.get("/nodes/CAUCE-001")
    assert r.status_code == 200
    assert f"{VALUES[variable]:.1f}" in r.text


def test_a_node_with_no_temperature_still_leads_with_something_real(client):
    """The card used to render "— °C · — %RH" as its whole reading for such a node.

    That is the failure this file exists for: a node with a working sensor and nothing to show
    for it. It must lead with a variable it actually has.
    """
    rec = make_record(1, BASE_TS, 4.7)
    rec["variable"] = "wind_speed"
    rec["unit"] = "m/s"
    client.post("/v1/sync", json=sync_payload([rec]))
    r = client.get("/")
    assert r.status_code == 200
    assert "4.7" in r.text
    # Not the old empty temperature readout.
    assert "— <span class=\"u\">°C</span>" not in r.text


def test_the_registry_is_the_single_place_a_channel_is_declared():
    names = [v.name for v in variables.VARIABLES]
    assert len(names) == len(set(names)), "a variable is declared twice in the registry"
    assert names[0] == "air_temperature", (
        "air_temperature must stay first: the legend and the default selection both use this "
        "order, and moving it changes what every node report leads with")
    for info in variables.VARIABLES:
        assert variables.color_for(info.name) == info.color
        assert variables.label_for(info.name, "en") == info.label
        assert variables.label_for(info.name, "es") == info.label_es
        assert variables.unit_for(info.name) == info.unit


def test_every_channel_the_firmware_can_emit_is_in_the_registry():
    """A variable the firmware reports but the registry omits renders with no legend entry."""
    import re
    from pathlib import Path

    source = (
        Path(__file__).resolve().parents[2] / "firmware/lib/cauce_core/src/Types.cpp"
    ).read_text(encoding="utf-8")

    # Only the variableName() switch. Scanning every `return "..."` in the file also picks up
    # the unit strings, and "lx" and "ppb" are not channels - a check that demands they appear
    # in the registry would be demanding nonsense.
    body = source[source.index("const char* variableName"):source.index("const char* variableUnit")]
    emitted = set(re.findall(r'return\s+"([a-z_0-9]+)";', body)) - {"unknown"}
    missing = emitted - set(variables.BY_NAME)
    assert not missing, (
        f"the firmware reports {sorted(missing)} but the presentation registry has no entry "
        f"for them; they would store correctly and render with no label, colour or legend row")


def test_a_channel_the_registry_has_never_seen_still_renders():
    """Not a failure case to suppress - a node may carry something newer than this table."""
    assert variables.color_for("something_new") == variables.FALLBACK_COLOR
    assert variables.label_for("something_new") == "something new"
    entries = variables.legend(["air_temperature", "something_new"])
    assert [e["variable"] for e in entries] == ["air_temperature", "something_new"]


def test_the_legend_lists_only_what_a_node_reports():
    entries = variables.legend(["air_temperature", "wind_speed"])
    assert [e["variable"] for e in entries] == ["air_temperature", "wind_speed"]
    assert variables.legend([]) == []


def test_the_default_selection_prefers_what_a_reader_looks_for_first():
    assert variables.default_selection(
        ["tvoc", "air_temperature", "wind_speed"]) == ["air_temperature"]
    # A node with none of the usual channels still reports what it has, rather than nothing.
    assert variables.default_selection(["wind_speed", "tvoc"]) == ["wind_speed", "tvoc"]


def test_battery_voltage_is_not_offered_as_an_environmental_reading():
    """It is a property of the hardware, not of the air, and a map keyed on it would mislead."""
    assert variables.is_environmental("battery_voltage") is False
    assert variables.is_environmental("wind_speed") is True


def test_no_co2_channel_exists_anywhere_in_the_registry():
    """The CCS811's eCO2 is an estimate derived from VOC, not a measurement of CO2."""
    names = {v.name for v in variables.VARIABLES}
    assert "co2" not in names
    assert "eco2" not in names
    assert "tvoc" in names, (
        "the part's real measurement - total VOC - is what should be reported instead")
