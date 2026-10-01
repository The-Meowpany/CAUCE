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
protocol · 1 calibration scheme · 1 local storage system · 1 LoRaWAN
system · 1 central API · 1 microclimate viewer.

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
                       ChirpStack
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

Heltec WiFi LoRa 32 V3 (868 MHz) · SHT35 (reference temperature +
humidity) · BME280 (pressure) · SCD40 (CO₂, complementary ambient
layer, forced FRC calibration at co-location) · BH1750 (ambient
illuminance — that is lux, not solar irradiance) · ultrasonic RS485
anemometer, candidate pending validation · tipping-bucket rain gauge ·
protected 18650 · 5 V panel + charge manager · CAUCE-Node carrier PCB
(power, I²C, RS485 with SM712 TVS and termination, SPI/SD, DS3231,
GPIO sensor power-cut, protections) · industrial high-endurance 16 GB
microSD on FAT32 · DS3231 (±2 ppm; NTP > LoRaWAN > DS3231 > internal
hierarchy) · IP65 box with cable glands · 868 MHz antenna + pigtail ·
printed Stevenson screen + mast (2 m installation protocol, WMO
practice).

## 5. Prototype BOM ×1 (~USD 280)

Heltec V3 20 · SHT35 + BME280 + BH1750 18 · SCD40 from the same supplier
as the final batch 45 · ultrasonic RS485 anemometer candidate pending
validation 60 · rain gauge 18 · 18650 + panel + manager 23 · carrier
(prototype batch, prorated) 40 · SD + RTC + RS485 + protections + box +
antenna + mast 56.

## 6. Budget (USD 4,000)

| Item | USD |
|---|---|
| Build of 8 stations: up to | 1,920 |
| Gateway + LoRa infrastructure | 200 |
| Spares | 350 |
| Protection + installation | 300 |
| Carrier PCB fabrication | 150 |
| Transport + logistics | 250 |
| Communication + signage | 200 |
| Training + participation | 200 |
| Contingency | 430 |
| **Total** | **4,000** |

"Up to USD 1,920" is a ceiling with room for price drift, import
costs, and equivalent substitutions (§4–5 BOM attached as annex:
USD 235 + USD 5 manufacturing margin per node). No physical
interventions line: the project generates evidence to evaluate
measures, it doesn't execute them.

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
in the field, not on paper. The anemometer (supply voltage and boost
draw) is the declared unknown in the budget. Covered failures: SD,
gateway, flaky power, clock drift (DS3231 + later server-side
correction), frozen/disconnected sensor (already handled by the
firmware validation engine).
