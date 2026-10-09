# CAUCE Testing

## Run

```powershell
cd firmware
pio test -e native          # 461 tests on PC (Unity)
pio run -e native           # builds host simulation demo
pio run -e esp32dev         # builds target firmware
```

```bash
cd backend
python -m pytest tests -q   # 600 backend tests
..\scripts\run-e2e.ps1      # node C++ ↔ FastAPI ↔ SQLite (3 phases)
```

## Suites (firmware/test/)

| File | Covers |
|---|---|
| `test_validation.cpp` (9) | Physical range, non-finite, rate-of-change, frozen sensor, duplicates, time uncertainty |
| `test_codec.cpp` (5) | CRC32 known vector, roundtrip, bit-flip detection, magic/version/length |
| `test_storage.cpp` (6) | Append+query, skip pagination, reboot recovery, **corrupt-tail isolation with sealing**, rotation, retention |
| `test_config.cpp` (9) | Defaults, KV roundtrip, NAN geo, thresholds, save/load, backup restore, validation, garbage input |
| `test_security.cpp` (6) | NIST SHA-256 vectors + constant-time compare, RFC 4231 HMAC-SHA256 |
| `test_bme280.cpp` (5) | Temperature vs datasheet, pressure/humidity agreement, I2C driver on simulated bus, disconnected sensor |
| `test_scheduler_integration.cpp` (5) | Full measure→validate→store cycle, sequence continuity, MISSING on disconnect+recovery, storage failure without crash, time uncertainty |
| `test_lora.cpp` (4) | Payload budget, min-interval duty gate, dead radio, optimistic ack |

## Backend suites (backend/tests/test_api.py)

Ingestion idempotency/resume/atomicity · token auth (constant-time,
fail-closed) · rate limiting + bounded memory · nodes/measurements
filters · analytics (summary/compare/before-after/period/heat-events) ·
dashboard localization (en/es/pt) · CSV export · `transport` field
(wifi/lora).

## Simulator

```bash
python simulator/generate_scenarios.py --outdir data/sim --days 2
```sh
python simulator/generate_scenarios.py --outdir data/sim --days 2
python simulator/generate_scenarios.py --sync-url http://localhost:8000/v1/sync --days 1
```

The series starts at yesterday's midnight UTC, so anything asking for recent data finds it.
`--base-ts 1787356800000` pins the fixed date the generator used to hardcode, for when
byte-identical output matters more than being current.


Writes CSVs, or feeds the backend directly over `/v1/sync`. Each
timestamp emits air_temperature, relative_humidity, pressure,
illuminance and battery_voltage. English by
default, Spanish with `CAUCE_LANG=es`.

## Philosophy

- Everything that can break in the field has a failure test. If a bug
  class has no test, that's the next test to write.
- The BME280 math is checked by two independent implementations plus
  the vendor vector — because one transcription of a datasheet is how
  subtle bugs are born.
- Scenario simulation covers the long behaviors tests can't afford
  (freezing, disconnects, recovery over dozens of cycles).
