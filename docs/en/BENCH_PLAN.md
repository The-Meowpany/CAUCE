# Physical Bench Validation Plan

Every automated test runs on host with doubles standing in for
hardware. This plan closes that gap before anything goes to the field.
Minimum rig: **3–4 nodes**, one controllable power source (relay +
programmable cut), one Wi-Fi AP with traffic shaping, an environmental
chamber — or at least controlled temperature/humidity exposure — and a
reference thermometer sitting next to the nodes.

## B1. Flash integrity under real power cuts

- Fill storage near budget (≥60 KiB across segments).
- Fire 200 randomized power cuts during active append windows.
- After each boot: `GET /api/v1/health` → `corrupted_frames` must match
  the expectation, the latest sequence must be contiguous (max+1 rule),
  and every queried record must pass CRC implicitly.
- Compare the observed corruption granularity against the frame model.
  If reality disagrees with the model, the model loses.

## B2. Sensor fidelity vs datasheet model

- 72 h soak across at least a 10 °C ambient swing, reference probe
  alongside.
- |node − reference| must stay inside the datasheet tolerance band;
  log the drift curve either way.
- Condensation cycle: driver error states have to surface as
  `INVALID`/`MISSING`, not as plausible-looking garbage. A wrong
  number you believe is worse than no number.

## B3. Radio endurance

- 14 days continuous in STA: count reconnects, watch RSSI drift and
  heap high-water (`/api/v1/health`), note every unexpected reboot.
- AP-fallback drill: kill the upstream link repeatedly. The FSM must
  reach `AP_FALLBACK`, the dashboard must answer through portal DNS,
  and the node must find its way back to STA.

## B4. Sync under real network faults

- Run the host E2E matrix again, this time against the live central
  with physical nodes: ≥30 % packet loss (tc/netem), server restarts
  mid-batch, watermark file deleted from device flash.
- Acceptance: zero duplicates in central SQLite after each scenario
  (`rows == distinct`), monotonic acks, halt only on semantic
  rejection. Same bar as host, no discount for hardware.

## B5. Energy characterization

- Measure average current in three modes: always-on (today),
  duty-cycled radio, deep sleep between cycles (once implemented).
- Check `SleepPolicy` recommendations against the measured numbers and
  publish the solar/battery sizing table derived from measurements —
  not from hopes.

## Exit criteria

All five blocks run, results recorded in this repository
(`BENCH_RESULTS.md` template to be created per run). Any mismatch
between bench and host-model predictions becomes a tracked issue
before field expansion. No green bench, no field.
