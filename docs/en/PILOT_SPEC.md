# CAUCE — Frozen Pilot Spec

Technical annex. The funding application carries only Meta and Result
(section 1); everything else is build specification.

## 1. Form (evaluator version)

**Goal:** develop and validate a pilot network of four autonomous
environmental microstations capable of producing hyperlocal, comparable
information on microclimate conditions in Canelones.

**Result:** four calibrated, operating stations generating
environmental records through the pilot period.

## 2. Frozen scope

5 nodes: 4 identical stations + 1 central node (gateway and server) · 1 CAUCE-Node carrier PCB · 1 data
protocol · 1 calibration scheme · 1 local storage system · 1 LoRa
point-to-point link · 1 central API · 1 microclimate viewer.

Golden rule: **the node never depends on the gateway to measure.**

Purchase rule: **no component gets bought in quantity until the
prototype passes acceptance**: the prototype is a full station, so
every sensor type is validated before the lot.

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

## 4. Station (×4 identical)

ESP32-WROOM-32 + RFM95W (SX1276) at 915 MHz (AU915, the Uruguayan band) ·
BME280 (temperature, humidity, pressure) · BH1750 (illuminance — lux, not
solar irradiance) · BPW34 photodiode with transimpedance stage (relative
solar irradiance) · PMS5003 (PM1, PM2.5, PM10) · MH-Z19C (NDIR CO₂,
automatic baseline correction off, zeroed at co-location) · LTR390 (UV
index) · AS3935 (lightning count and distance) · Misol cup anemometer, wind
vane and tipping-bucket rain gauge · capacitive soil moisture probe,
hand-made gypsum block (soil water potential, read with alternating
polarity through an MCP23017 expander) and DS18B20 (soil temperature) ·
leaf wetness board · DS3231 (±2 ppm; NTP > DS3231 > internal hierarchy) ·
microSD 32 GB (CSV archive) · 20 W panel, 12 V 7 Ah battery, PWM charge
controller, DC-DC, fuse, TVS · weatherproof box with cable glands · 915 MHz
antenna · commercial ABS radiation shield + mast (2 m installation
protocol, WMO practice). 21 variables per station.

The SPS30 and VEML6075 cost four to six times more for PM4 and a separate
UVA/UVB reading; the firmware still drives them (`pm_pms5003=0`,
`uv_ltr390=0`) for a deployment that needs those.

Every sensor has a driver in the firmware and a config key that enables
it; the pin map is in `HARDWARE.md`. Drivers are host-tested against
scripted buses and have not yet run on a board.

## 5. Station cost

Lowest prices found on 2026-11-21: AliExpress shipping to Uruguay for the
electronics, local stores for what is heavy or cheaper locally (battery,
panel, pipe, box, antenna). At the BROU selling rate of UYU 41.95 plus a
10 % allowance for price changes: USD 264.77 per station, itemised in the
budget document.

## 6. Budget (USD 4,000 available)

| Item | USD |
|---|---|
| 4 identical stations | 1,059.08 |
| Central node: gateway and server (Orange Pi Zero 3) | 79.70 |
| Tools and bench rig | 132.03 |
| Calibration and co-location | 30.00 |
| Workshops, teaching materials and travel | 2,159.12 |
| Parcel delivery and installation | 155.00 |
| Critical spares | 49.94 |
| Contingency, dual purpose | 290.00 |
| **Project total** | **3,954.87** |
| Not used of the USD 4,000 | 45.13 |
| Import taxes, requested from the Intendencia de Canelones | 387.42 |

The contingency is sized to the price risk. Until stage 4 closes
(everything bought and imported) it covers what ends up costing more than
planned: prices above the allowance, import above the provision, an
antenna for a site the gateway cannot reach, failed parts beyond the
spares. Whatever is left then funds, before co-location, a traceable
calibration of the bench reference instrument the project owns (or a
certified one if it cannot be certified), which is what turns co-location
from relative consistency into absolute bias.

The education chapter holds most of what the network no longer needs:
six practice kits (ESP32 and a sensor set) that stay with the schools, an
ink-tank printer, a cutting plotter and a laminator for guides, stickers
and station labels, a roll-up, snacks for the workshops, and suburban bus
fares for the team (workshops, four maintenance rounds) and for students
travelling to the closing day.

There is no 3D printer: a commercial radiation shield costs less than
printing one. No commercial gypsum blocks are available, so they are made
in the workshops. Import taxes sit outside the budget and are
requested from the Intendencia de Canelones: under the single-payment
regime of Decreto 50/2026 (60 % of each parcel's value, at least USD 20,
replacing VAT and duties) they come to USD 387.42 on about USD 639 of goods
in 11 parcels, bought in one order so each part pays the minimum once. No physical interventions line: the project
generates evidence to evaluate measures, it doesn't execute them.

What remains afterwards: hardware, firmware, protocol, data,
calibration methodology, platform.

## 7. Calibration

Two-week co-location of all 8 nodes before deployment; individual
`cal_*` coefficients per node and variable; a divergence report.
Reference first: the bench's own reference thermohygrometer, with a
calibration certificate from an accredited laboratory funded by the
unspent contingency; every instrument the plan relies on is owned, none
borrowed or shared. Until that certificate exists, intercomparison
of the four stations **establishes relative consistency, not absolute bias**,
and the scientific docs must say so (a bias shared by all nodes would be
invisible).
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
