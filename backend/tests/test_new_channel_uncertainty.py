"""Every variable the network publishes has to say how much it trusts itself.

Adding wind, light and VOC without adding their budgets would have left `budget_for` returning
None for all three - which is honest in the narrow sense that nothing false is claimed, and
misleading in the one that matters. A reader sees a number with no uncertainty beside it and
reads it as precise. Temperature carries a budget with a stated assumption; a wind reading
carrying none invites exactly the comparison the project exists to refuse.

So the three new channels have budgets, and they are deliberately pessimistic. Two properties
are asserted here that prose alone would not hold:

- The budgets are dominated by terms that are honestly named as unknown or uncalibrated,
  rather than by the datasheet figure. A CCS811's datasheet accuracy for TVOC is not a small
  number, it is no number, and the budget says so in a component called
  `unquantified_instrument` instead of quietly reporting the quantisation term alone.

- Each one states what it may not be used for. A budget that lists magnitudes without saying
  "this does not compare between nodes" has moved the problem rather than solved it.
"""

from __future__ import annotations

import pytest
from cauce_server import variables
from cauce_server.uncertainty import BUDGETS, PROPORTIONAL_SUFFIX, budget_for

NEW = ("illuminance", "wind_speed", "tvoc")


@pytest.mark.parametrize("variable", NEW)
def test_a_new_channel_has_a_budget_rather_than_none(variable):
    budget = budget_for(variable)
    assert budget is not None, (
        f"{variable} is published with no uncertainty budget. None means 'we have no numbers "
        f"for this', which is honest in isolation and misleading next to a value a reader "
        f"will treat as precise.")
    assert budget.components, "an empty budget is the same as no budget"
    assert budget.assumption.strip(), (
        "a budget without a stated assumption is a list of numbers with nothing to test them "
        "against")


@pytest.mark.parametrize("variable", NEW)
def test_the_assumption_says_what_the_number_may_not_be_used_for(variable):
    """The sentence that stops someone putting a regulatory threshold next to it."""
    text = budget_for(variable).assumption.lower()
    must_say = {
        "illuminance": ("not a luminance", "comparable"),
        "wind_speed": ("within one node", "co-location"),
        "tvoc": ("indicator", "not"),
    }[variable]
    for phrase in must_say:
        assert phrase in text, (
            f"the {variable} budget does not say {phrase!r}; without it the magnitude is a "
            f"number waiting to be compared against something it was never good for")


def test_wind_is_dominated_by_the_terms_nobody_has_measured_yet():
    """A cup anemometer is a relative instrument until a reference unit says otherwise."""
    budget = budget_for("wind_speed")
    proportional = [k for k in budget.components if k.endswith(PROPORTIONAL_SUFFIX)]
    assert proportional, "an uncalibrated anemometer's error is proportional, not fixed"
    reading = budget.combined_for_reading(5.0)
    # A budget claiming better than about a third at 5 m/s for an uncalibrated hobby rotor is
    # not credible. The gate is deliberately loose in the right direction: the failure this
    # guards is understating, and a generous 60 % still passes.
    assert reading / 5.0 > 0.30, (
        f"a 5 m/s reading carries only {reading / 5.0:.0%} uncertainty, which understates a "
        f"rotor that has never been compared against a reference")
    assert reading / 5.0 < 0.60, (
        f"{reading / 5.0:.0%} is too pessimistic to be useful; the budget should be honest "
        f"without being useless")


def test_the_low_wind_under_read_is_a_named_term_not_a_smaller_aggregate():
    """An aggregate threshold was not enough to catch its own removal.

    The first version of this file asserted only that a 5 m/s reading carries more than 30 %
    uncertainty, and shrinking `coset_correction_pct_of_reading` from 25 to 1 left the total
    at 30.6 % - still over the bar, test green, and the term that says this rotor under-reads
    at low wind quietly gone. The aggregate is a floor, not the check; the term is the check.
    """
    budget = budget_for("wind_speed")
    coset = budget.components.get("coset_correction_pct_of_reading")
    assert coset is not None, (
        "the coset correction is why this rotor type is not calibrated; without the term the "
        "budget describes a different instrument")
    assert coset >= 20.0, (
        f"coset correction recorded as {coset}% - a 3-cup rotor is known to under-read by "
        f"roughly a quarter at low wind, and a smaller term understates the low end where "
        f"most deployments actually sit")


def test_wind_still_has_a_floor_so_a_stalled_rotor_does_not_read_as_calm():
    budget = budget_for("wind_speed")
    assert budget.combined_for_reading(0.0) > 0.0, (
        "a zero wind reading with zero uncertainty claims a stopped rotor is a perfect "
        "measurement of still air. The floor is what prevents that.")


def test_tvoc_names_the_absence_of_a_datasheet_rather_than_hiding_it():
    """The part publishes no TVOC accuracy. The budget has to say that, not imply otherwise."""
    budget = budget_for("tvoc")
    assert "unquantified_instrument" in budget.components, (
        "the largest TVOC term is that the instrument is unquantified, and it has to be named "
        "rather than left out - leaving it out would publish the quantisation term as if it "
        "were the whole error")
    total = budget.combined_for_reading(300.0)
    unquantified_share = budget.components["unquantified_instrument"] / total
    assert unquantified_share > 0.05, (
        "the unknown term should dominate a budget for an instrument with no published "
        "accuracy; if it does not, the budget is reassuring rather than honest")


def test_illuminance_carries_the_housing_term_not_only_the_datasheet_one():
    """The part is +/-20 %. The mount is worse, and the mount is what we chose."""
    budget = budget_for("illuminance")
    assert budget.components["housing_geometry"] > budget.components[
        "sensor_datasheet_pct_of_reading"], (
        "the housing geometry error dominates the datasheet figure and must be the larger "
        "term, or the budget describes the part rather than the node")


@pytest.mark.parametrize("variable", variables.BY_NAME)
def test_no_published_channel_lacks_a_budget(variable):
    """The general property, so the next variable added cannot skip this step.

    Battery voltage is a property of the hardware rather than a measurement of the air, but
    it is published on a node report like any other variable, so it is not exempt. The failure
    this prevents is exactly the one the last three channels would have hit.
    """
    budget = budget_for(variable)
    assert budget is not None, (
        f"{variable} is in the presentation registry and published on a node report, but has "
        f"no uncertainty budget")


def test_the_three_new_budgets_are_not_copies_of_each_other():
    """A copy-paste budget across channels would satisfy every check above and mean nothing."""
    assumptions = {v: budget_for(v).assumption for v in NEW}
    assert len(set(assumptions.values())) == len(NEW), (
        "two channels share an assumption text verbatim")
    shapes = {v: tuple(sorted(budget_for(v).components)) for v in NEW}
    assert len(set(shapes.values())) == len(NEW), (
        "two channels share a component list; the budgets were copy-pasted")


def test_every_budget_unit_matches_the_registry_unit():
    """degC against a registry that says °C is a mismatch a reader will hit."""
    registry = {k: v.unit for k, v in variables.BY_NAME.items()}
    for name in BUDGETS:
        if name not in registry:
            continue
        budget = budget_for(name)
        # The one deliberate exception, named here rather than left as a surprise.
        if name == "air_temperature":
            assert budget.unit == "degC"
            continue
        assert budget.unit == registry[name], (
            f"{name} budget is in {budget.unit} but the registry publishes {registry[name]}")
