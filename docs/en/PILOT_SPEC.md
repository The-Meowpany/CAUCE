# CAUCE — Frozen Pilot Spec

Technical annex. The funding application carries only Meta and Result
(section 1); everything else is build specification.

## 1. Form (evaluator version)

**Goal:** develop and validate a pilot network of eight autonomous
environmental microstations capable of producing hyperlocal, comparable
information on microclimate conditions in Canelones.

**Result:** five calibrated, operating stations generating
environmental records through the pilot period.

## 2. Frozen scope

5 identical nodes · 1 gateway · 1 CAUCE-Node carrier PCB · 1 data
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

## 4. Node (×5 identical)

ESP32-WROOM-32 + SX1276/RFM95W at 915 MHz (AU915, the Uruguayan band) ·
BME280 (temperature, humidity, pressure) · BH1750 (illuminance — lux, not
solar irradiance) · BPW34 photodiode with transimpedance stage (relative
solar irradiance) · SPS30 (PM1, PM2.5, PM4, PM10) · MH-Z19C (NDIR CO₂,
automatic baseline correction off, zeroed at co-location) · VEML6075 (UV
index with separate UVA and UVB) · AS3935 (lightning count and distance) ·
Misol cup anemometer, wind vane and tipping-bucket rain gauge · capacitive
soil moisture probe, hand-made gypsum block (soil water potential, read
with alternating polarity through an MCP23017 expander) and DS18B20 (soil
temperature) · leaf wetness board · DS3231 (±2 ppm; NTP > DS3231 > internal
hierarchy) · microSD 32 GB surveillance-grade card (CSV archive) · 20 W
panel, 12 V 7 Ah battery, PWM charge controller, DC-DC, fuse, TVS ·
weatherproof box with cable glands · 915 MHz antenna · commercial ABS
radiation shield + mast (2 m installation protocol, WMO practice).

Every sensor has a driver in the firmware and a config key that enables
it; the pin map is in `HARDWARE.md`. Drivers are host-tested against
scripted buses and have not yet run on a board.

## 5. Station cost

Prices consulted in Uruguay on 2026-11-21 (Mercado Libre Uruguay and
AliExpress shipping to Uruguay), at the BROU selling rate of UYU 41.95,
plus a 10 % allowance for price changes until purchase: USD 429.73 per
station, itemised in the budget document. At that price eight stations do
not fit the budget; five identical ones do, with a reserve.

## 6. Budget (USD 4,000)

| Item | USD |
|---|---|
| 5 identical stations | 2,148.65 |
| Gateway and central server | 252.22 |
| Tools and bench rig | 146.68 |
| Calibration and co-location | 30.00 |
| Workshops and educational sessions | 479.27 |
| Import provision and installation | 530.01 |
| Critical spares | 122.55 |
| Contingency, dual purpose | 290.62 |
| **Total** | **4,000.00** |

The contingency (7.3 %) has two uses, in order. Until stage 4 closes
(everything bought and imported) it covers what ends up costing more than
planned: prices above the allowance, import above the provision, an
antenna for a site the gateway cannot reach, failed parts beyond the
spares. Whatever is left then funds, before co-location, a traceable
calibration of the bench reference instrument the project owns (or a
certified one if it cannot be certified), which is what turns co-location
from relative consistency into absolute bias; then a second SPS30 spare.

There is no 3D printer: a commercial radiation shield costs less than
printing one. No commercial gypsum blocks are available, so they are made
in the workshops. The import provision is 35 % of the goods bought from
abroad (about USD 1,114). No physical interventions line: the project
generates evidence to evaluate measures, it doesn't execute them.

What remains afterwards: hardware, firmware, protocol, data,
calibration methodology, platform.

## 7. Calibration

Two-week co-location of all 8 nodes before deployment; individual
`cal_*` coefficients per node and variable; a divergence report.
Reference first: the bench's own reference thermohygrometer, with a
calibration certificate from an accredited laboratory funded by the
unspent contingency; every instrument the plan relies on is owned, none
borrowed or shared. Until that certificate exists, eight-way
intercomparison **establishes relative consistency, not absolute bias**,
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
