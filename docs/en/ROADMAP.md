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
- **TLS termination** via `deployment/Caddyfile`: automatic internal
  CA, domain from `CAUCE_DOMAIN`, and the central bound to
  `127.0.0.1` so only the proxy reaches it.
- **Cursor pagination** on measurements (`timestamp_utc_ms, sequence`)
  and nodes (`node_id`), so a long export stays consistent while the
  fleet keeps writing. `limit`/`offset` still work.
- **Async OTA download**: the firmware reader returns a tri-state per
  chunk instead of sleeping 10 s waiting for bytes, and the manager
  gives up with `OTA_STALLED` rather than blocking the scheduler
  forever. The initial HTTP open is still blocking.
- **Deep sleep policy**: `DeepSleepController` decides when it is
  allowed to sleep (not while pending data is unsynced, not while OTA
  runs, not below the battery gate). It ships **disabled by default**
  because nobody has measured the power yet.
- **Long-window analytics**: `granularity=auto` answers a 30-day
  request from the `agg_hourly` buckets instead of streaming every raw
  row, and says in the response which granularity it used.
- **Downlink, idempotent by construction.** The central queues a command
  per `(node_id, idempotency_key)` and hands it to the node in the same
  `/v1/sync` response that acknowledges a batch, so it costs no extra
  radio wakeup. The node remembers applied ids in flash and reports a
  receipt; the central re-offers the command until it arrives. Delivery
  is honest: `pending` means nobody confirmed it, `expired` means it aged
  out, and neither is quietly reported as done. Kinds shipped are
  validation-only (`set_sampling_interval`, `set_sync_interval`,
  `request_resync`, `set_led_mode`) — they check their arguments and
  report, but do not reconfigure the running node, because a command
  that could stop a node reporting is one nobody should send by accident.
  Real actuation waits for hardware that can be actuated.
- **OTA rollback now wired.** `OtaBootConfirm` keeps a boot-attempt
  counter in flash and marks the image valid once the node proves it can
  store a measurement, or rolls back after three bad boots. Without it
  an image that crashed on the first tick would stay installed forever.

Still open here:

- **OTA rollback on hardware.** The policy, the counter and the ESP32
  partition calls are wired and host-tested, but nobody has flashed a
  board, marked an image valid, or watched a bad image roll back. Until
  that happens, updates need physical access.
- **Calibration uncertainty.** The central applies offsets and
  scales correctly, but nothing carries an uncertainty estimate and
  there is no formal procedure. Until then a calibrated reading is a
  better relative comparison, not a traceable measurement.
- **Deep sleep power measurement.** The policy is tested; the savings
  are not.
- A real certificate if the central is ever exposed publicly — the
  deployment gets ACME by default now, so this is a DNS problem, not a
  code one.
- LoRa still sends raw JSON within the payload budget instead of the
  68-byte frame M2 describes. Acknowledgement is now real, but the
  compact encoding and its fragmentation are not written.

## Non-goals for now

Full mesh routing, firmware OTA over LoRa, MQTT/CoAP on constrained nodes,
multi-region LoRaWAN roaming, asymmetric signatures. Each has been
suggested at least once; each waits its turn.

Two of them changed status when the interfaces were examined:

- **Downlink control loops** were a non-goal and are no longer one, at the
  transport level. `ILoRaRadio` and `ISyncTransport` were both
  send-only, which is what actually blocked it; both now have a receive
  path. What shipped is delivery and idempotency, not actuation: see M5.
- **Peer-to-peer exchange** is still a non-goal, and adding `receive()`
  did not change that. A radio that can hear a neighbour is not a mesh;
  it is the prerequisite for one, and the reason it is cheap to defer
  is that the sync path is already idempotent.

Ed25519 stays out for a concrete reason rather than a taste one: the
firmware has SHA-256 and HMAC and no bignum, so curve arithmetic would be
written from scratch on a part with no room to review it. Symmetric
per-device keys plus TLS cover the pilot threat model; a signature scheme
is worth it when there is a key-distribution problem to solve that
symmetry cannot.
