# CAUCE Hardware

## Status

**No physical hardware acquired yet.** All firmware validated through
simulation and tests. BME280 chosen as reference implementation for:
reasonable accuracy (±0.5 °C, ±3 %RH), barometric pressure, clean I2C bus,
availability and price.

## Bill of Materials (per node) — draft v0

| Component | Qty | Purpose | Minimum | Recommended | Alternatives | Est. cost USD |
|---|---|---|---|---|---|---|
| MCU board | 1 | Compute + Wi-Fi + flash | ESP32-WROOM-32, 4 MB flash | ESP32-DevKitC WROOM-32E | ESP32-S3 (future), ESP8266 (limited) | 4–7 |
| Environmental sensor | 1 | T°, RH, pressure | BME280 I2C module 3.3 V | GY-BME280 with regulator | SHT31 (+BMP388), DHT22 (lower quality) | 3–6 |
| Storage | — | Local log | Internal 4 MB flash (LittleFS) | same | microSD (later phase) | 0 |
| Power supply | 1 | Stable power | USB 5 V / adapter ≥1A | charger + power bank | solar panel + LiPo + TP4056 (autonomy) | 3–10 |
| Enclosure | 1 | IP protection + radiation | Ventilated IP54, no direct sun exposure | mini Stevenson-type radiation screen (3D printed) | louvered PVC | 2–8 |
| Wiring | — | I2C | short dupont/JST (<30 cm) | twisted cable, pull-ups already on module | — | 1 |
| Total | | | | | | **13–32** |

Avoid single-vendor dependence: generic BME280 modules are interchangeable;
the driver only assumes I2C address 0x76/0x77.

## Reference wiring (ESP32 DevKit → BME280)

```
ESP32 3V3  → BME280 VCC     (verify the module is 3.3 V!)
ESP32 GND  → BME280 GND
ESP32 GPIO21 (SDA) → BME280 SDI
ESP32 GPIO22 (SCL) → BME280 SCK
BME280 SDO → GND            (address 0x76; to 3V3 = 0x77)
```

Configured in `main.cpp`: I2C at 100 kHz on pins 21/22.

## Physical considerations (mandatory for scientific quality)

- The sensor **never** in direct sun nor near heat-radiating walls; suggested
  height 1.5–2.5 m.
- Radiation screen mandatory for cross-node comparability.
- Record per node: installation date/time, height, orientation, dominant
  surrounding surface (context lives in `NodeConfig`).
- No metrological certificate: values are for comparative community
  analysis, not official meteorology (see docs/CALIBRATION.md).
