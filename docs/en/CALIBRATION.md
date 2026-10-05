# CAUCE Calibration

## Model

```
calibrated_value = raw_value × scale + offset
```

Implemented in the central, which is the **single source of truth**.
`measurements.value` is never rewritten: calibrated values are derived on
read, so a recalibration can be replayed over the whole history without
touching a node or losing the raw record.

One line of math, deliberately boring. Per-sensor metadata to record
during the calibration phase:

- sensor_id, model, serial
- installation_date, calibration_date
- calibration_method, calibration_reference
- calibration_status

## Why the central and not the node

A `co-location-relative` offset only exists once several nodes have sat
in the same spot together, so the knowledge is born at the centre.
Pushing it back to the nodes would duplicate state that could then
diverge, and the node would then be storing a number it cannot verify.
The node therefore keeps reporting raw values and the central does the
arithmetic.

## API

| Method | Path | Description |
|---|---|---|
| PUT | `/v1/sites/{id}/calibration` | Upsert one `(site, variable)` record. Admin token |
| GET | `/v1/sites/{id}/calibration` | All records for the site, with the model spelled out |
| POST | `/v1/sites/{id}/maintenance` | Log a maintenance event (install, calibration, sensor replacement, relocation) |
| GET | `/v1/sites/{id}/maintenance` | Maintenance log, newest first |

`scale` must be finite and non-zero; `offset` must be finite. Both are
range-checked to catch typos rather than to police legitimate values.
`status` is one of `applied`, `provisional`, `retired`, `rejected`, and
`retired` removes the record from every calculation without deleting it.

## Where it is applied

| Surface | Behaviour |
|---|---|
| `/v1/analytics/summary`, `/compare`, `/period-compare` | Top-level numbers stay raw; a `calibrated` block appears next to them when a record exists |
| `/v1/analytics/summary-fast`, `granularity=hourly` | Same, applied to the hourly aggregates |
| `/v1/analytics/heat-events` | Events are detected on calibrated values, so a threshold means what an operator expects |
| `/v1/analytics/before-after` | `mean_shift_calibrated` and `difference_in_differences_calibrated` beside the raw figures |
| `/colocation` | Applied before the bias is computed, which is what the pilot acceptance criterion measures |
| `/nodes/{id}/report` | Extra calibrated min/max/mean columns, `*` marks a calibrated variable |
| CSV exports | Three appended columns: `calibrated_value`, `calibration_scale`, `calibration_offset` |

Because the map is linear, the transform is exact on aggregates: location
statistics shift, dispersion scales by `|scale|`, and no re-read of the
raw rows is needed to correct an hourly bucket.

Every response carries a `calibration` object stating whether it was
applied, and with which scale, offset, method and date. When it was not,
the `calibrated` key is absent rather than identical to the raw value.

## What the system claims and does NOT

**Does NOT claim:**

- Professional meteorological accuracy or metrological traceability.
- Factory precision verified experimentally by this project — the
  datasheet numbers are printed in the table below, not endorsed.
- That a calibrated node is *correct*: a co-location offset fixes the
  difference between two sensors in one place at one moment, and nothing
  else.

**Does support today:**

- **Relative comparison between nodes** sitting in the same context.
  "Node A reads 1.2 °C above node B all week" is a solid statement;
  "it is 24.7 °C outside" is not one we make.
- Behavioral anomaly detection (frozen, jumping, out-of-range sensors)
  through the validation pipeline.

## Mandatory distinctions when reporting data

| Term | BME280 (datasheet, not verified by us) |
|---|---|
| Resolution | 0.01 °C / 0.01 %RH |
| Sensor accuracy | ±0.5 °C / ±3 %RH |
| System accuracy | **Not measured yet** — enclosure, self-heating and exposure all count and none are characterized |
| Calibration uncertainty | **Does not exist yet** — there is no formal process |

Project rule: any scientific claim that needs outside validation stays
marked **pending**. We don't present wishes as results. A calibration
record with `method=co-location-relative` and no uncertainty estimate
belongs in a report under "pending", not under "measured".

## Calibration uncertainty

A calibration record can carry an absolute `uncertainty` and the
`uncertainty_kind` that says where it came from: `sensor_datasheet`,
`co_location_spread`, `repeatability`, `estimated` or `unknown`.

Two rules make this worth having:

- **`null` is not zero.** An absent uncertainty means nobody characterised it,
  which is a different statement from "it is exact". The CSV leaves the column
  blank in that case, and every analytics response reports `uncertainty: null`.
- **It scales with the correction.** A scale of 0.5 halves the uncertainty the
  scale introduces, so the reported figure is the uncertainty *after*
  calibration. A negative scale uses its magnitude, because a reflection adds
  no error.

`uncertainty_kind` without an `uncertainty` is a `422`: naming the kind of an
uncertainty nobody quantified is worse than saying nothing. A partial update
that omits both keeps whatever was recorded before, so re-posting an offset does
not silently drop a characterisation that took a co-location week to produce.

## The budget, per quantity

`backend/cauce_server/uncertainty.py` holds the components; the combined figure
reaches a report through `calibration_summary`, so a number never arrives without
what to compare it to.

