# Physical Bench Validation Plan

The entire automated suite runs on host with test doubles. This plan closes
that gap before any field deployment. Minimum rig: **3–4 nodes**, one
controllable power source (relay + programmable cut), one Wi-Fi AP with
traffic shaping, environmental chamber or at least controlled
temperature/humidity exposure, and a reference thermometer co-located.

## B1. Flash integrity under real power cuts

- Fill storage near budget (≥60 KiB across segments).
- Execute 200 randomized power-cut events during active append windows.
- After each boot: `GET /api/v1/health` → assert `corrupted_frames` matches
  expectation, latest sequence is contiguous (max+1 rule), and every queried
  record passes CRC implicitly.
- Compare observed corruption granularity against the frame model.

## B2. Sensor fidelity vs datasheet model

- 72 h soak across ≥10 °C ambient swing with reference probe.
- Assert |node − reference| within datasheet tolerance band; log drift curve.
- Condensation exposure cycle: verify driver error states surface as
  `INVALID`/`MISSING` instead of plausible garbage.

## B3. Radio endurance

- 14-day continuous STA operation: track reconnects, RSSI drift, heap
  high-water (`/api/v1/health`), unexpected reboots.
- AP-fallback drill: kill upstream link repeatedly; verify FSM reaches
  `AP_FALLBACK`, dashboard reachable via portal DNS, and recovery to STA.

## B4. Sync under real network faults

- Repeat the host E2E matrix against the live central using physical nodes:
  packet loss ≥30 % (tc/netem), server restarts mid-batch, watermark file
  deletion on device flash.
- Acceptance: zero duplicates in central SQLite after each scenario
  (`rows == distinct`), monotonic acks, halt behavior only on semantic
  rejection.

## B5. Energy characterization

- Measure average current in three modes: always-on (today), duty-cycled
  radio, deep sleep between cycles (when implemented).
- Validate `SleepPolicy` recommendations against measured data; publish the
  solar/battery sizing table derived from measurements.

## Exit criteria

All five blocks executed with results recorded in this repository
(`docs/BENCH_RESULTS.md` template to be created per run). Any discrepancy
between bench and host-model predictions becomes a tracked issue before
field expansion.
