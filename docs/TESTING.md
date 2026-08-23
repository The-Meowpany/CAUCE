# CAUCE Testing

## Run

```powershell
cd firmware
pio test -e native          # 102 tests on PC (Unity), no board needed
cd ..\backend
python -m pytest tests -q   # 26 backend tests
..\scripts\run-e2e.ps1      # node C++ ↔ FastAPI ↔ SQLite, 4 phases
```

Requirements: MinGW-w64 (GCC ≥9) on PATH for the `native` env; Linux CI uses
stock gcc. Host doubles: `ManualClock`, `ScriptedI2cBus`,
`MemoryFileSystem`, scripted network/sync fakes.

## Suites (firmware/test/)

| File | Covers |
|---|---|
| `test_validation.cpp` (9) | Physical range, non-finite, rate-of-change, frozen sensor, duplicates, time uncertainty |
| `test_codec.cpp` (5) | CRC32 known vector, full roundtrip, bit-flip detection, magic/version/length |
| `test_storage.cpp` (6) | Append+query, paginated skip, reboot recovery, **corrupt-tail isolation with sealing**, rotation, retention |
| `test_config.cpp` (9) | Defaults, KV roundtrip, geo NAN, thresholds, save/load, backup restore, validations, garbage input |
| `test_security.cpp` (6) | SHA-256 NIST vectors + constant-time compare |
| `test_bme280.cpp` (5) | Datasheet temperature vector, integer-vs-mirror pressure/humidity agreement, full I2C read with simulated bus, disconnected sensor |
| `test_scheduler_integration.cpp` (5) | Full measure→validate→store cycle, sequence continuity across reboots, MISSING on disconnect + recovery, storage failure without crash (`FailingFileSystem`), time uncertainty during outage |
| `test_export.cpp` (6) | CSV header+rows+ISO-8601, RFC4180 escapes, typed closed JSON array, empty `[]`, chunking ≡ single-shot, time-range filter |
| `test_metrics.cpp` (8) | Known-vector stats, variable/quality/NaN filtering, windowed aggregation with capacity bounds, exposure hours |
| `test_network.cpp` (8) | Full FSM: disabled, connect, retry backoff 5s/10s, AP fallback after N attempts, timeout, degraded↔connected by RSSI, AP without credentials |
| `test_sync.cpp` (10) | Persisted watermark, resume **without duplicates**, watermark loss → idempotent resend, auth backoff, halt-on-reject, network gating, payload format |
| `test_ota.cpp` (10) | Semver pairs, streaming sha256 ≡ one-shot, happy path (single catalog fetch), hash mismatch abort, size exceeded, same version skip, no release, installer rejection, heap gate, battery gate |

## Backend suites (backend/tests/test_api.py)

Ingestion idempotency/resume/atomicity · token auth (constant-time path,
fail-closed) · rate limiting incl. bounded memory · nodes/measurements
filters · analytics vectors (summary/compare/before-after/period/
heat-events) · dashboard localization via Accept-Language · CSV export.

## End-to-end integration

```powershell
.\scripts\run-e2e.ps1
```

Starts a real FastAPI server on an ephemeral port, runs the `integration`
binary — the actual C++ `SyncManager` with real socket HTTP — and verifies
directly in SQLite: initial batch (50), incremental (+5), full idempotent
replay after watermark deletion, then `rows == distinct == 55`. This E2E has
already caught one real protocol bug (zero-padded `batch_size`).

## Philosophy

- Everything that can fail in the field has a failure test.
- BME280 math validated by two independent implementations plus the vendor
  vector.
- Scenario simulation complements tests for long-running behaviors.
