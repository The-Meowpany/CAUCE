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

What this does not do is make the project traceable. It lets a report say how
much of a number is measurement and how much is method. There is still no
calibration procedure and no metrological claim anywhere in this system.


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
