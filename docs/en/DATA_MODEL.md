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

Three rules with no exceptions:

- **Nothing is silently discarded.** An out-of-range reading goes to
  flash as `quality=INVALID` with its reason bits attached. You can
  always see what the sensor actually said.
- `timeUncertain` lets the node keep working with no NTP and no RTC.
  The data stays recorded and auditable, flagged for temporal
  reconstruction later instead of being thrown away for having no
  timestamp.
- After a reboot the sequence resumes at stored-max+1, so power cuts
  never create duplicates.

## Quality semantics

| State | Meaning |
|---|---|
| VALID | Passed every check |
| CALIBRATED / UNCALIBRATED | Room reserved for a future calibration overlay (not applied at runtime yet) |
| ESTIMATED | Reserved for aggregation/imputation |
| SUSPECT | Plausible but off (sudden jump or frozen sensor) |
| INVALID | Impossible (physical range, NaN, duplicate) |
| MISSING | A window where data was expected and isn't |

## Validation thresholds (defaults, configurable per node)

| Variable | Range | Max change/minute |
|---|---|---|
| air_temperature | -40..85 °C | ±5 |
| relative_humidity | 0..100 %RH | ±20 |
| pressure | 300..1100 hPa | ±2 |
| illuminance | 0..200000 lx | ±120000 |
| battery_voltage | 2.5..4.5 V | ±0.2 |
| wind_speed | 0..60 m/s | ±25 |
| tvoc | 0..65000 ppb | ±20000 |

Stuck detection: 6 or more identical readings within epsilon 0.01 —
that's a dead sensor pretending to work.

### What the optional sensors add, and what they do not

Three channels beyond the BME280's temperature, humidity and pressure, each off by
default and enabled per node because the hardware differs:

| Channel | Part | Why it is here |
|---|---|---|
| `illuminance` | BH1750 | The channel has been in this model since the beginning with no driver behind it. Datasheet accuracy is +/-20 %, and the field procedure budgets for that. |
| `wind_speed` | Hall switch on a cup rotor | Makes co-location checkable instead of assumed. Two nodes 300 m apart only measure the same air if the air is moving slowly; without a wind reading that assumption is untestable. |
| `tvoc` | CCS811 | The part senses VOC. It also publishes an *estimated* CO2 figure derived from that response, and this network does not report it - see below. |

**There is no CO2 channel, and that is a decision rather than a gap.** The CCS811
estimates `eCO2` from the response of a metal-oxide sensor to volatile compounds; it
does not measure carbon dioxide. The estimate drifts with humidity and temperature,
reads high next to vegetation or any solvent, and moves during its own ten-minute
warm-up. Publishing it as a CO2 measurement would put a value in the network that
cannot be calibrated and cannot be defended, in a project whose entire claim is
comparable data with the uncertainty shown. A real CO2 reading needs an NDIR sensor -
SCD4x or MH-Z19 - which is a different part with a different calibration story.

The rise thresholds for wind and VOC are deliberately loose. A gust is real data and a
person walking past the inlet is real data; a tight limit would mark both SUSPECT and
discard exactly the signal the sensor was added to capture.

## Calibration and maintenance (central)

`measurements.value` is the raw reading and is never rewritten. The
calibrated value is derived on read:

```
calibrated_value = value * scale + offset
```

| Table | Key | Holds |
|---|---|---|
| `calibration` | `(site_id, variable)` | `scale`, `offset`, `method`, `calibration_reference`, `sensor_id`, `calibration_date`, `status`, `notes`, `updated_utc_ms` |
| `maintenance_events` | `event_id` | `site_id`, `kind`, `at_utc_ms`, `notes` |

`status` is `applied`, `provisional`, `retired` or `rejected`; anything
but `retired` is applied to analytics, CSV and the dashboard, and retired
rows stay for audit. Because the map is linear the transform is exact on
the `agg_hourly` buckets: location statistics shift and dispersion scales
by `|scale|`.

Design note: the `CALIBRATED` / `UNCALIBRATED` quality codes remain
reserved. Calibration is an overlay applied at read time, not a property
of the record, so the stored quality keeps describing what the validation
engine saw.

## Repository

- Interface `IStorageRepository`: append / paginated query / query-by-
  sequence / latest / counters / retention / integrityCheck.
- Implementation: `LogStorageRepository` (protocol/v1/PROTOCOL.md).
- Queries walk sequentially — full history never sits in RAM at once.

## Node configuration (`NodeConfig`)

Identity (`node_id`, `site_id`), optional geo (lat/lon/elevation, NAN =
undeclared), installation context (land cover, shade), operation
(sampling interval, sync interval, timezone), network (Wi-Fi, NTP),
storage budgets and the full validation thresholds.

What you get: validation that tells you every error, a `.bak` backup
before each save, and a recovery chain of main→backup→defaults.

## Export formats

**CSV (RFC4180)** — opens in Excel/LibreOffice/Python/R:

```
node_id,sensor_id,sequence,timestamp_utc_ms,timestamp_iso,variable,value,unit,quality,reason_bits,time_uncertain,calibrated_value,calibration_scale,calibration_offset
CAUCE-001,BME280-1,1842,1787356860000,2026-08-22T00:01:00Z,air_temperature,21.50,C,VALID,0,0,21.50,1.0,0.0
CAUCE-002,BME280-1,91,1787356860000,2026-08-22T00:01:00Z,air_temperature,22.60,C,VALID,0,0,21.50,1.0,-1.1
```

Text fields quoted/escaped per RFC4180. ASCII encoding. The three
calibration columns are appended at the end so older readers keep
working; they are empty for a variable with no calibration record.

**JSON array** — one object per measurement, same schema as the
protocol v1 payload.

Both come out of `ChunkedExporter` — history is streamed, never loaded
whole.

One analytical rule: derived metrics leave out `INVALID`, `MISSING` and
`ESTIMATED`. `SUSPECT` counts, but its share has to be reported next to
the number.

Time windows are INCLUSIVE on both ends, firmware and backend alike —
settled once, enforced everywhere.
