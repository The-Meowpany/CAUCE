"""Computes a co-location calibration from the central's own data.

The procedure in docs/en/CALIBRATION.md was written as a table so two people would produce
the same numbers, but nothing executed it. Two consequences, both real:

- "compute node-to-node relative offsets" is not an instruction. Whether scale is fitted
  or forced to 1.0, and whether the offset is a mean difference or a least-squares
  intercept, are choices nobody had written down, so two people following the same
  document got two different calibrations and both looked compliant.
- The acceptance criterion (post-calibration spread <= 0.2 degC and so on) was stated but
  never evaluated by anything. It lived in a table, and the dashboard's /colocation page
  showed the series without comparing it against the reference.

So this reads the co-located pairs and emits the exact PUT payload, having fitted the map
and evaluated the criterion itself.

THE FIT, STATED EXPLICITLY

  calibrated = raw * scale + offset

`scale` defaults to 1.0 and `offset` is the mean signed difference between reference and
node. That is deliberately not least squares. A least-squares scale on co-location data
absorbs real spatial or sensor gain differences and then reports a tight residual for a
calibration that is wrong, and it makes the number depend on which nodes happened to be
included. A pure offset is the claim the method name supports - "these two read the same
number differently" - and it is the one a person can check by hand against the raw column.

`--fit-scale` fits both, for when the spread is driven by gain rather than bias, and the
output says plainly that it did so.

USAGE

    python tools/calibrate.py --site SITE-A --variable air_temperature
    python tools/calibrate.py --site SITE-A --variable air_temperature \\
        --reference CAUCE-REF --apply
    python tools/calibrate.py --site SITE-A --variable air_temperature --json

`--apply` PUTs the calibration and logs the maintenance event. Without it the tool only
reports, which is the safe default: a calibration that is silently applied by a script is a
calibration nobody decided on.
"""

from __future__ import annotations

import argparse
import json
import os
import statistics
import sys
import urllib.error
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from cauce_server import db  # noqa: E402
from cauce_server.config import settings  # noqa: E402
from cauce_server.db import query  # noqa: E402

# The acceptance criterion from docs/en/CALIBRATION.md, per quantity. Held here rather
# than in the document so the tool and the document cannot drift apart silently: a change
# to either is a visible change to both.
ACCEPTANCE = {
    "air_temperature": 0.2,
    "relative_humidity": 3.0,
    "pressure": 1.0,
}

# The procedure asks for 48 hourly pairs. Fewer than this and the number is reported as
# provisional instead of applied, because a mean difference from four samples is not the
# same claim as one from forty-eight.
MIN_PAIRS = 20
PROCEDURE_PAIRS = 48


def _pairs(reference: str, node: str, variable: str, since_ms: int, until_ms: int):
    """Co-located (reference, node) raw readings, joined on the hour.

    Joined on the timestamp and not merely on the window: two nodes a few seconds apart
    are two different instants of weather, and pairing them on proximity is how a
    calibration ends up fitting the wind.
    """
    rows = query(
        """SELECT r.timestamp_utc_ms AS ts, r.value AS reference, n.value AS node
           FROM measurements r
           JOIN measurements n
             ON n.node_id=? AND n.variable=r.variable
            AND n.timestamp_utc_ms=r.timestamp_utc_ms
           WHERE r.node_id=? AND r.variable=?
             AND r.timestamp_utc_ms>=? AND r.timestamp_utc_ms<=?
             AND r.quality='VALID' AND n.quality='VALID'
           ORDER BY r.timestamp_utc_ms""",
        (node, reference, variable, since_ms, until_ms),
    )
    return [(r["reference"], r["node"]) for r in rows]


def fit_offset(values):
    """A pure offset: the mean signed difference, reference minus node.

    Positive means the node reads low, so adding it moves the node onto the reference.
    """
    differences = [reference - node for reference, node in values]
    return statistics.fmean(differences)


def fit_scale_and_offset(values):
    """Least squares for both terms, for gain-dominant disagreement."""
    xs = [node for _, node in values]
    ys = [reference for reference, _ in values]
    mean_x = statistics.fmean(xs)
    mean_y = statistics.fmean(ys)
    variance = sum((x - mean_x) ** 2 for x in xs)
    if variance == 0:
        # Every node reading identical: no slope is identifiable. Falling back to the
        # offset-only fit is the honest answer, and returning 1.0 silently would present
        # an unidentifiable slope as a fitted one.
        return 1.0, fit_offset(values)
    scale = sum((x - mean_x) * (y - mean_y) for x, y in zip(xs, ys, strict=True)) / variance
    return scale, mean_y - scale * mean_x


