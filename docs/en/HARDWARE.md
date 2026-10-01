# CAUCE Hardware

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
