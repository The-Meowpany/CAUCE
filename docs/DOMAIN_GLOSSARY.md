# CAUCE Domain Glossary

Canonical English vocabulary for the entire system. One concept, one term.
When a Spanish term historically mapped to several English candidates, the
chosen one is listed here and used consistently across code, schemas, logs,
tests and docs.

## Core

| Term | Definition |
|---|---|
| **Node** | A single microstation device (`CAUCE-001`…). Canonical id: `node_id`. |
| **Site** | Physical location where one or more nodes are installed (`site_id`). Carries placement metadata: latitude, longitude, elevation, land cover, shade condition. |
| **Measurement** | The atomic data unit: `node_id + sequence`, UTC timestamp, variable, value, quality, reason bits. Never deleted; invalid values are stored flagged. |
| **Variable** | Enumerated environmental quantity: `air_temperature`, `relative_humidity`, `pressure`, `illuminance`, `battery_voltage`. |
| **Quality** | Data assessment state machine: `VALID`, `CALIBRATED`, `UNCALIBRATED`, `ESTIMATED`, `SUSPECT`, `INVALID`, `MISSING`. |
| **Reason bits** | Bitmask explaining *why* a quality was assigned (range, non-finite, rate-of-change, stuck, time-uncertain, duplicate). |

## Storage

| Term | Definition |
|---|---|
| **Frame** | 72-byte on-flash record: magic + version + length + payload + CRC32. |
| **Segment** | Append-only `.clog` file holding frames. Sealed when its tail is corrupt; new writes roll to a fresh segment. |
| **Watermark** | Highest acknowledged `sequence` for a node, persisted by the sync client. |
| **Retention policy** | Rule deleting oldest segments above a byte budget; at least one segment always remains. |
| **Integrity check** | Full rescan counting corrupted/unparsable segments. |

## Networking & Sync

| Term | Definition |
|---|---|
| **Sync batch** | JSON payload carrying up to N measurements since the watermark. `batch_size` always equals the number of records actually serialized. |
| **Acknowledged sequence** | Highest `sequence` the server confirms persisted; never fabricated. |
| **Idempotent replay** | Re-sending already-acked records after watermark loss; the server deduplicates on `(node_id, sequence)`. |
| **Degraded** | Link state where RSSI falls below threshold but traffic still flows. |
| **AP fallback** | Node-side access point started after repeated STA failures; hosts the captive portal. |

## Analytics

| Term | Definition |
|---|---|
| **Derived metric** | Any computed statistic (mean, percentile, exposure hours). Always tagged `metric_type: derived`; never presented as raw measurement. |
| **Before/after evaluation** | Comparison split by an intervention's time window. Requires ≥30 valid samples per period to be marked `sufficient_sample: true`. |
| **Exposure hours** | Trapezoidal duration above a temperature threshold between consecutive samples. |
| **Causality disclaimer** | Mandatory note: differences may reflect placement or calibration; they do not establish causality. |

## Update & Power

| Term | Definition |
|---|---|
| **OTA release** | Manifest entry `{version, sha256_hex, url, total_size}`. |
| **Reboot pending** | New image verified and staged; device restarts into it. Rollback relies on the alternate-partition boot-counter pattern (hardware bring-up pending). |
| **Sleep advisory** | Recommendation produced by `SleepPolicy`; never applied automatically (master plan §33). |

## Public presentation

| Term | Definition |
|---|---|
| **Locale labels** | Human strings live only in presentation dictionaries (`I18N` in the node SPA, `LABELS` in the central dashboard). Domain values stay language-neutral codes (`VALID`, not "Válido"). |