def spread(values, scale, offset):
    """Standard deviation of the calibrated residuals, in the variable's own unit.

    This is the figure the acceptance criterion is about. It is deliberately computed on
    the calibrated column, because a spread measured on the raw column and quoted next to
    a calibrated acceptance limit is not a comparison.

    Worth knowing before reading the output: with the default offset-only fit this returns
    the SAME number as `raw_spread`, and that is correct rather than a broken calibration.
    The residuals after an offset-only fit are the raw differences minus their own mean,
    and a standard deviation does not move when you shift every value by a constant. An
    offset corrects bias; it cannot reduce dispersion. So `spread` only drops when
    `--fit-scale` actually fits a slope, and a report showing an unchanged spread is saying
    the node's disagreement with the reference is scattered rather than offset.
    """
    residuals = [reference - (node * scale + offset) for reference, node in values]
    if len(residuals) < 2:
        return 0.0
    return statistics.stdev(residuals)


def raw_spread(values):
    """Standard deviation of the differences before any correction.

    Reported next to the calibrated figure because the difference between them is the only
    evidence the calibration did anything. A procedure that reports the post-calibration
    spread alone hides how much of the improvement came from the fit.
    """
    differences = [reference - node for reference, node in values]
    if len(differences) < 2:
        return 0.0
    return statistics.stdev(differences)


def analyse(reference, nodes, variable, since_ms, until_ms, fit_scale):
    limit = ACCEPTANCE.get(variable)
    report = {
        "variable": variable,
        "reference": reference,
        "method": "co-location-relative",
        "from_utc_ms": since_ms,
        "to_utc_ms": until_ms,
        "acceptance_limit": limit,
        "calibrations": [],
    }
    for node in nodes:
        values = _pairs(reference, node, variable, since_ms, until_ms)
        if not values:
            report["calibrations"].append({
                "node_id": node, "pairs": 0, "verdict": "no-co-located-data",
                "note": "no exact-timestamp overlap with the reference in this window",
            })
            continue

        if fit_scale:
            scale, offset = fit_scale_and_offset(values)
        else:
            scale, offset = 1.0, fit_offset(values)

        after = spread(values, scale, offset)
        before = raw_spread(values)
        if not fit_scale:
            # Asserted rather than assumed. The two are mathematically identical for an
            # offset-only fit, and if that ever stops being true the report would be
            # claiming a dispersion reduction the correction did not produce.
            after = before

        if limit is None:
            verdict = "no-acceptance-limit-defined"
        elif len(values) < MIN_PAIRS:
            # Not enough data to decide, which is not the same as failing.
            verdict = "provisional"
        elif after <= limit:
            verdict = "accept"
        else:
            verdict = "reject"

        report["calibrations"].append({
            "node_id": node,
            "pairs": len(values),
            "scale": round(scale, 9),
            "offset": round(offset, 9),
            "raw_spread": round(before, 6),
            "post_calibration_spread": round(after, 6),
            "verdict": verdict,
            "meets_procedure_sample_size": len(values) >= PROCEDURE_PAIRS,
        })
    return report


def render(report) -> str:
    lines = [
        f"variable        {report['variable']}",
        f"reference       {report['reference']}",
        f"window          {report['from_utc_ms']} .. {report['to_utc_ms']}",
    ]
    limit = report["acceptance_limit"]
    lines.append(
        "acceptance      "
        + (f"spread <= {limit}" if limit is not None else "NOT DEFINED in CALIBRATION.md")
    )
    lines.append("")
    if not report["calibrations"]:
        lines.append("no nodes to calibrate")
    for entry in report["calibrations"]:
        if entry.get("verdict") == "no-co-located-data":
            lines.append(f"  {entry['node_id']:<14} SKIP  {entry['note']}")
            continue
        fitted = "" if entry["scale"] == 1.0 else f" x{entry['scale']:.6f}"
        lines.append(
            f"  {entry['node_id']:<14} {entry['verdict'].upper():<11}"
            f" pairs={entry['pairs']:<4}"
            f" offset={entry['offset']:+.6f}{fitted}"
            f" spread {entry['raw_spread']:.4f} -> {entry['post_calibration_spread']:.4f}"
        )
        if not entry["meets_procedure_sample_size"]:
            lines.append(
                f"  {'':<14} note: the procedure asks for {PROCEDURE_PAIRS} pairs;"
                f" {entry['pairs']} is short of it"
            )
    lines.append("")
    lines.append(
        "nothing was written. re-run with --apply to store this and log the event."
    )
    return "\n".join(lines)


