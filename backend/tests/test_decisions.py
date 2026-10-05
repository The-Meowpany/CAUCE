"""The parts of the central that decide something, and had no test naming them.

An audit for functions defined and called but never asserted on found three that make
judgements a report is then built from:

- `classify_gap` picks why a gap happened, which is what separates "the node was dead" from
  "the node measured and the radio ate it". Those demand different responses from whoever
  owns the fleet.
- `is_identity` decides whether a calibration does anything at all, and every reporting path
  branches on it.
- `haversine_m` feeds the control-vs-treatment comparison, so an error in it would show up as
  a study result rather than as an error.

None of them is exotic. All three are arithmetic or a small decision tree over a database,
and all three were reachable from a report with nothing asserting their answers.
"""

from __future__ import annotations

import math
import os

import pytest

os.environ["CAUCE_DB_PATH"] = "./data/test_decisions.sqlite"

from cauce_server import db  # noqa: E402
from cauce_server.analytics import haversine_m  # noqa: E402
from cauce_server.calibration import (  # noqa: E402
    apply_value,
    is_identity,
    transform_stats,
)
from cauce_server.coverage import (  # noqa: E402
    REASON_CLOCK_UNCERTAIN,
    REASON_NO_DATA,
    REASON_NOT_DELIVERED,
    classify_gap,
)

BASE = 1_787_356_800_000
HOUR = 3_600_000


@pytest.fixture()
def conn():
    db.reset_for_tests()
    yield db.engine()
    db.reset_for_tests()


def _measure(node_id, ts, *, uncertain=0, variable="air_temperature", value=20.0):
    db.engine().execute(
        "INSERT OR IGNORE INTO nodes(node_id) VALUES (?)", (node_id,))
    db.engine().execute(
        "INSERT INTO measurements(node_id, variable, timestamp_utc_ms, value,"
        " quality, sequence, time_uncertain) VALUES (?,?,?,?,?,?,?)",
        (node_id, variable, ts, value, "VALID", (ts % 10_000_000), uncertain),
    )


def _batch(node_id, received_at):
    db.engine().execute(
        "INSERT INTO sync_batches(node_id, batch_size, received_at_utc_ms,"
        " transport) VALUES (?,?,?,?)",
        (node_id, 1, received_at, "lora"),
    )


# --- classify_gap ------------------------------------------------------

def test_a_gap_with_no_evidence_is_no_data(conn):
    """The default, and the one that must be right: nothing was sent, so the node was
    silent. Asserting the default is what stops a later edit from defaulting to something
    more comfortable."""
    _measure("CAUCE-001", BASE)
    assert classify_gap("CAUCE-001", BASE, BASE + HOUR) == REASON_NO_DATA


def test_a_gap_beside_an_uncertain_timestamp_is_clock_uncertain(conn):
    """Checked first, and deliberately.

    A node whose clock jumped produced samples with `time_uncertain` set. Saying `no_data`
    about that would tell an operator the node was dead when it was reporting fine with a bad
    clock - and the fix for those are entirely different.
    """
    _measure("CAUCE-001", BASE + 5 * 60_000, uncertain=1)
    _measure("CAUCE-001", BASE + 10 * 60_000, uncertain=1)
    assert classify_gap("CAUCE-001", BASE, BASE + HOUR) == REASON_CLOCK_UNCERTAIN


def test_a_gap_beside_a_delivered_batch_is_measured_not_delivered(conn):
    """The node measured and the uplink did not arrive. This is the one that matters for a
    LoRa pilot: the data exists on the node and was not transmitted."""
    _measure("CAUCE-001", BASE)
    _batch("CAUCE-001", BASE + 5 * 60_000)
    assert classify_gap("CAUCE-001", BASE, BASE + HOUR) == REASON_NOT_DELIVERED


def test_clock_uncertain_outranks_a_delivered_batch(conn):
    """The ordering is a decision, so it is asserted rather than assumed.

    Both signals present. Clock uncertainty wins because it invalidates the timestamps the
    other inference depends on: if when the samples were taken is unknown, whether a batch
    fell inside the gap cannot be concluded from its arrival time.
    """
    _measure("CAUCE-001", BASE + 5 * 60_000, uncertain=1)
    _batch("CAUCE-001", BASE + 5 * 60_000)
    assert classify_gap("CAUCE-001", BASE, BASE + HOUR) == REASON_CLOCK_UNCERTAIN


def test_classification_does_not_look_past_the_gap(conn):
    """Evidence outside the window is not evidence about the gap.

    A batch that arrived before the gap started says nothing about why the gap happened, and
    counting it would relabel every quiet period that follows a sync as a delivery failure.
    """
    _measure("CAUCE-001", BASE)
    _batch("CAUCE-001", BASE - HOUR)
    assert classify_gap("CAUCE-001", BASE, BASE + HOUR) == REASON_NO_DATA


def test_classification_is_per_node(conn):
    """A batch from another node proves nothing about this one's silence."""
    _measure("CAUCE-001", BASE)
    _batch("CAUCE-002", BASE + 5 * 60_000)
    assert classify_gap("CAUCE-001", BASE, BASE + HOUR) == REASON_NO_DATA


def test_an_unknown_node_is_no_data_not_an_error(conn):
    """A gap for a node with no rows at all still classifies, so a report over a stale
    node_id renders instead of raising."""
    assert classify_gap("CAUCE-NEVER-SEEN", BASE, BASE + HOUR) == REASON_NO_DATA


# --- is_identity -------------------------------------------------------

def test_the_identity_map_is_recognised(conn):
    """1.0 scale and 0 offset does nothing, and every reporting path branches on that."""
    assert is_identity(1.0, 0.0) is True
    assert is_identity(1, 0) is True


