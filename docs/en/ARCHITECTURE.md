# CAUCE Architecture

Four rules run this codebase:

1. **The domain never touches hardware.** Everything physical goes
   through `cauce_hal` interfaces, so the same domain code runs on an
   ESP32 and on your PC. When a test passes on host, it tested the real
   logic — only the drivers get swapped.
2. **Offline-first.** Connectivity is an extension, not a requirement.
   Nothing in the core waits for a network.
3. **Fail-safe, not fail-silent.** Bad data gets stored with a flag, and
   corruption gets fenced off. The worst thing this firmware does with a
   failure is count it.
4. **No blocking delays.** Everything runs on `tick()`. If we ever move
   to FreeRTOS, it'll be mechanical, not a rewrite.

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
│                                             │  INetworkController · ISyncTransport ·
│                                             │  ILoRaRadio
├─────────────────────────────────────────────┤
│ Host impls: Native*/Manual*/Memory*         │  ESP32: Esp32Clock/WireBus/
│ (PC)                                        │  LittleFs/WifiController (+ radio)
└─────────────────────────────────────────────┘
```

Dependencies point down only. Tests plug in doubles (`ManualClock`,
`ScriptedI2cBus`, `MemoryFileSystem`, scripted network/sync fakes) —
that's how 315 tests run in under a minute with no board attached.

## Measurement flow

```
tick() [scheduler]
  └─ sensor->read(var)                    # concrete driver
       └─ ValidationEngine::evaluate()    # quality + reason bits
            └─ LogStorageRepository::append()   # CRC32 frame, append-only
                 └─ Logger::eventf("MEAS_STORED", ...)
```

Any stage can fail mid-cycle without stopping the loop: a failed read
bumps a counter (two misses in a row records a MISSING placeholder); a
dead storage bumps another counter and the next tick just tries again.

Downstream, two consumers read from the same repository:

```
ChunkedExporter (CSV/JSON)  ·  Metrics (stats/aggregation/exposure)
```

Both page through `IStorageRepository`. Nobody keeps a second copy of
the data.

## State machines

- **Node**: `BOOT → SENSOR_DISCOVERY → READY ⇄ MEASURING ⇄ STORING`.
- **Network** (logic proven against a scripted controller):
  `OFFLINE ↔ WAITING_RETRY ↔ CONNECTING → CONNECTED ⇄ DEGRADED`, AP
  fallback after N failures, exponential backoff 5s→300s. The ESP32
  wiring (`Esp32WifiController`, NTP discipline, portal-DNS retry)
  landed in `main.cpp` — the FSM finally drives a real radio.
- **OTA**: IDLE → CHECKING → DOWNLOADING → REBOOT_PENDING with three
  failure states; alternate-partition anti-brick rules (see OTA.md).

## Deployment (pilot)

```mermaid
flowchart TB
    U[Phone / Laptop<br/>http://192.168.4.1] --> N1 & N2 & N3
    subgraph Central["Central (optional)"]
        C[FastAPI + SQLite<br/>dashboard + analytics]
    end
    N1["CAUCE-001<br/>BME280"] -. HMAC-signed batches .-> C
    N2["CAUCE-002"] -. .-> C
    N3["CAUCE-003<br/>...010"] -. .-> C
    N1 --- N2
    N2 --- N3
    classDef node fill:#182430,stroke:#39c2a7,color:#e8eef4
    class N1,N2,N3 node
```

## Storage

- Segments `data/meas_NNNNNN.clog`, append-only, always.
- Frame: `[0xCA][0x01][len u16][60B payload][crc32]` = 68 B (4 header +
  60 payload + 4 CRC).
- Opening scans every segment: counters, last sequence and latest
  record get rebuilt from what's actually on flash. A half-written tail
  seals its segment and writes roll over to a fresh one — that's the
  whole power-cut story.
- Rotation by size; retention deletes oldest segments but always keeps
  at least one.

## Configuration

Flat versioned KV (`schema_version=1`) with strict validation and a
list of exactly what's wrong, automatic backup of the previous file on
every save, and a load chain of `main → .bak → defaults`. Admin tokens
live on flash as SHA-256 only.

## Key decisions

| Decision | Rejected alternative | Why |
|---|---|---|
| Own binary format with CRC | Embedded SQLite | We control every byte, power-cut behavior is demonstrable, footprint stays tiny |
| Flat KV config | Embedded JSON | No dependencies, parses without heap, diffs read well; JSON stays at the APIs |
| Bosch integer compensation + float mirror | Float only | The integer math is the vendor reference; the mirror caught transcription slips |
| `tick()` without RTOS | FreeRTOS tasks now | The domain doesn't need them; adding them later is plumbing |
| Unity single binary on host | On-target tests | Feedback in seconds, CI stays trivial |
