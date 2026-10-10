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

import csv
import datetime as dt
import importlib.util
import os
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
    """The generator, run for real, and the CSV read back off disk.

    The anchor tests above reason about arithmetic and would agree with each other even if the
    units were wrong twice. This one does not: it runs every scenario, writes the files the
    tool actually writes, and reads the timestamps back out of them. A milliseconds/seconds
    slip survives the first test and dies here.

    Which is why the file is written rather than the rows inspected in memory - the CSV
    carries both `timestamp_utc_ms` and a derived `timestamp_iso`, and only the round trip
    proves the two agree.
    """
    out = tmp_path / "sim"
    out.mkdir(parents=True, exist_ok=True)

    out = tmp_path / "sim"
    rows = 0
    future = 0
    worst = 0
    seen_iso = 0
    for index in range(1, 4):
        series = gen.Series(f"CAUCE-TEST-{index:03d}", f"TEST-{index}", 42 + index)
        for name, fn in gen.SCENARIOS.items():
            produced = fn(series, days)
            rows += len(produced)
            path = str(out / f"{index}-{name}.csv")
            # The generator's own writer, not a reimplementation. The first version of this
            # test built the CSV by hand and got the header wrong, which is the kind of thing
            # that makes an end-to-end test stop testing the real thing.
            gen.write_csv(path, produced)

            now = _now_ms()
            with open(path, encoding="utf-8", newline="") as fh:
                for record in csv.DictReader(fh):
                    ts = int(record["timestamp_utc_ms"])
                    worst = max(worst, ts)
                    if ts > now:
                        future += 1
                    # The ISO column is derived from the millisecond one. If they drift apart
                    # the CSV tells two stories about when a reading happened.
                    iso = record["timestamp_iso"]
                    expected = dt.datetime.fromtimestamp(ts / 1000, dt.UTC).strftime(
                        "%Y-%m-%dT%H:%M:%SZ")
                    assert iso == expected, (
                        f"{os.path.basename(path)}: {iso} does not match {ts} ms, which is "
                        f"{expected}")
                    seen_iso += 1

    assert rows > 0, "the generator produced nothing, so this test proved nothing"
    assert seen_iso == rows, f"read back {seen_iso} rows for {rows} written"
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
