# CAUCE — Frozen Pilot Spec

Technical annex. The funding application carries only Meta and Result
(section 1); everything else is build specification.

## 1. Form (evaluator version)

**Goal:** develop and validate a pilot network of eight autonomous
environmental microstations capable of producing hyperlocal, comparable
information on microclimate conditions in Canelones.

**Result:** eight calibrated, operating stations generating
environmental records through the pilot period.

## 2. Frozen scope

8 identical nodes · 1 gateway · 1 CAUCE-Node carrier PCB · 1 data
protocol · 1 calibration scheme · 1 local storage system · 1 LoRa
point-to-point link · 1 central API · 1 microclimate viewer.

Golden rule: **the node never depends on the gateway to measure.**

Purchase rule: **no critical component gets bought ×8 until the
prototype passes acceptance** (this is mostly about the anemometer:
buy one, validate it, then buy the other seven).

The project's causal chain:
**measure → validate → calibrate → preserve → transmit → compare →
visualize → generate evidence.** No physical intervention is committed
with these funds.

## 3. Frozen architecture

```text
                    ┌───────────────┐
                    │    Sensors    │
                    └───────┬───────┘
                            ↓
                       ┌─────────┐
                       │  ESP32  │
                       └────┬────┘
                            │
               ┌────────────┴────────────┐
               ↓                         ↓
          LittleFS                    microSD
       operational buffer         historical archive
               │                         │
               └────────────┬────────────┘
                            ↓
                          LoRa
                            ↓
                         Gateway
                            ↓
               lora_gateway → POST /v1/sync
                            ↓
                           API
                            ↓
                 Processing engine
                            ↓
                2D schematic microclimate map
                 + uncertainty
```

LittleFS is the operational buffer; microSD is the historical archive.
If the SD fails (mount, write, corruption), the node keeps running on
LittleFS and raises a health flag: no storage failure ever stops
measurement. The gateway aggregates; it is not the source of truth —
the node always keeps its data and the server consolidates.

Project language: "microclimate map (2D schematic with interpolated
field and explicit uncertainty)". Wind: "per-station speed
and direction → interpolated vector field". Volumetric 3D rendering is
discarded by design decision. Anything grander has to
earn its wording with node density first.

## 4. Node (×8 identical)

ESP32-WROOM-32 + SX1276/RFM95W at 915 MHz (AU915, the Uruguayan band) ·
BME280 (temperature, humidity, pressure) · BH1750 (illuminance — lux,
not solar irradiance) · BPW34 photodiode with transimpedance stage
(relative solar irradiance) · SPS30 (PM1, PM2.5, PM4, PM10) · MH-Z19C
(NDIR CO₂, automatic baseline correction off, zeroed at co-location) ·
VEML6075 (UV index with separate UVA and UVB) · AS3935 (lightning count
and distance) · cup anemometer + resistor-ladder vane + tipping-bucket
rain gauge (one kit validated before buying seven) · capacitive soil
moisture probe, gypsum block (soil water potential, read with alternating polarity
through an MCP23008 expander) and DS18B20 (soil
temperature) · leaf wetness grid · DS3231 (±2 ppm; NTP > DS3231 >
internal hierarchy) · microSD 16 GB high-endurance (CSV archive) · 20 W panel, 12 V 7 Ah battery, 10 A controller, DC-DC, fuse,
TVS · IP65 box with cable glands · 915 MHz antenna + pigtail · printed
PETG radiation screen + mast (2 m installation protocol, WMO practice).

Every sensor has a driver in the firmware and a config key that enables
it; the pin map is in `HARDWARE.md`. Drivers are host-tested against
scripted buses and have not yet run on a board.

## 5. Prototype BOM ×1 (~USD 255)

