# CAUCE Domain Glossary

One concept, one term. This file exists because early drafts used
three different English words for the same thing depending on who
wrote the paragraph. When Spanish historically mapped to several
English candidates, the winner is listed here and used everywhere —
code, schemas, logs, tests, docs.

## Core

| Term | Definition |
|---|---|
| **Node** | One microstation device (`CAUCE-001`…). Canonical id: `node_id`. If you mean the physical box, say node. If you mean the place, that's a site. |
| **Site** | Where one or more nodes are installed (`site_id`). Carries placement metadata: latitude, longitude, elevation, land cover, shade condition. |
| **Measurement** | The atomic data unit: `node_id + sequence`, UTC timestamp, variable, value, quality, reason bits. Never deleted; bad values are stored flagged, not erased. |
| **Variable** | An enumerated environmental quantity: `air_temperature`, `relative_humidity`, `pressure`, `illuminance`, `battery_voltage`. Closed set — adding one touches firmware, backend and docs. |
| **Quality** | Assessment state machine: `VALID`, `CALIBRATED`, `UNCALIBRATED`, `ESTIMATED`, `SUSPECT`, `INVALID`, `MISSING`. Assigned by the validator, never by hand. |
| **Reason bits** | Bitmask saying *why* a quality was assigned (range, non-finite, rate-of-change, stuck, time-uncertain, duplicate). The receipt, not just the verdict. |

## Storage

| Term | Definition |
|---|---|
| **Frame** | 68-byte on-flash record: magic + version + length + payload + CRC32. |
| **Segment** | Append-only `.clog` file of frames. Sealed when its tail corrupts; new writes roll to a fresh segment. Oldest segments die under retention. |
| **Watermark** | Highest acknowledged `sequence` for a node, persisted by the sync client after every ack. Lose it and you resend from zero — by design, not by accident. |
| **Retention policy** | Rule deleting oldest segments past a byte budget. At least one segment always survives, even if it's over budget. |
| **Integrity check** | Full rescan counting corrupted/unparsable segments. A number, not a feeling. |

## Networking & Sync

| Term | Definition |
|---|---|
| **Sync batch** | JSON payload with up to N measurements since the watermark. `batch_size` always equals the records actually serialized — padded counts would corrupt server bookkeeping. |
| **Acknowledged sequence** | Highest `sequence` the server confirms persisted. Never fabricated; the E2E suite verifies it against SQLite. |
| **Idempotent replay** | Re-sending acked records after watermark loss. Expected traffic, deduplicated on `(node_id, sequence)`. |
| **Degraded** | Link state: RSSI under threshold, traffic still flowing. Sync keeps going — degraded is not down. |
| **AP fallback** | Node-side access point after repeated STA failures. Hosts the captive portal so a human can still reach the node. |

## Analytics

| Term | Definition |
|---|---|
| **Derived metric** | Any computed statistic (mean, percentile, exposure hours). Tagged `metric_type: derived`, never presented as raw measurement. A mean is an opinion with math. |
| **Before/after evaluation** | Comparison split by an intervention's time window. Under 30 valid samples per period it's marked `sufficient_sample: false` and you stop interpreting. |
| **Exposure hours** | Trapezoidal duration above a temperature threshold between consecutive samples. |
| **Causality disclaimer** | Mandatory note: differences usually mean placement or calibration, not proof of anything. |

## Update & Power

| Term | Definition |
|---|---|
| **OTA release** | Manifest entry `{version, sha256_hex, url, total_size}`. |
| **Reboot pending** | New image verified and staged; the device restarts into it. Rollback uses the alternate-partition boot-counter pattern (hardware bring-up pending). |
| **Sleep advisory** | A recommendation from `SleepPolicy`. Advisory means advisory — never applied automatically. |

## Public presentation

| Term | Definition |
|---|---|
| **Locale labels** | Human strings live only in presentation dictionaries (`I18N` in the node SPA, `LABELS` in the central dashboard). Domain values stay neutral codes (`VALID`, not "Válido") — translating a code starts arguments, not conversations. |