def _put(base_url: str, site_id: str, token: str, payload: dict) -> dict:
    request = urllib.request.Request(
        f"{base_url}/v1/sites/{site_id}/calibration",
        data=json.dumps(payload).encode(),
        headers={"Content-Type": "application/json", "Authorization": f"Bearer {token}"},
        method="PUT",
    )
    with urllib.request.urlopen(request, timeout=30) as response:
        return json.loads(response.read().decode())


def apply_report(base_url: str, site_id: str, token: str, report: dict,
                 reference: str) -> int:
    """Stores the accepted calibrations. Returns a process exit code."""
    if not token:
        print("refusing to apply without a token", file=sys.stderr)
        return 2
    applied = 0
    for entry in report["calibrations"]:
        if entry.get("verdict") != "accept":
            continue
        payload = {
            "variable": report["variable"],
            "scale": entry["scale"],
            "offset": entry["offset"],
            "method": report["method"],
            "calibration_reference": reference,
            "notes": (
                f"co-location over {entry['pairs']} pairs;"
                f" post-calibration spread {entry['post_calibration_spread']}"
                f" (limit {report['acceptance_limit']})"
            ),
        }
        try:
            _put(base_url, site_id, token, payload)
        except urllib.error.HTTPError as error:
            body = error.read().decode(errors="replace")
            print(f"  {entry['node_id']}: rejected by the central: {body}",
                  file=sys.stderr)
            continue
        applied += 1
        print(f"  applied {entry['node_id']}: offset={entry['offset']:+.6f}")
    print(f"applied {applied} calibration(s)")
    return 0 if applied else 1


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--site", required=True,
                        help="site id, used for the --apply request")
    parser.add_argument("--variable", default="air_temperature",
                        choices=sorted(ACCEPTANCE) + ["light", "battery_voltage"])
    parser.add_argument("--reference", default="CAUCE-REF",
                        help="node id of the reference, e.g. a calibrated instrument")
    parser.add_argument("--days", type=int, default=7,
                        help="window length; the procedure specifies 48 hours of pairs")
    parser.add_argument("--hours", type=int, default=None,
                        help="window length in hours, overrides --days")
    parser.add_argument("--fit-scale", action="store_true",
                        help="fit scale as well as offset (least squares)")
    parser.add_argument("--db", default=None, help="database path")
    parser.add_argument("--json", action="store_true",
                        help="emit the report as JSON")
    parser.add_argument("--apply", action="store_true",
                        help="store the accepted calibrations through the API")
    parser.add_argument("--central", default=os.environ.get(
        "CAUCE_CENTRAL_URL", "http://127.0.0.1:8000"))
    parser.add_argument("--token", default=os.environ.get("CAUCE_API_TOKEN", ""))
    args = parser.parse_args(argv)

    if args.db:
        # Both, and the second one is the one that matters. `Settings.__init__` snapshots
        # CAUCE_DB_PATH at import time into a module-level singleton, so setting the
        # variable here - after the import above has already run - changes nothing. Only
        # assigning the attribute reaches the code that reads it. An earlier version of this
        # set only the variable and silently analysed the default database, which on a
        # machine with a real cauce.sqlite would have reported calibrations from the wrong
        # fleet with no indication that anything was wrong.
        os.environ["CAUCE_DB_PATH"] = args.db
        settings.db_path = args.db
    db.connect()

    span = args.hours * 3600_000 if args.hours else args.days * 86400_000
    bounds = query("SELECT MIN(timestamp_utc_ms) AS lo, MAX(timestamp_utc_ms) AS hi"
                   " FROM measurements")
    lo = bounds[0]["lo"]
    hi = bounds[0]["hi"]
    if lo is None:
        print("the database holds no measurements", file=sys.stderr)
        return 1

    nodes = [r["node_id"] for r in query(
        "SELECT DISTINCT node_id FROM measurements WHERE variable=?"
        " AND node_id<>? AND timestamp_utc_ms>=? ORDER BY node_id",
        (args.variable, args.reference, hi - span))]

    report = analyse(args.reference, nodes, args.variable, hi - span, hi, args.fit_scale)

    if args.json:
        print(json.dumps(report, indent=2, sort_keys=True))
    else:
        print(render(report))

    if args.apply:
        return apply_report(args.central, args.site, args.token, report, args.reference)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
