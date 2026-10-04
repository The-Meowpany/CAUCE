"""The uncertainty budgets, and the arithmetic they are combined with.

The point of a budget rather than a single number is that the components answer
different questions, so the tests check the combination rule and the dominant-term
selection rather than only that a number came out.
"""

from __future__ import annotations

import math
import os

import pytest
from fastapi.testclient import TestClient

os.environ["CAUCE_DB_PATH"] = "./data/test_budget.sqlite"

from cauce_server import db  # noqa: E402
from cauce_server.calibration import (  # noqa: E402
    budget_summary,
    calibration_summary,
)
from cauce_server.main import app  # noqa: E402
from cauce_server.uncertainty import (  # noqa: E402
    BUDGETS,
    budget_for,
    variables_with_budgets,
)
from conftest import ADMIN_HEADERS  # noqa: E402

ADMIN = {"Authorization": "Bearer admin-token"}


@pytest.fixture()
def client():
    db.reset_for_tests()
    with TestClient(app, headers=ADMIN_HEADERS) as c:
        yield c


# --- the combination rule --------------------------------------------------

def test_absolute_terms_combine_in_quadrature():
    budget = budget_for("air_temperature")
    assert budget is not None
    expected = math.sqrt(0.5 ** 2 + 0.3 ** 2 + 0.01 ** 2)
    assert budget.combined_absolute() == pytest.approx(expected)


def test_quadrature_is_not_a_sum():
    # The distinction that matters: three 0.5 errors are not 1.5.
    budget = budget_for("air_temperature")
    assert budget.combined_absolute() < sum(budget.absolute_components().values())


def test_a_proportional_term_scales_with_the_reading():
    humidity = budget_for("relative_humidity")
    assert humidity.has_proportional_terms
    # 3% of 80 is 2.4, which is above the 2.0 constant term and so dominates; at
    # 10%RH the same 3% is only 0.3 and the constant dominates. The ratio between
    # the two totals is nowhere near 8x, and asserting it was would be asserting a
    # model that is wrong: the constant terms do not scale with the reading.
    dry = humidity.combined_for_reading(10.0)
    wet = humidity.combined_for_reading(80.0)
    assert wet > dry
    assert wet > 2.4
    assert humidity.dominant_term(80.0) == "sensor_datasheet_pct_of_reading"
    assert humidity.dominant_term(10.0) == "co_location_spread"


def test_a_constant_budget_does_not_depend_on_the_reading():
    temperature = budget_for("air_temperature")
    assert temperature.combined_for_reading(5.0) == pytest.approx(
        temperature.combined_for_reading(45.0))


def test_the_dominant_term_is_visible():
    # The reason the components are kept. An operator needs to know whether to
    # replace the sensor or re-do the comparison, and a single averaged figure
    # cannot tell them.
    temperature = budget_for("air_temperature")
    assert temperature.dominant_term() == "sensor_datasheet"
    assert temperature.dominant_term() == max(
        temperature.components.items(), key=lambda kv: kv[1])[0]


def test_the_pressure_budget_names_the_term_that_calibration_cannot_fix():
    pressure = budget_for("pressure")
    assert "altitude_offset" in pressure.components
    assert "altitude" in pressure.assumption


# --- the numbers are assumptions, and say so -------------------------------

def test_every_budget_declares_what_it_assumes():
    for variable in variables_with_budgets():
        budget = budget_for(variable)
        assert budget.assumption, variable
        assert len(budget.assumption) > 40, variable
        # Nothing here is traceable yet, and the payload has to say so rather than
        # letting an absence be read as a claim.
        assert budget.as_dict()["traceable"] is False


def test_every_budget_names_its_unit():
    for variable in variables_with_budgets():
        assert budget_for(variable).unit, variable


def test_the_variables_this_project_measures_all_have_budgets():
    for variable in ("air_temperature", "relative_humidity", "pressure"):
        assert budget_for(variable) is not None


# --- absence is not zero --------------------------------------------------

def test_an_unknown_variable_has_no_budget_rather_than_a_zero_one():
    # "We have no numbers for this quantity" and "this quantity is exact" are
    # different, and conflating them is how a report quotes a precision nobody
    # established.
    assert budget_for("soil_moisture") is None
    assert budget_summary("soil_moisture") is None


def test_a_budget_dict_names_the_variable_and_its_components():
    payload = budget_summary("air_temperature", 21.0)
    assert payload["variable"] == "air_temperature"
    assert payload["unit"] == "degC"
    assert payload["components"]
    assert payload["dominant_term"]
    assert payload["combined_for_reading"] == pytest.approx(
        payload["combined_absolute"])


def test_reading_none_yields_no_per_reading_figure():
    # Better an explicit absence than a figure computed from a reading of zero,
    # which for a proportional term is a real and misleading number.
    payload = budget_summary("relative_humidity")
    assert payload["combined_for_reading"] is None
    assert payload["combined_absolute"] is not None


# --- it reaches a calibration summary --------------------------------------

def test_a_calibration_summary_carries_the_budget(client):
    # Calibrations hang off a site, and the node is attached to that site, so the
    # summary resolves a variable from the record rather than the node.
    assert client.post("/v1/sites", json={"site_id": "s1"}).status_code in (200, 201)
    response = client.put("/v1/sites/s1/calibration", headers=ADMIN, json={
        "variable": "air_temperature",
        "scale": 1.0,
        "offset": -0.25,
        "method": "co-location-relative",
        "uncertainty": 0.6,
        "uncertainty_kind": "co_location_spread",
    })
    assert response.status_code == 200, response.text
    # Read the stored record back rather than summarising the write response: the
    # two are not the same shape, and the stored one is what a report is built from.
    stored = client.get("/v1/sites/s1/calibration").json()["calibrations"][0]
    summary = calibration_summary(stored)
    assert summary["budget"] is not None
    assert summary["budget"]["variable"] == "air_temperature"
    # A summary that quotes 0.6 without saying what to compare it to is how an
    # operator reads a co-location spread as a sensor tolerance.
    assert summary["uncertainty"] == pytest.approx(0.6)
    assert summary["budget"]["dominant_term"] == "sensor_datasheet"


def test_budgets_are_consistent_with_their_own_constants():
    for variable, raw in BUDGETS.items():
        budget = budget_for(variable)
        assert set(budget.components) == set(raw["components"]), variable