ESP32 3 · SX1276 + antenna 7 · BME280 7 · SPS30 36.12 · MH-Z19C 24.99 ·
VEML6075 6 · BH1750 1.50 · BPW34 stage 3 · wind/rain kit 34 · soil
probes 9 · gypsum excitation expander 1.50 · leaf wetness 3 · AS3935 12 · DS3231 3 · microSD 6 · panel 28 ·
battery 27 · charge controller 7.64 · regulation and protection 5 ·
box 7.39 · printed screen 4 · PCB and connectors 12 · mast 8.
Total USD 256.14.

## 6. Budget (USD 4,000)

| Item | USD |
|---|---|
| 8 complete stations | 2,049.12 |
| LoRa point-to-point gateway (ESP32 + SX1276) | 60.00 |
| Central server | 90.00 |
| Tools, fabrication and bench rig | 501.00 |
| Calibration and co-location | 30.00 |
| Workshops and educational sessions | 480.00 |
| Installation and local logistics | 140.00 |
| International transport and import (provision) | 400.00 |
| Critical spares | 78.11 |
| Contingency, dual purpose | 171.77 |
| **Total** | **4,000.00** |

The contingency (4.3 %) has two uses, in order. Until stage 4 closes (all
eight nodes bought and imported) it covers what ends up costing more than
planned: price or exchange-rate differences, import above the provision,
the DFRobot AS3935 if the generic one fails its sample test, an antenna for
a site the gateway cannot reach, failed parts beyond the spares. Whatever is
left then funds, before co-location, a traceable calibration of the bench
reference instrument (or a certified reference if none is lent), which is
what turns co-location from relative consistency into absolute bias; then a
second SPS30 spare. It is funded by printing on a Bambu Lab A1 mini (USD 271)
instead of the A1, which now costs USD 441.

About half of the budget (USD 1,947) still rests on estimates with no price
checked, so a risk-weighted contingency would be about USD 489. The budget
document covers that in layers - the contingency, the conditional catering
line, and the adjustment levers - and requires quotes for its five heaviest
estimated lines (wind and rain kit, battery, workshop and installation
travel, teaching materials) before the stage 4 purchase.

The import provision covers an effective cost of up to ~30 % on roughly
USD 1,368 of imported goods; the budget document lists the adjustment
levers if the real figure is higher. No physical interventions line:
the project generates evidence to evaluate measures, it doesn't execute
them.

What remains afterwards: hardware, firmware, protocol, data,
calibration methodology, platform.

## 7. Calibration

Two-week co-location of all 8 nodes before deployment; individual
`cal_*` coefficients per node and variable; a divergence report.
Reference first: try to borrow a pattern instrument (university /
agency). If that fails, eight-way intercomparison: **that establishes
relative consistency, not absolute bias**, and the scientific docs
must say so (a bias shared by all nodes would be invisible).
Periodic field re-validation by neighbor comparison; sustained
divergence = maintenance flag, not silent data.

## 8. Prototype acceptance (pass / fail)

| Test | Bar |
|---|---|
| 14-day energy balance | Positive measured daily balance + recorded reserve (design hypothesis ~160 mAh/day; autonomy gets validated, not promised) |
| 14-day co-located wind | Speed bias ≤10 %, direction ±≤5 °, no rain dropouts; else mechanical plan B |
| Multi-rate rain | Several simulated volumes and rates (drizzle, steady, heavy); counts within ±10 % each |
| Hot SD removal | Keeps measuring on LittleFS, health flag, zero resets |
| 48 h dead gateway | Zero losses on resume (watermark + SD + time-reconstruct) |
| T/HR co-location | Post-`cal_*` spread ≤±0.2 °C / ±3 %RH |

## 9. Energy and failure management

Sized for autonomous operation with reserve; validated experimentally
in the field, not on paper. The SPS30 fan and the MH-Z19C lamp (both
on 5 V) are the declared unknowns in the energy budget; the battery
divider reports `battery_voltage` so the balance is measured, not
assumed. Covered failures: SD,
gateway, flaky power, clock drift (DS3231 + later server-side
correction), frozen/disconnected sensor (already handled by the
firmware validation engine).
