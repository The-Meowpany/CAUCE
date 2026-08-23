# CAUCE — Community Microclimate Microstation Network

Offline-first environmental monitoring infrastructure for climate adaptation
in Canelones, Uruguay. Each node is an ESP32-based microstation that
measures, validates, stores locally and serves its data over Wi-Fi without
requiring Internet.

> **Current status**: fully functional firmware core with a green test suite,
> verified ESP32 build, central backend and node↔server E2E integration.
> Read [STATUS.md](STATUS.md) before assuming something exists: the document
> explicitly separates implemented from pending work.

Internal engineering language is **English**. Public-facing experiences are
**localization-ready** (Spanish and English shipped on the node dashboard;
English, Spanish and Portuguese on the central dashboard).

## Philosophy

```
offline-first · local-first · modular · reproducible · observable · fail-safe
```

A failed component never drags the system down:

- Sensor disconnected → `MISSING` recorded, remaining sensors keep working.
- No Internet → node keeps measuring and storing.
- Power cut → records carry CRC; a corrupt partial tail is sealed off.
- Invalid config → rollback to previous backup or safe defaults.

## Layout

```
CAUCE/
├── firmware/            ESP32 firmware (PlatformIO)
│   ├── lib/cauce_hal/      Hardware abstraction (interfaces)
│   ├── lib/cauce_core/     Domain: measurement, validation, storage, config
│   ├── lib/cauce_drivers/  Drivers: simulated + BME280 reference
│   ├── lib/cauce_app/      Scheduler, network FSM, API, sync, OTA
│   ├── src/main.cpp        Composition root (+ host simulation demo)
│   └── test/               Unity suite (97 tests, run on PC)
├── backend/             Central server (FastAPI + SQLite) + pytest suite
├── simulator/           Scenario generator (CSV or direct /v1/sync feed)
├── protocol/v1/         Data protocol specification v1
├── deployment/          docker-compose for the backend
├── docs/                Technical documentation (English)
├── configs/             Example node configuration
├── scripts/             build/test/E2E scripts
└── poolpa/              Unrelated project (do not touch)
```

## Quick start (no hardware)

```powershell
# Requirements: Python 3.10+, PlatformIO (pip install platformio),
# GCC MinGW-w64 on PATH (native tests) — see docs/TESTING.md
cd firmware
pio test -e native          # 97 tests on PC
pio run -e native           # builds host simulation demo
.\.pio\build\native\program.exe   # runs injected-fault scenarios
```

The demo runs ~50 cycles with injected events: frozen sensor, out-of-range,
disconnect, recovery. Watch data quality move from `VALID` to `SUSPECT`,
`MISSING` placeholders being recorded, and the node continuing regardless.

## Backend + full verification

```powershell
cd backend && pip install -r requirements.txt
python -m pytest tests -q          # 20 tests
..\scripts\run-e2e.ps1             # node C++ -> FastAPI -> SQLite (4 phases)
```

`scripts/verify-all.ps1` runs everything in one command.

## Build for ESP32

```powershell
cd firmware
pio run -e esp32dev          # produces .pio\build\esp32dev\firmware.bin
pio run -e esp32dev -t upload    # requires a connected board
```

No physical bench validation yet: the binary compiles and links; on-board
validation is a declared pending item.

## Documentation

| Document | Content |
|---|---|
| [ARCHITECTURE.md](docs/ARCHITECTURE.md) | Layers, modules, decisions |
| [PROTOCOL.md](protocol/v1/PROTOCOL.md) | Binary record format v1 |
| [DATA_MODEL.md](docs/DATA_MODEL.md) | Measurement, quality, storage, export formats |
| [HARDWARE.md](docs/HARDWARE.md) | BOM, I2C wiring |
| [CALIBRATION.md](docs/CALIBRATION.md) | Offset/scale model and honest limits |
| [TESTING.md](docs/TESTING.md) | How to run tests, real coverage |
| [SECURITY.md](docs/SECURITY.md) | Current security posture |
| [API.md](docs/API.md) | Node `/api/v1` endpoints, auth, streaming |
| [SYNC.md](docs/SYNC.md) | Node↔central synchronization contract |
| [BACKEND.md](docs/BACKEND.md) | Central server: API, ingestion, analytics |
| [DASHBOARD.md](docs/DASHBOARD.md) | Local web UI and captive portal |
| [OTA.md](docs/OTA.md) | Firmware update architecture |
| [DEPLOYMENT.md](docs/DEPLOYMENT.md) | Full pilot deployment guide |
| [DOMAIN_GLOSSARY.md](docs/DOMAIN_GLOSSARY.md) | Canonical English vocabulary |

## License

MIT — see [LICENSE](LICENSE).
