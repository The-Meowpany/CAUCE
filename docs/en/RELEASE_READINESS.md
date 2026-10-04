# Release readiness: what stands between this and a frozen product

`STATUS.md` says what exists. `ROADMAP.md` says what the product will grow into.
This file answers a narrower question: **what is still missing before the feature
set can be declared closed and the result handed over as a fixed thing.**

## The vocabulary, since it was asked

The sequence has standard names, and they are not the same thing:

| Term | Meaning |
|---|---|
| **Feature freeze** | The date after which no new capability enters. Work continues only on defects. |
| **Code freeze / RC** | Release candidate: the tree is cut, only fixes against it. |
| **GA (general availability)** | The release as sold. |
| **Baseline** | The frozen artefact set: firmware image, partition table, backend version, schema version, docs. *This* is what people mean by "the version we deployed". |
| **LTS / maintenance-only** | Frozen features, but security and critical defects still patched for a stated period. |
| **EOL** | No further patches of any kind. |

The honest framing: **you can freeze the feature set, you cannot freeze the
absence of patches.** This product terminates TLS, verifies Ed25519 signatures and
writes its own flash. A build with no patch channel is a liability, not a finished
product. So the target state below is a **frozen baseline under maintenance-only
(LTS)**, with EOL defined as a date rather than declared at handover.

---

## Phase 0 — Freeze the specification

Nothing is built until the non-goals are written down, because "complete" is
undefined without them.

- [ ] A one-page statement of what the product does **not** do, per subsystem.
- [ ] Decide the two questions that change the hardware bill:
  - **Is ESP-NOW/mDNS peer-to-peer in the baseline?** The merge and the interfaces
    exist; the radio does not. Either fund it or defer it explicitly (see below).
  - **Does the LoRa path ship?** It is host-tested end to end but has no radio
    driver and no link budget, so the air interface is unproven.
- [ ] Freeze the on-wire formats: LoRa frame, sync envelope, REST resources,
  storage frame, partition table. From that point they are versioned, not edited.
- [ ] Freeze the database schema and commit to migration discipline from here on.

**Gate:** a reviewer who has never seen the project can say, from one page, what
the product is and is not.

---

## Phase 1 — Close the software gaps (desk only)

These need no hardware. All of them are real, and all of them are visible in
`STATUS.md`.

### 1.1 Ed25519 in the transport — **required**

`ed25519Sign` / `ed25519Verify` exist and pass RFC 8032 vectors, but the firmware
transport signs with **HMAC-SHA-256 only**. The central already supports both
algorithms per node (`device_key_algorithm`). Until the firmware can select, every
node is symmetric and the asymmetric story in the thesis is not true of shipped
devices.

- [ ] `LoRaSyncTransport` takes an algorithm and a seed, not a shared key.
- [ ] Frame trailer is 32 bytes for HMAC, 64 for Ed25519, and the central's
      existing length dispatch is exercised by a test for each.
- [ ] Signing measured on the target: an ESP32 signature is milliseconds, not
      microseconds. Establish the number and put it in `docs/en/SECURITY.md`.

### 1.2 Downlink actuation

Downlink kinds validate and report but do not reconfigure a node.

- [ ] `relay` / `sample_interval` / `calibrate` actually applied and persisted.
- [ ] Acknowledgement reports the applied value, not the requested one.

### 1.3 Calibration as a method, not an overlay

Records carry an absolute uncertainty and scale it with the correction. There is no
formal procedure and no traceability, so a number cannot be defended.

- [ ] Written procedure per quantity: reference, method, number of points,
      acceptance criterion, interval.
- [ ] Uncertainty budget per quantity, propagated into the report — already
      plumbed, needs the budget filled in.
- [ ] Certificates or reference records retained so a calibration is auditable.

### 1.4 Coverage completeness

A query is capped at 400 days and reports the top 20 gaps with `gaps_truncated`
set. Bounded, not complete.

- [ ] Decide whether the cap is acceptable for a pilot and write that decision
      down, or paginate the gaps endpoint.
- [ ] Surface `gaps_truncated` in the dashboard, not only in the API.

### 1.5 Test binary isolation

One Unity binary for every suite, isolated by data directories. Works today, but a
suite that leaks state will eventually pass for the wrong reason.

- [ ] Split per-suite binaries, or add a teardown that provably resets state.

### 1.6 Central operations

- [ ] A real certificate and DNS name. `deployment/` ships Caddy with an internal
      CA; that is fine on a trusted LAN and not fine on the internet.
- [ ] Backup and restore, exercised at least once for real. An untested backup is
      a hypothesis.
- [ ] Schema migrations from this point forward must be reversible and numbered.
- [ ] Rate limiting and retention already work; confirm the retention caps match
      the pilot's expected volume (`docs/en/PILOT_SPEC.md`).

**Gate:** `scripts/verify-all.ps1` green, plus a signed release note and an updated
`SECURITY.md`.

---

## Phase 2 — Close the air interface

### 2.1 LoRa radio and link budget — **required if LoRa ships**

- [ ] SX1276 driver behind the tested `ILoRaRadio` interface.
- [ ] Link budget written out: spreading factor, bandwidth, payload, airtime per
      frame, duty cycle, and the worst-case number of nodes per gateway.
- [ ] Measured, not calculated: RSSI, SNR, packet error rate at range.
- [ ] Regulatory: duty-cycle limits and band certification for the deployment
      region (`docs/en/LEGAL.md`).

