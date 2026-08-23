# CAUCE Data Model

## Measurement (atomic unit)

```cpp
struct Measurement {
  char     nodeId[16];       // "CAUCE-001"
  char     sensorId[24];     // "BME280-1"
  uint32_t sequence;         // monotonic, survives reboots
  uint64_t timestampUtcMs;   // epoch ms; 0 when clock untrusted
  float    value;
  Variable variable;
  Quality  quality;
  uint8_t  reasonBits;       // why the quality is what it is
  bool     timeUncertain;    // orthogonal to quality
};
```

Rules:

- **Never silently discarded.** An out-of-range reading is stored as
  `quality=INVALID` plus its reason bits.
- `timeUncertain` keeps the node running without NTP/RTC: data stays recorded
  and auditable, flagged for future temporal reconstruction.
- The sequence restarts from stored-max+1 after reboot → no duplicates across
  power cuts.

## Quality semantics

| State | Meaning |
|---|---|
| VALID | Passed every check |
| CALIBRATED / UNCALIBRATED | Future calibration overlay (not applied at runtime yet) |
| ESTIMATED | Reserved for aggregation/imputation |
| SUSPECT | Plausible but anomalous (abrupt jump or frozen sensor) |
| INVALID | Impossible (physical range, NaN, duplicate) |
| MISSING | Expected window without data |

## Validation thresholds (defaults, configurable per node)

| Variable | Range | Max change/minute |
|---|---|---|
| air_temperature | -40..85 °C | ±5 |
| relative_humidity | 0..100 %RH | ±20 |
| pressure | 300..1100 hPa | ±2 |
| illuminance | 0..200000 lx | ±120000 |
| battery_voltage | 2.5..4.5 V | ±0.2 |

Stuck detection: ≥6 identical readings within epsilon 0.01.

## Repository

- Interface `IStorageRepository`: append / paginated query / query-by-
  sequence / latest / counters / retention / integrityCheck.
- Implementation: `LogStorageRepository` (protocol/v1/PROTOCOL.md).
- Queries paginate sequentially — full history is never loaded into RAM.

## Node configuration (`NodeConfig`)

Identity (`node_id`, `site_id`), optional geo (lat/lon/elevation, NAN =
undeclared), installation context (land cover, shade), operation (sampling
interval, sync interval, timezone), network (Wi-Fi, NTP), storage budgets and
full validation thresholds.

Guarantees: validation with enumerated errors; `.bak` backup before every
save; recovery chain main→backup→defaults.

## Export formats

**CSV (RFC4180)** — Excel/LibreOffice/Python/R compatible:

```
node_id,sensor_id,sequence,timestamp_utc_ms,timestamp_iso,variable,value,unit,quality,reason_bits,time_uncertain
CAUCE-001,BME280-1,1842,1787356860000,2026-08-22T00:01:00Z,air_temperature,21.50,C,VALID,0,0
```

Text fields quoted/escaped per RFC4180. ASCII encoding.

**JSON array** — one object per measurement, same schema as protocol v1
payload.

Both stream through `ChunkedExporter` — history is never loaded into RAM.

Analytical rule: derived metrics exclude `INVALID`, `MISSING` and
`ESTIMATED`; `SUSPECT` participates but its share must be reported.

Time-window semantics are INCLUSIVE on both ends everywhere (firmware and
backend).
