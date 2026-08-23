# CAUCE Architecture

## Principles

1. **The domain never touches hardware.** All dependencies go through
   `cauce_hal` interfaces. The same domain code runs on ESP32 and host.
2. **Offline-first.** Nothing in the core requires connectivity; sync is an
   extension, not a requirement.
3. **Fail-safe by design.** Invalid data is stored flagged, never silently
   dropped. Corruption is isolated, never propagated.
4. **No blocking delays.** Everything is `tick()`-driven; moving to FreeRTOS
   later is mechanical.

## Layers

```
┌─────────────────────────────────────────────┐
│ main.cpp (composition root)                 │  wires implementations
├─────────────────────────────────────────────┤
│ cauce_app                                   │  scheduler, network FSM,
│                                             │  API router, sync, OTA
├─────────────────────────────────────────────┤
│ cauce_drivers                               │  ISensorDriver → BME280/Simulated
├─────────────────────────────────────────────┤
│ cauce_core                                  │  pure domain: measurement,
│                                             │  validation, storage, config,
│                                             │  logging, security, metrics
├─────────────────────────────────────────────┤
│ cauce_hal (interfaces)                      │  IClock · II2cBus · IFileSystem ·
│                                             │  INetworkController · ISyncTransport
├─────────────────────────────────────────────┤
│ Host impls: Native*/Manual*/Memory*         │  ESP32: Esp32Clock/WireBus/
│ (PC)                                        │  LittleFs (+ radio pending)
└─────────────────────────────────────────────┘
```

Dependencies point downward only. Tests use doubles (`ManualClock`,
`ScriptedI2cBus`, `MemoryFileSystem`, scripted network/sync fakes).

## Measurement flow

```
tick() [scheduler]
  └─ sensor->read(var)                    # concrete driver
       └─ ValidationEngine::evaluate()    # quality + reason bits
            └─ LogStorageRepository::append()   # CRC32 frame, append-only
                 └─ Logger::eventf("MEAS_STORED", ...)
```

Every stage may fail without stopping the cycle: failed read → counter +
MISSING placeholder after 2 consecutive misses; storage down → counter, next
tick retries.

Downstream consumers:

```
ChunkedExporter (CSV/JSON)  ·  Metrics (stats/aggregation/exposure)
```

Both paginate from `IStorageRepository`; no duplicated state.

## State machines

- **Node** (implemented): `BOOT → SENSOR_DISCOVERY → READY ⇄ MEASURING ⇄ STORING`.
- **Network** (logic verified against a scripted controller): `OFFLINE ↔
  WAITING_RETRY ↔ CONNECTING → CONNECTED ⇄ DEGRADED`, AP fallback after N
  failures, exponential backoff 5s→300s.
- **OTA**: IDLE → CHECKING → DOWNLOADING → REBOOT_PENDING with three failure
  states; alternate-partition anti-brick rules (see docs/OTA.md).

## Storage

- Segments `data/meas_NNNNNN.clog`, append-only.
- Frame: `[0xCA][0x01][len u16][60B payload][crc32]` = 72 B.
- Open scans all segments: rebuilds counters, last sequence, latest record;
  a partial corrupt tail seals the segment (writes roll to a new one).
- Size-based rotation; retention removes oldest segments keeping ≥1.

## Configuration

Flat versioned KV (`schema_version=1`), strict validation with enumerated
errors, automatic backup of the previous file before save, load chain
`main → .bak → defaults`. Admin tokens stored only as SHA-256.

## Key decisions

| Decision | Rejected alternative | Why |
|---|---|---|
| Own binary format with CRC | Embedded SQLite | Full format control, demonstrable power-cut tolerance, minimal footprint |
| Flat KV config | Embedded JSON | Zero dependencies, parse without heap, readable diffs; JSON reserved for APIs |
| Bosch integer compensation + float mirror | Float only | Integer version is the vendor reference; the mirror validates transcription |
| `tick()` without RTOS | FreeRTOS tasks now | Domain doesn't need it; migrating later is mechanical |
| Unity single binary on host | On-target tests | Seconds-level feedback, trivial CI |
