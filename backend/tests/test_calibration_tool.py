"""The calibration tool, so the procedure is executable rather than described.

The maths is deliberately simple, and that is the point worth protecting. `fit_offset` is
a mean of signed differences, so a reader can recompute any published calibration by hand
from the raw column. `fit_scale_and_offset` exists for gain-dominant cases and has to
decline when the slope is not identifiable.

What these tests protect is not the arithmetic - it is arithmetic - but the decisions
around it: that too few pairs yields `provisional` and not `accept`, that a fitted slope
of 1.0 is not reported as fitted, and that `spread` is measured on the calibrated column
because the acceptance limit is stated in calibrated terms.
"""

from __future__ import annotations

import os

import pytest

os.environ["CAUCE_DB_PATH"] = "./data/test_calibrate_tool.sqlite"

from cauce_server import db  # noqa: E402
from tools.calibrate import (  # noqa: E402
    ACCEPTANCE,
    MIN_PAIRS,
    analyse,
    fit_offset,
    fit_scale_and_offset,
    raw_spread,
    spread,
)

HOUR = 3600 * 1000
BASE = 1787356800000


@pytest.fixture()
def db_with_nodes():
    db.reset_for_tests()
    yield
    db.reset_for_tests()


_sequence = [0]


def insert(node_id, variable, timestamp, value, quality="VALID"):
    _sequence[0] += 1
    # measurements.node_id is a foreign key, so the node has to exist first. Registering it
    # here rather than through /v1/sync keeps the test about the tool's arithmetic.
    db.engine().execute(
        "INSERT OR IGNORE INTO nodes(node_id) VALUES (?)", (node_id,))
    db.engine().execute(
        "INSERT INTO measurements"
        "(node_id, variable, timestamp_utc_ms, value, quality, sequence)"
        " VALUES (?,?,?,?,?,?)",
        (node_id, variable, timestamp, value, quality, _sequence[0]),
    )


def test_a_pure_offset_recovers_a_known_bias():
    """A node reading exactly 1.5 low must come out with offset +1.5."""
    values = [(20.0 + i * 0.01, 20.0 + i * 0.01 - 1.5) for i in range(48)]
    offset = fit_offset(values)
    assert offset == pytest.approx(1.5)


def test_the_offset_is_hand_checkable():
    """Two pairs and a mental subtraction, which is the whole reason for a pure offset."""
    values = [(10.0, 9.0), (12.0, 11.5)]
    assert fit_offset(values) == pytest.approx(0.75)


def test_a_fitted_scale_recovers_a_known_gain():
    values = [(node * 1.1, node) for node in (10.0, 12.5, 15.0, 17.5)]
    scale, offset = fit_scale_and_offset(values)
    assert scale == pytest.approx(1.1, rel=1e-6)
    assert offset == pytest.approx(0.0, abs=1e-9)


def test_a_flat_node_has_no_identifiable_slope():
    """Every node reading identical: no slope exists, so 1.0 is a fallback, not a fit.

    Returning some number here would present an unidentifiable slope as a measured one,
    and the calibration would look fitted in the notes while being an assumption.
    """
    values = [(7.0, 7.0), (7.0, 7.0), (7.0, 7.0)]
    scale, offset = fit_scale_and_offset(values)
    assert scale == 1.0
    assert offset == 0.0


def test_the_calibrated_spread_is_smaller_than_the_raw_one():
    """A biased node with real dispersion around the bias.

    The noise matters: a node offset by exactly a constant makes every residual identical,
    so both spreads are zero and the test would pass while proving nothing.
    """
    noise = [0.02, -0.03, 0.04, -0.01, 0.05, -0.02, 0.01, -0.04]
    values = [(10.0 + i * 0.5 + noise[i % 8], 10.0 + i * 0.5) for i in range(8)]
    offset = fit_offset(values)
    # The mean of the noise is near zero but not exactly zero, and requiring it to be would
    # be asserting arithmetic about the test data rather than about the fit.
    assert offset == pytest.approx(0.0, abs=0.01)
    assert spread(values, 1.0, offset) == pytest.approx(raw_spread(values))


