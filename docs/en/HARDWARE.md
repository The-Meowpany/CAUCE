# CAUCE Hardware

## SX1276 LoRa hat - the pins the firmware uses

`main.cpp` builds the radio when `lora_enabled=1`, with these pins. They are in code rather
than in the config file on purpose: a *frequency* in a text config is one typo away from putting
a node on a band it must not use, and there is no safe default for that.

| Signal | GPIO | Notes |
|---|---|---|
| SCK | 18 | VSPI |
| MISO | 19 | VSPI |
| MOSI | 23 | VSPI |
| NSS | 5 | chip select, active low |
| RESET | 14 | held low briefly at `begin()` |
| DIO0 | 26 | TxDone / RxDone |
| DIO1 | 33 | optional, used for Fhss / timeout |
| BUSY | 32 | input, polled while the radio is busy |

SPI runs at **1 MHz**, which is the SX1276's own maximum for the modes this driver uses.

The region comes from `lora_region` as a **name** - `EU868`, `US915`, `AU915`, `AS923` - and
each maps to its frequency in `kLoRaDefaultsFor`. A region this build does not recognise is
refused and reported as `LORA_REGION_UNKNOWN`; it does not fall back to 868 MHz, because
putting an AU node on the European band is precisely the failure the refusal exists to prevent.

### What is not proven about this

That the wiring is correct. `LORA_ENABLED` says the SPI peripheral initialised and the radio
accepted its configuration register writes - not that a register read returned a plausible
value. The first real evidence is that read, and it needs a board.

## Status

**No physical hardware bought yet.** Everything firmware-side is proven
through simulation and tests. The BME280 is the reference pick because
it's cheap, everywhere, speaks clean I2C, and brings pressure along
with temperature and humidity (±0.5 °C, ±3 %RH — datasheet numbers, not
ours).

## Bill of Materials (per node) — draft v0

| Component | Qty | Purpose | Minimum | Recommended | Alternatives | Est. cost USD |
|---|---|---|---|---|---|---|
| MCU board | 1 | Compute + Wi-Fi + flash | ESP32-WROOM-32, 4 MB flash | ESP32-DevKitC WROOM-32E | ESP32-S3 (future), ESP8266 (limited) | 4–7 |
| Environmental sensor | 1 | T°, RH, pressure | BME280 I2C module 3.3 V | GY-BME280 with regulator | SHT31 (+BMP388), DHT22 (lower quality) | 3–6 |
| Storage | — | Local log | Internal 4 MB flash (LittleFS) | same | microSD (later phase) | 0 |
| Power supply | 1 | Stable power | USB 5 V / adapter ≥1A | charger + power bank | solar panel + LiPo + TP4056 (autonomy) | 3–10 |
| Enclosure | 1 | IP protection + radiation | Ventilated IP54, no direct sun | mini Stevenson-type radiation screen (3D printed) | louvered PVC | 2–8 |
| Wiring | — | I2C | short dupont/JST (<30 cm) | twisted cable, pull-ups already on module | — | 1 |
| Total | | | | | | **13–32** |

Don't marry one vendor: generic BME280 modules are interchangeable,
and the driver only assumes I2C address 0x76/0x77.

## Reference wiring (ESP32 DevKit → BME280)

```
ESP32 3V3  → BME280 VCC     (check the module is really 3.3 V!)
ESP32 GND  → BME280 GND
ESP32 GPIO21 (SDA) → BME280 SDI
ESP32 GPIO22 (SCL) → BME280 SCK
BME280 SDO → GND            (address 0x76; to 3V3 = 0x77)
```

Wired in `main.cpp`: I2C at 100 kHz on pins 21/22.

## Physical considerations (mandatory for scientific quality)

- The sensor **never** sits in direct sun or next to a heat-radiating
  wall; 1.5–2.5 m height works.
- A radiation screen is mandatory if you want nodes to be comparable
  at all. Same screen, same height, or don't compare.
- Per node, write down: installation date/time, height, orientation,
  dominant surrounding surface (context lives in `NodeConfig`).
- No metrological certificate here: these numbers are for comparative
  community analysis, not official meteorology (see CALIBRATION.md).