### 2.2 ESP-NOW / mDNS peer-to-peer — **decide before Phase 0 closes**

`Replication.h` and `IPeerLink.h` exist and are tested; the transport does not.

- [ ] Ship it: `IPeerDiscovery` over mDNS, `IPeerRadio` over ESP-NOW, and the
      exchange loop wired into `main.cpp`.
- [ ] Or defer it. Deferring is defensible — the thesis itself argues a mesh is not
      required — but then it must be deferred *explicitly*, not left as an
      interface with no implementation.

### 2.3 Deep sleep enabled by default

Wired but disabled pending the bench measurement. Cannot be enabled in Phase 0-2.

---

## Phase 3 — Bench validation (hardware only)

`docs/en/BENCH_PLAN.md` is the procedure. What is missing is the **result**. Every
item must record measured numbers, not "works".

| # | Item | Pass criterion |
|---|---|---|
| B1 | BME280 over real Wire | Reads match a reference instrument within the stated uncertainty |
| B2 | LittleFS on real flash | Mount, fill, power-cut mid-write, recover; wear figure recorded |
| B3 | Wi-Fi radio | Reconnect after AP loss and after power cycle; measured time to first sync |
| B4 | OTA on real flash | Write, boot, confirm, and roll back on a deliberately bad image |
| B5 | NTP time source | Offset measured against a reference clock; behaviour when unreachable recorded |
| B6 | LoRa radio | Link budget as in 2.1, at range |
| B7 | Power | Deep-sleep current measured; the resulting interval is what Phase 2.3 enables |
| B8 | Enclosure and thermal | Sensor reads its ambient, not its own heat |

**Gate:** B1-B8 recorded with numbers in `BENCH_PLAN.md`. Until then the honest
status is "host-tested, unproven on hardware", which is what `STATUS.md` says today.

---

## Phase 4 — Manufacturing and provisioning

Not started, and it is the phase most likely to be forgotten until it is urgent.

- [ ] **Per-device seed injection at end of line.** Each node gets its own Ed25519
      seed, generated on the line, never shared between units.
- [ ] **How the seed is protected.** It is on the device and it is not extractable
      in software. State plainly in `SECURITY.md` what a physical attacker gets.
- [ ] **What happens when a seed is lost.** A lost seed means a lost identity: the
      node must be re-provisioned and the old identity revoked at the central.
- [ ] **Factory test.** One command that proves a unit works before it ships:
      sensor read, storage write, Wi-Fi join, signed sync, signed downlink.
- [ ] **Provisioning manifest.** Device id, seed, algorithm and site recorded once,
      in a file that can be re-read during support.
- [ ] **Revocation and rotation.** The central needs a way to retire a compromised
      device. It does not exist yet.

---

## Phase 5 — Release engineering

- [ ] Semantic versioning applied, with the schema and the wire formats as
      explicit major-version boundaries.
- [ ] A release tag, and the artefact set pinned: firmware `.bin`, `partitions.csv`,
      backend image, fixture files, docs.
- [ ] SBOM for the backend dependencies (`cryptography` is the one that matters).
- [ ] Reproducible backend build, or at least a recorded lockfile hash.
- [ ] **A release gate script** that refuses to tag unless: `verify-all.ps1` green,
      firmware tests green on the target configuration, backend green, E2E green,
      docs index consistent with the tree.
- [ ] A runbook: what to do when a node goes silent, when the central is down,
      when a sync backlog grows, when a bad image ships.

---

## Phase 6 — Freeze, then maintenance-only

The declaration, and it should be a single document:

1. **Feature freeze is in force.** New capability enters `v2`, not `v1`.
2. **The baseline is identified**: version, git tag, artefact hashes, schema
   version, wire format versions.
3. **LTS window is stated**: how long security and critical defects are patched,
   and who does it.
4. **EOL date is stated.** After it, no patches, and the central stops accepting
   those nodes. Say this up front rather than discovering it.
5. **The frozen list of deferred items is published**, so nobody assumes they were
   forgotten.

---

## Explicitly deferred to v2

Deferral is a decision, not an omission. Each of these is a reasonable thing *not*
to have in a frozen baseline, provided it is written down:

- Peer-to-peer mesh (if 2.2 defers it) — the thesis argues it is not needed
- Volumetric 3D visualisation — already discarded by design
- LoRa downlink actuation of physical actuators — needs actuated hardware
- Formal metrological traceability beyond Phase 1.3
- Multi-tenant central, if a single operator is the target

---

## The gate, in one place

A frozen baseline requires all of:

- [ ] Phase 0 signed off: non-goals written
- [ ] Phase 1 complete: firmware signs with Ed25519, downlink actuates,
      calibration has a procedure, central has a real certificate and a tested
      restore
- [ ] Phase 2 decided: LoRa shipped with a measured link budget, or deferred in
      writing; ESP-NOW shipped or deferred in writing; deep sleep enabled with a
      measured current
- [ ] Phase 3 complete: B1-B8 recorded with numbers
- [ ] Phase 4 complete: per-device seeds, factory test, revocation
- [ ] Phase 5 complete: tagged baseline, SBOM, release gate script, runbook
- [ ] `scripts/verify-all.ps1` green on the exact tagged tree
- [ ] LTS window and EOL date published

Anything unchecked above means the honest answer is "a very good prototype", not a
finished product. That is a fine thing to be. It is not the same thing.