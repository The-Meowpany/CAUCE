# CAUCE Roadmap

Where this is going and in what order. `STATUS.md` says what exists
today; this file says what comes next. Time windows stay INCLUSIVE on
both ends, firmware and backend — that decision is closed.

## Principles

- Offline-first, always. A milestone that stops measuring without
  internet is a failed milestone.
- Places first, radios second. Site selection drives the comms choice,
  not the other way around.
- No mesh until proven necessary. Gateways scale; neighbor-routing
  doesn't, not on batteries.
- Small payloads over LoRa, full resolution over Wi-Fi. Each transport
  does what it's good at.

## M0 — Stabilize the base (done)

- `main.cpp` finally wires everything the tests assumed:
  `NetworkManager` + `Esp32WifiController`, sync endpoint and device
  secret from `sync_server_url/sync_device_key`, NTP on first link-up,
  captive-portal DNS retry, sync on Connected/Degraded, OTA ticking on
  a null catalog.
- Backend: one shared `ratelimit.check_rate` (proxy-aware) across API,
  evaluation and dashboard; bearer gate on CSV exports; sanitized
  export filename; `sync_batches.transport` in {wifi, lora};
  before-after windows inclusive without overlap.
- Config: `sync_server_url`, `lora_enabled`, `lora_sync_interval_s`,
  `lora_region`, validated, with an example.
- Docs at 109/32, root `SECURITY.md`, this roadmap.
- i18n: ES/EN in the node SPA, EN/ES/PT on the central dashboard,
  `CAUCE_LANG` in scripts and simulator; docs mirrored in `docs/en` +
  `docs/es`.

Done means: `verify-all.ps1` green plus one board actually syncing
over Wi-Fi. Both true.

## M1 — Physical bench B1–B5 (needs hardware)

Per `BENCH_PLAN.md` with 3–4 nodes: real BME280 reads, LittleFS under
power cuts, Wi-Fi STA/AP roaming, NTP discipline, current profiling.
Expect no code changes beyond tuning thresholds and timeouts — if the
bench demands a rewrite, the host tests were lying.

Done means: a bench log with RSSI, delivery %, consumption per state.

## M2 — LoRa P2P spike, 2 nodes + 1 home gateway (2 wks, cheap)

The goal is link budget on your real sites, not protocol elegance.

- HAL: `ILoRaRadio {send/receive/sleep}` plus `LoRaSyncTransport`
  behind `ISyncTransport::postBatch`, 60 B frame + HMAC, honoring
  `lora_sync_interval_s` and the EU868 duty cycle.
- Policy: one uplink per 10–15 min, SF7–9, hourly aggregates and
  events only. No raw 60 s firehose over LoRa, ever.
- Gateway: an ESP32 + SX1276 forwarding to `POST /v1/sync` with
  `"transport": "lora"`.
- OTA over LoRa is out. Manifest check at most.

Done means: RSSI/SNR vs distance, delivery %, and a documented SF and
period decision.

## M3 — Real LoRaWAN (the recommended path)

- Nodes join via OTAA; per-device keys reuse the `device_key` from
  `POST /v1/provision`. No second key infrastructure.
- Gateway: commercial, or ChirpStack on a Raspberry Pi if you want
  sovereignty. Bridge `ChirpStack MQTT -> POST /v1/sync` with a 68 B
  frame decoder.
- Backend already stores `transport`; add per-transport counters and a
  link-quality log (RSSI/SNR/SF) so gateway placement becomes a data
  decision.
- Firmware: sample every 60 s locally, hourly LoRa aggregate, full
  batch over Wi-Fi; `SleepPolicy` actually used between samples.

Done means: 3 nodes × 7 days in central with `time-reconstruct`
covering the LoRa gaps.

## M4 — 8-node km-scale pilot (frozen in PILOT_SPEC.md)

Pick 8 sites for environmental interest, survey gateway height and
antenna, deploy 1–2 gateways. Everything feeds one database. Add
ChirpStack, backups, and an HTTPS reverse proxy per `DEPLOYMENT.md`.
One prototype through 6 acceptance tests before any ×8 purchase.

Done means: one database, a coverage map from real link logs, and an
ops runbook that isn't fiction.

## M5 — Operations hardening

TLS termination, cursor pagination, the central map, runtime
calibration (`CALIBRATION.md`). Per-node CSV already shipped, and so
did the pieces that decide whether a field deployment is survivable:

- `measurements` retention + `VACUUM`, now on a scheduler as well as
  an endpoint, with the last run visible on `/system`.
- Coverage accounting (`/v1/nodes/{id}/coverage`, site variant, CSV)
  so a before/after can be checked against how much data actually
  arrived.
- Control sites and `difference_in_differences` in `before-after`.
- Field diagnostics bundles, ingested centrally, plus `/v1/fleet`
  answering which node needs a visit.
- `simulator/load_pilot.py` to rehearse pilot volume.

Still open here:

- **OTA rollback on hardware.** The decision FSM and the rollback
  policy are host-tested and the two-slot partition table ships, but
  nobody has flashed a board, marked an image valid, or watched a bad
  image roll back. Until that happens, updates need physical access.
- TLS termination, cursor pagination, async OTA.

## Non-goals for now

Full mesh routing, firmware OTA over LoRa, downlink control loops,
MQTT/CoAP on constrained nodes, multi-region LoRaWAN roaming. Each of
these has been suggested at least once; each waits its turn.