A budget rather than a single figure because the components answer different
questions. Someone deciding whether a reading can drive a heat alert needs to know
whether the error is in the sensor (replace it) or in the comparison (redo it), and
one averaged number cannot say. Components are combined by root-sum-of-squares,
which assumes independence, and the largest is reported as `dominant_term`.

| quantity | components | dominant |
|---|---|---|
| `air_temperature` | sensor 0.5 °C, co-location 0.3 °C, quantisation 0.01 °C | sensor, but the pair is close |
| `relative_humidity` | **3 % of reading**, co-location 2 %RH, quantisation 0.1 | **depends on the reading** |
| `pressure` | sensor 1 hPa, altitude offset 0, quantisation 0.01 hPa | sensor |

Two of those deserve emphasis.

**Humidity is not a constant.** Its datasheet term is proportional, so the budget
cannot be one number — `combined_for_reading` exists for that, and the dominant term
*changes* with humidity: the proportional term wins at 80 %RH, the co-location term
wins at 10 %RH. A single figure for humidity is a claim nobody can make.

**Pressure cannot be fixed by calibration.** A station reporting sea-level pressure
disagrees by roughly 1 hPa per 8.5 m of altitude. That term is in the budget as
`altitude_offset` and set to zero on purpose: it is zero when nothing is being
compared to sea level, and the assumption text says so. Comparing against a
sea-level station is an error no `scale`/`offset` can correct.

### What these numbers are

Stated assumptions for a BME280-class sensor, recorded so a later traceable
calibration replaces a figure rather than rewriting a rationale. Every payload
carries `traceable: false` until that changes, because the absence of traceability
must be visible in the data rather than in a document.

A variable with no budget returns `None`, never a zero budget. "We have no numbers
for this quantity" and "this quantity is exact" are different, and conflating them
is how a report ends up quoting a precision nobody established.

## Procedure, per quantity

Written so two people produce the same numbers. Filled in where the method is
decided and left explicit where it is not, because an unfilled cell is a decision
nobody has made yet.

### Running it

The table below is executed by `backend/tools/calibrate.py`, so the fit is not
left to the reader:

```bash
python tools/calibrate.py --site SITE-A --variable air_temperature \
    --reference CAUCE-REF --hours 48

# writes the accepted calibrations through the API, and only those
python tools/calibrate.py --site SITE-A --variable air_temperature \
    --reference CAUCE-REF --hours 48 --apply
```

It reads the central's own co-located rows, fits the map, evaluates the acceptance
limit above, and reports `accept`, `reject` or `provisional` per node. Without
`--apply` it only reports, because a calibration applied by a script nobody read is
a calibration nobody decided on.

Two decisions it settles, which the prose alone did not:

- **`scale` is 1.0 by default and only `offset` is fitted**, as the mean signed
  difference. Least squares over both terms is available behind `--fit-scale`, and
  is the right choice when the disagreement is gain rather than bias.
- **Fewer than 20 pairs yields `provisional`, never `accept`.** Four samples are not
  the procedure's 48.

One result worth reading before interpreting output: **an offset cannot reduce the
spread.** The residuals after an offset-only fit are the raw differences minus their
own mean, and a standard deviation does not move when every value shifts by a
constant. So `spread` only drops under `--fit-scale`. A report showing an unchanged
spread is not a broken calibration; it is saying the disagreement is scattered
rather than offset.

| step | air_temperature | relative_humidity | pressure |
|---|---|---|---|
| reference | co-located reference node, calibrated against a reference thermometer | co-located reference node | co-located reference node |
| method | `co-location-relative` | `co-location-relative` | `co-location-relative` |
| duration | 48 h | 48 h | 48 h |
| points | 48 hourly pairs | 48 hourly pairs | 48 hourly pairs |
| accept when | post-calibration spread ≤ **0.2 °C** | ≤ **3 %RH** | ≤ **1 hPa** |
| interval | every 6 months, and after relocation | every 6 months | every 6 months |
| on failure | `status=rejected`, node stays on the raw column | same | same |

Two rules that are not per-quantity:

- **Retire, never delete.** A superseded record becomes `status=retired`. The
  history is what makes a calibration auditable.
- **Record the reference, not just the number.** `calibration_reference` names the
  reference node or instrument. A correction with no reference is an opinion.

### What this still is not

There is no metrological traceability here. The reference is another node, and the
chain ends at a sensor datasheet. Everything above supports "how much of this number
is measurement and how much is method", which is worth having, and it is not the
same claim as a calibrated instrument.


## Minimal recommended plan (once ≥2 physical nodes exist)

1. Co-locate all nodes for 48 h at the same spot → compute
   node-to-node relative offsets.
2. Record the offsets as `method=co-location-relative` calibrations and
   log the event in `POST /v1/sites/{id}/maintenance`.
3. Re-open `/colocation` and read the post-calibration spread.
4. Repeat every season change or after maintenance; `status=retired` the
   previous record instead of deleting it.
5. Check that the pilot acceptance criterion
   (spread ≤0.2 °C / ≤3 %RH) is evaluated on the calibrated column, and
   quote the raw spread next to it.