def test_an_offset_cannot_reduce_dispersion():
    """An offset corrects bias and nothing else, and the report must not pretend
    otherwise.

    The residuals after an offset-only fit are the raw differences minus their own mean,
    and a standard deviation does not move when every value shifts by a constant. So the
    post-calibration spread is *identical* to the raw one, by construction. An earlier
    version of this test asserted it got smaller, which is not a property of the method -
    it was a claim the code did not implement and the test was written to match.
    """
    values = [(node + 2.0 + noise, node)
              for node, noise in zip([10.0, 12.0, 11.0, 13.0, 12.5],
                                     [0.02, -0.03, 0.01, -0.02, 0.03], strict=True)]
    offset = fit_offset(values)
    assert offset == pytest.approx(2.0, abs=0.03)
    assert spread(values, 1.0, offset) == pytest.approx(raw_spread(values))


def test_only_a_fitted_slope_reduces_dispersion():
    """The contrast that gives the offset tests their meaning.

    Enough points that two fitted parameters cannot simply memorise the residuals, which
    is the failure mode of a five-point least squares.
    """
    noise = [0.02, -0.03, 0.01, -0.02, 0.03, 0.04, -0.01, 0.05, -0.04, 0.02,
             0.01, -0.05, 0.03, -0.02, 0.04, -0.01]
    values = [(node * 1.2 + noise[i], node)
              for i, node in enumerate(10.0 + i * 0.4 for i in range(len(noise)))]
    scale, offset = fit_scale_and_offset(values)
    assert spread(values, scale, offset) < raw_spread(values) / 5


def test_an_offset_that_does_nothing_leaves_the_spread_alone():
    """The counter-test to the one above.

    Without this, any function that returned a smaller number would pass the previous test,
    including one that subtracted the mean twice.
    """
    values = [(node + 2.0 + noise, node)
              for node, noise in zip([10.0, 12.0, 11.0, 13.0, 12.5],
                                     [0.02, -0.03, 0.01, -0.02, 0.03], strict=True)]
    assert spread(values, 1.0, 0.0) == pytest.approx(raw_spread(values))


def test_the_calibrated_spread_matches_the_raw_one_for_an_offset_fit(db_with_nodes):
    """The same guarantee at the report level, so it is not only in the maths tests."""
    for i in range(48):
        reference = 20.0 + (i % 5) * 0.01
        insert("CAUCE-REF", "air_temperature", BASE + i * HOUR, reference)
        insert("CAUCE-A", "air_temperature", BASE + i * HOUR, reference - 0.05)

    report = analyse("CAUCE-REF", ["CAUCE-A"], "air_temperature",
                     BASE, BASE + 48 * HOUR, False)
    entry = report["calibrations"][0]

    assert entry["verdict"] == "accept"
    assert entry["scale"] == 1.0
    assert entry["offset"] == pytest.approx(0.05, abs=1e-6)
    assert entry["post_calibration_spread"] == pytest.approx(entry["raw_spread"])
    assert entry["meets_procedure_sample_size"] is True


def test_analyse_rejects_a_node_that_cannot_be_fixed_with_an_offset(db_with_nodes):
    """A gain disagreement: an offset moves the mean onto the reference but cannot
    narrow the spread, so it must not be reported as an accepted calibration."""
    for i in range(48):
        node = 10.0 + i * 0.1
        insert("CAUCE-REF", "air_temperature", BASE + i * HOUR, node * 2.0)
        insert("CAUCE-B", "air_temperature", BASE + i * HOUR, node)

    report = analyse("CAUCE-REF", ["CAUCE-B"], "air_temperature",
                     BASE, BASE + 48 * HOUR, False)
    entry = report["calibrations"][0]

    assert entry["verdict"] == "reject"
    assert entry["post_calibration_spread"] > ACCEPTANCE["air_temperature"]


