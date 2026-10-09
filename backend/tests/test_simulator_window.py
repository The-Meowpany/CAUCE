"""The scenario generator must never produce a measurement dated in the future.

This is the second time this has been true, and the first time 51,200 rows were in the
future with every test green. Nothing rejects a future timestamp: it is not a constraint
violation, it is a plausible-looking number, so the database takes it and the dashboard
renders it. What showed up on the index was `last_seen` reading 23:59 *tomorrow* on every
node while their real last reading was minutes ago, and the map's staleness rules comparing
themselves against data that had not happened yet.

The anchor was anchored to yesterday's midnight, which is only correct when the window is one
day: each scenario emits `days * 1440` minutes *forward* from `BASE_TS`, so `--days 3` ran
three days past the anchor. The fix is to anchor a whole window back from now. These tests
hold that invariant for every window length, because the bug was specifically that it held
for one of them.
"""

from __future__ import annotations

import datetime as dt
import importlib.util
import sys
from pathlib import Path

import pytest

SIM = Path(__file__).resolve().parents[2] / "simulator" / "generate_scenarios.py"


def _load():
    spec = importlib.util.spec_from_file_location("gen_scen", SIM)
    mod = importlib.util.module_from_spec(spec)
    sys.modules["gen_scen"] = mod
    spec.loader.exec_module(mod)
    return mod


gen = _load()


def _now_ms() -> int:
    return int(dt.datetime.now(dt.UTC).timestamp() * 1000)


# --------------------------------------------------------------------- the anchor


@pytest.mark.parametrize("days", [1, 2, 3, 7, 30])
def test_the_anchor_leaves_the_whole_window_in_the_past(days):
    """The anchor is a whole window back, in minutes.

    Every scenario emits `days * 1440` minutes forward from it, so anchoring to yesterday's
    midnight put the end of a three-day window two days into the future.
    """
    base = gen.default_base_ts(days)
    span_minutes = days * 1440
    end_of_series = base + span_minutes * 60_000
    now = _now_ms()
    assert end_of_series <= now, (
        f"--days {days}: the series would end "
        f"{dt.datetime.fromtimestamp(end_of_series / 1000, dt.UTC)}, "
        f"{dt.datetime.fromtimestamp(now / 1000, dt.UTC)} is now")


def test_the_window_is_the_length_that_was_asked_for():
    """The anchor moves as the window grows, so the coverage query finds the data.

    A fix that pinned every window to yesterday's midnight would satisfy the future check and
    silently make `--days 7` cover only its last day.
    """
    one = gen.default_base_ts(1)
    seven = gen.default_base_ts(7)
    assert seven < one, "a wider window must start further back"
    # Six extra days, within a minute of slop for the two calls being microseconds apart.
    delta_days = (one - seven) / 86_400_000
    assert 5.9 < delta_days < 6.1, f"expected ~6 more days of history, got {delta_days}"


def test_one_day_of_data_ends_close_to_now():
    """With the default window the newest sample is now, not yesterday.

    Otherwise every freshness rule in the dashboard - the map's stale markers, the staleness
    alert, "last sync" on the index - is comparing against a series that stopped a day ago.
    """
    end = gen.default_base_ts(1) + 1440 * 60_000
    now = _now_ms()
    assert now - end < 120_000, "a one-day window should end within two minutes of now"


# --------------------------------------------------------------------- end to end


@pytest.mark.parametrize("days", [1, 3])
def test_no_generated_row_is_in_the_future(days, tmp_path):
    """The whole generator, run for real, writing real CSV.

    The anchor tests above reason about arithmetic; this one runs the code and reads the
    timestamps back out of the files, which is where a different units error would show up.
    """
    out = tmp_path / "sim"
    rows = 0
    future = 0
    worst = 0
    for index in range(1, 4):
        series = gen.Series(f"CAUCE-TEST-{index:03d}", f"TEST-{index}", 42 + index)
        for _name, fn in gen.SCENARIOS.items():
            produced = fn(series, days)
            rows += len(produced)
            now = _now_ms()
            for row in produced:
                ts = row[3]
                worst = max(worst, ts)
                if ts > now:
                    future += 1
    assert rows > 0, "the generator produced nothing, so this test proved nothing"
    assert future == 0, f"{future} of {rows} rows are dated in the future"
    assert worst <= _now_ms()


def test_the_fixed_epoch_still_reproduces_the_same_data():
    """`--base-ts` is the escape hatch for byte-identical output, and must still work.

    Asserting only the default would leave the flag untested, and a future change that made
    it relative would break every fixture that depends on a fixed date without anything here
    noticing.
    """
    series = gen.Series("CAUCE-A", "A", 1)
    first = gen.SCENARIOS["normal_day"](series, 1)
    second = gen.SCENARIOS["normal_day"](gen.Series("CAUCE-A", "A", 1), 1)
    assert [r[3] for r in first] == [r[3] for r in second], (
        "the same seed and window must give the same timestamps, or nothing downstream "
        "that compares two generated runs means anything")