@pytest.mark.parametrize("scale,offset", [
    (1.0, 0.1),   # a shift alone
    (1.1, 0.0),   # a gain alone
    (1.0, -0.5),
    (0.999, 0.0),
])
def test_a_map_that_changes_anything_is_not_identity(scale, offset):
    """Both directions matter: a calibration that only shifts and one that only scales are
    each a real correction, and calling either one identity would report a raw number as
    though it had been calibrated."""
    assert is_identity(scale, offset) is False


def test_a_tiny_offset_is_still_a_correction(conn):
    """No threshold.

    Tempting to call 1e-9 "identity" so calibration reports stay tidy. That would make a
    real correction invisible, and invisible is exactly the failure the `calibrated` key is
    supposed to avoid - the point is that a report says whether it applied something, not
    whether the something was large.
    """
    assert is_identity(1.0, 1e-9) is False


def test_stats_are_left_alone_when_the_map_is_identity(conn):
    """The branch every report depends on, end to end."""
    stats = {"n": 10, "mean": 20.0, "stddev": 1.5}
    assert transform_stats(dict(stats), {"scale": 1.0, "offset": 0.0}) == stats


def test_a_linear_map_moves_the_mean_and_scales_the_dispersion(conn):
    """`mean` shifts by the offset, `sd` scales by the slope, and `n` does not change.

    Getting `stddev` wrong here would quietly change every standard deviation a report prints,
    which is a study result rather than a visible error.
    """
    stats = {"n": 10, "mean": 20.0, "stddev": 2.0}
    # 20 * 2 + 1 = 41. I first wrote 21 here from the offset alone and it failed, which is
    # the arithmetic being checked rather than the code being wrong - a scale of 2 is not a
    # shift.
    out = transform_stats(dict(stats), {"scale": 2.0, "offset": 1.0})
    assert out["mean"] == pytest.approx(41.0)
    assert out["stddev"] == pytest.approx(4.0)
    assert out["n"] == 10


def test_apply_value_matches_the_reported_transform(conn):
    """`apply_value` is the per-point path and `transform_stats` the aggregate one. If they
    disagree, a summary and the readings underneath it tell different stories."""
    calibration = {"scale": 1.5, "offset": -2.0}
    assert apply_value(20.0, calibration) == pytest.approx(28.0)
    assert transform_stats({"n": 1, "mean": 20.0, "stddev": 0.0},
                           calibration)["mean"] == pytest.approx(28.0)


def test_apply_value_passes_none_through(conn):
    """A missing reading must stay missing, not become 0.0.

    Substituting zero would invent a measurement at a real timestamp, and the coverage
    accounting would count it as received.
    """
    assert apply_value(None, {"scale": 2.0, "offset": 5.0}) is None


def test_apply_value_without_a_calibration_is_the_identity(conn):
    assert apply_value(20.0, None) == pytest.approx(20.0)


# --- haversine_m -------------------------------------------------------

def test_haversine_is_zero_for_the_same_point(conn):
    assert haversine_m(40.0, -3.0, 40.0, -3.0) == pytest.approx(0.0, abs=1e-6)


def test_haversine_matches_a_known_distance(conn):
    """Madrid to Barcelona, about 505 km. Pinning against a real distance rather than a
    self-consistent value is the only way this catches a units error."""
    metres = haversine_m(40.4168, -3.7038, 41.3851, 2.1734)
    assert metres == pytest.approx(505_000, rel=0.01)


def test_haversine_is_symmetric(conn):
    assert haversine_m(40.0, -3.0, 41.0, 2.0) == pytest.approx(
        haversine_m(41.0, 2.0, 40.0, -3.0))


def test_one_degree_of_latitude_is_about_111_km(conn):
    """The Earth is roughly 111 km per degree of latitude anywhere. A radius error or a
    degrees/radians slip moves this by a large factor, and a control-vs-treatment
    comparison would quietly group sites wrongly."""
    assert haversine_m(0.0, 0.0, 1.0, 0.0) == pytest.approx(111_195, rel=0.01)


def test_haversine_returns_metres_not_kilometres(conn):
    """A unit slip is the likely error and it is invisible in a comparison unless the
    expected magnitude is written down, which is why the 111 km test above exists."""
    assert haversine_m(0.0, 0.0, 0.5, 0.0) > 50_000
    assert haversine_m(0.0, 0.0, 0.5, 0.0) < 60_000


def test_haversine_across_the_antimeridian(conn):
    """Two points either side of 180 degrees are 222 km apart, not 39,875 km.

    Naive longitude subtraction makes them 358 degrees apart and the formula returns the
    distance the long way round - a finite, plausible-looking, completely wrong number that
    would group a site pair across the date line as being on opposite sides of the planet.
    """
    assert haversine_m(0.0, 179.0, 0.0, -179.0) == pytest.approx(222_390, rel=0.01)


def test_haversine_normalises_an_out_of_range_longitude(conn):
    """A CSV import can hand over 181 or -200 without complaint, and they mean 1 and 160.

    Normalising only after the subtraction would not catch these: 181 - 179 is 2, which is
    in range, so the wrap never runs. Wrapping the inputs is the part that works.
    """
    # 181 is 1. One degree of longitude at the equator, not 358 of it.
    assert haversine_m(0.0, 180.0, 0.0, 181.0) == pytest.approx(
        haversine_m(0.0, 0.0, 0.0, 1.0))
    # -200 is 160.
    assert haversine_m(0.0, -200.0, 0.0, -190.0) == pytest.approx(
        haversine_m(0.0, 160.0, 0.0, 170.0))


def test_haversine_poles(conn):
    assert math.isfinite(haversine_m(90.0, 0.0, -90.0, 0.0))
    assert haversine_m(90.0, 0.0, -90.0, 0.0) == pytest.approx(20_015_000, rel=0.02)
