# CAUCE Calibration

## Model (defined; runtime application pending)

```
calibrated_value = raw_value × scale + offset
```

One line of math, deliberately boring. Per-sensor metadata to record
during the calibration phase:

- sensor_id, model, serial
- installation_date, calibration_date
- calibration_method, calibration_reference
- calibration_status

## What the system claims and does NOT

**Does NOT claim:**

- Professional meteorological accuracy or metrological traceability.
- Factory precision verified experimentally by this project — the
  datasheet numbers are printed in the table below, not endorsed.

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
marked **pending**. We don't present wishes as results.

## Minimal recommended plan (once ≥2 physical nodes exist)

1. Co-locate all nodes for 48 h at the same spot → compute
   node-to-node relative offsets.
2. Record the offsets as `method=co-location-relative` calibrations.
3. Repeat every season change or after maintenance.
4. Log every event into maintenance_events (backend phase).
