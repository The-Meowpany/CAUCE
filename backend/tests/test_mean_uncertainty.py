"""Uncertainty on aggregated means, and order statistics under a negative calibration scale."""

from __future__ import annotations

import math

import pytest
from cauce_server.api import _agg_stats
from cauce_server.calibration import transform_stats


def bucket(values):
    return {"cnt": len(values), "sum": sum(values), "sumsq": sum(v * v for v in values),
            "min_v": min(values), "max_v": max(values)}


def test_independent_samples_give_the_textbook_standard_error():
    rows = [bucket([1.0, 3.0]), bucket([1.0, 3.0])]
    stats = _agg_stats(rows)
    sd = math.sqrt(4 * 1.0 / 3)  # population variance 1, n = 4
    assert stats["mean_stderr"] == pytest.approx(sd / 2, abs=1e-4)
    assert stats["mean_stderr_method"] == "independent_samples"
    assert stats["mean_ci95"] == pytest.approx([2 - 1.96 * sd / 2, 2 + 1.96 * sd / 2], abs=1e-3)


def test_a_drifting_series_is_not_reported_as_precise():
    # Each hour is flat inside but the hours differ: sixty identical readings per bucket
    # would make s/sqrt(n) tiny, and it is the buckets that say how well the mean is known.
    rows = [bucket([20.0 + h] * 60) for h in range(4)]
    stats = _agg_stats(rows)
    between = math.sqrt(sum((m - 21.5) ** 2 for m in (20, 21, 22, 23)) / 3) / 2
    assert stats["mean_stderr_method"] == "between_buckets"
    assert stats["mean_stderr"] == pytest.approx(between, abs=1e-4)


def test_one_sample_has_no_standard_error():
    stats = _agg_stats([bucket([5.0])])
    assert stats["mean_stderr"] is None and stats["mean_ci95"] is None


def test_a_negative_scale_swaps_every_order_statistic_and_keeps_the_interval_ordered():
    raw = {"count": 10, "min": 1.0, "max": 9.0, "mean": 5.0, "median": 5.0, "p05": 1.5,
           "p25": 3.0, "p75": 7.0, "p95": 8.5, "stddev": 2.0, "mean_stderr": 0.5,
           "mean_ci95": [4.0, 6.0]}
    out = transform_stats(raw, {"scale": -1.0, "offset": 0.0})
    assert (out["min"], out["max"]) == (-9.0, -1.0)
    assert (out["p05"], out["p95"]) == (-8.5, -1.5)
    assert (out["p25"], out["p75"]) == (-7.0, -3.0)
    assert out["mean_ci95"] == [-6.0, -4.0]
    assert out["mean_stderr"] == 0.5