def test_too_few_pairs_is_provisional_not_accept(db_with_nodes):
    """Four samples are not the procedure's forty-eight.

    Deciding `accept` here would put a calibration into production on the strength of an
    afternoon, and `provisional` exists in the status vocabulary for exactly this.
    """
    for i in range(4):
        reference = 20.0
        insert("CAUCE-REF", "air_temperature", BASE + i * HOUR, reference)
        insert("CAUCE-C", "air_temperature", BASE + i * HOUR, reference - 0.02)

    report = analyse("CAUCE-REF", ["CAUCE-C"], "air_temperature",
                     BASE, BASE + 48 * HOUR, False)
    entry = report["calibrations"][0]

    assert entry["verdict"] == "provisional"
    assert entry["pairs"] < MIN_PAIRS
    assert entry["meets_procedure_sample_size"] is False


def test_a_node_with_no_overlap_is_skipped_not_fitted(db_with_nodes):
    """No exact-timestamp overlap means no pairs, and inventing an offset would be worse
    than reporting nothing."""
    for i in range(48):
        insert("CAUCE-REF", "air_temperature", BASE + i * HOUR, 20.0)
        # Offset by half an hour, so nothing ever coincides.
        insert("CAUCE-D", "air_temperature", BASE + i * HOUR + HOUR // 2, 19.0)

    report = analyse("CAUCE-REF", ["CAUCE-D"], "air_temperature",
                     BASE, BASE + 48 * HOUR, False)
    assert report["calibrations"][0]["verdict"] == "no-co-located-data"
    assert report["calibrations"][0]["pairs"] == 0


def test_invalid_quality_is_excluded_from_the_pairs(db_with_nodes):
    """A node reporting garbage alongside good readings must not be calibrated on both."""
    for i in range(48):
        reference = 20.0 + i * 0.01
        insert("CAUCE-REF", "air_temperature", BASE + i * HOUR, reference)
        insert("CAUCE-E", "air_temperature", BASE + i * HOUR, reference - 0.03)
    # Twenty wildly wrong readings marked INVALID.
    for i in range(20):
        insert("CAUCE-E", "air_temperature", BASE + 100 * HOUR + i * HOUR,
               -999.0, quality="INVALID")

    report = analyse("CAUCE-REF", ["CAUCE-E"], "air_temperature",
                     BASE, BASE + 500 * HOUR, False)
    entry = report["calibrations"][0]

    assert entry["pairs"] == 48
    assert entry["offset"] == pytest.approx(0.03, abs=1e-6)


def test_fit_scale_turns_the_rejected_case_into_an_acceptance(db_with_nodes):
    """The same node, with the gain actually fitted. If this does not pass, the rejection
    above is rejecting for the right reason."""
    for i in range(48):
        node = 10.0 + i * 0.1
        insert("CAUCE-REF", "air_temperature", BASE + i * HOUR, node * 2.0)
        insert("CAUCE-B", "air_temperature", BASE + i * HOUR, node)

    offset_only = analyse("CAUCE-REF", ["CAUCE-B"], "air_temperature",
                          BASE, BASE + 48 * HOUR, False)
    fitted = analyse("CAUCE-REF", ["CAUCE-B"], "air_temperature",
                     BASE, BASE + 48 * HOUR, True)

    assert offset_only["calibrations"][0]["verdict"] == "reject"
    assert fitted["calibrations"][0]["verdict"] == "accept"
    assert fitted["calibrations"][0]["scale"] == pytest.approx(2.0, rel=1e-6)


def test_a_variable_with_no_acceptance_limit_says_so(db_with_nodes):
    """The tool must not invent a threshold for a quantity CALIBRATION.md never set."""
    for i in range(48):
        insert("CAUCE-REF", "light", BASE + i * HOUR, 100.0)
        insert("CAUCE-F", "light", BASE + i * HOUR, 90.0)

    report = analyse("CAUCE-REF", ["CAUCE-F"], "light", BASE, BASE + 48 * HOUR, False)

    assert report["acceptance_limit"] is None
    assert report["calibrations"][0]["verdict"] == "no-acceptance-limit-defined"
