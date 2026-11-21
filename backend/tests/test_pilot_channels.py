"""The pilot station suite, checked across the firmware/backend boundary.

A variable is a number in the firmware enum, a name in the LoRa decoder, an entry in the
presentation registry and an uncertainty budget. Those four were kept in step by hand, and
they drifted: the decoder never learned wind_speed or tvoc, so a LoRa record of either was
stored as "unknown". These tests read the firmware source so the next drift fails here.
"""

from __future__ import annotations

import re
from pathlib import Path

import pytest
from cauce_server import lora_frames, variables
from cauce_server.uncertainty import budget_for

TYPES_H = Path(__file__).resolve().parents[2] / "firmware/lib/cauce_core/include/cauce/core/Types.h"
TYPES_CPP = Path(__file__).resolve().parents[2] / "firmware/lib/cauce_core/src/Types.cpp"


def _firmware_enum() -> dict[str, int]:
    source = TYPES_H.read_text(encoding="utf-8")
    body = source[source.index("enum class Variable"):source.index("};")]
    return {name: int(value) for name, value in re.findall(r"(\w+)\s*=\s*(\d+),", body)
            if name != "Unknown"}


def _firmware_names() -> dict[str, str]:
    source = TYPES_CPP.read_text(encoding="utf-8")
    return dict(re.findall(r'\{Variable::(\w+),\s*"([a-z_0-9]+)"', source))


def test_the_lora_decoder_knows_every_firmware_variable_by_the_same_number():
    enum = _firmware_enum()
    names = _firmware_names()
    assert len(enum) == len(names) == 24
    for symbol, number in enum.items():
        assert lora_frames.VARIABLES.get(number) == names[symbol], (
            f"firmware sends {symbol} as {number}; the decoder reads it as "
            f"{lora_frames.VARIABLES.get(number)!r}")


@pytest.mark.parametrize("name", sorted(set(_firmware_names().values())))
def test_every_firmware_channel_has_a_label_and_a_budget(name):
    assert name in variables.BY_NAME
    assert variables.label_for(name, "es") != name.replace("_", " "), (
        f"{name} has no Spanish label; the dashboard would show the raw identifier")
    budget = budget_for(name)
    assert budget is not None and budget.components and budget.assumption.strip()


@pytest.mark.parametrize("name", ["co2", "pm2_5", "solar_irradiance", "rainfall",
                                  "soil_moisture", "lightning_strikes"])
def test_each_pilot_budget_says_what_it_is_not_comparable_with(name):
    text = budget_for(name).assumption.lower()
    assert "not" in text and ("comparable" in text or "flag" in text)


@pytest.mark.parametrize("name", ["wind_speed", "tvoc", "co2", "lightning_distance"])
def test_a_lora_record_of_a_new_channel_round_trips_with_its_name(name):
    record = lora_frames.Record(
        sequence=7, timestamp_utc_ms=1787356800000, value=12.5, variable=name,
        quality="VALID", reason_bits=0, time_uncertain=False, node_id="CAUCE-004",
        sensor_id="S-1")
    decoded = lora_frames.decode_record(lora_frames.encode_record(record))
    assert decoded.variable == name
    assert decoded.value == pytest.approx(12.5)


def test_wind_direction_is_not_offered_on_a_temperature_ramp():
    lo, hi = variables.scale_for("wind_direction")
    assert (lo, hi) == (0.0, 360.0)
