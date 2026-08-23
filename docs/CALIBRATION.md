# CAUCE Calibration

## Model (defined; runtime application pending)

```
calibrated_value = raw_value × scale + offset
```

Per-sensor metadata to persist during the calibration phase:

- sensor_id, model, serial
- installation_date, calibration_date
- calibration_method, calibration_reference
- calibration_status

## What the system claims and does NOT

**Does NOT claim:**

- Professional meteorological accuracy or metrological traceability.
- Factory precision verified experimentally by this project.

**Does support today:**

- **Relative comparison between nodes** co-installed in the same context.
- Behavioral anomaly detection (freezing, jumps, out-of-range) through the
  validation pipeline.

## Mandatory distinctions when reporting data

| Term | BME280 (datasheet, not verified by us) |
|---|---|
| Resolution | 0.01 °C / 0.01 %RH |
| Sensor accuracy | ±0.5 °C / ±3 %RH |
| System accuracy | **Not measured yet** — includes enclosure, self-heating, exposure |
| Calibration uncertainty | **Does not exist yet** — no formal process |

Project rule: any scientific claim requiring external validation is marked
**pending**, never presented as a result.

## Minimal recommended plan (once ≥2 physical nodes exist)

1. Co-locate all nodes for 48 h at the same point → compute relative
   node-to-node offsets.
2. Record offsets as `method=co-location-relative` calibrations.
3. Repeat each season change or after maintenance.
4. Log every event into maintenance_events (backend phase).
