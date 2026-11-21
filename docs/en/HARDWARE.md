# CAUCE Hardware

## Status

**No physical hardware bought yet.** Every driver below is covered by host tests against
scripted buses (I2C, UART, ADC, 1-Wire) and the firmware cross-compiles for the ESP32, but none
of it has been on a board. Datasheet figures are the manufacturers', not ours.

## Pilot station (4 identical stations + central node)

Every sensor is optional and **off by default**: a probe is enabled with its config key only
where it is fitted, and a node missing one keeps measuring everything else. A probe that is
enabled but does not answer at boot is logged as `SENSOR_ABSENT` and skipped.

| Variable(s) | Part | Bus / pin | Config key | Driver |
|---|---|---|---|---|
| air_temperature, relative_humidity, pressure | BME280 | I2C 0x76/0x77 | always on | `Bme280Driver` |
| illuminance | BH1750 | I2C 0x23 | `light_enabled` | `Bh1750Driver` |
| pm1, pm2_5, pm10 | Plantower PMS5003 (pilot) | UART1 RX GPIO2, 9600 baud | `pm_enabled`, `pm_pms5003` | `Pms5003Driver` |
| pm1, pm2_5, pm4, pm10 | Sensirion SPS30 (alternative) | I2C 0x69 | `pm_enabled` | `Sps30Driver` |
| co2 | Winsen MH-Z19C (NDIR) | UART2 RX 16 / TX 17, 9600 baud | `co2_enabled` | `Mhz19Driver` |
| uv_index | Lite-On LTR390 (pilot) | I2C 0x53 | `uv_enabled`, `uv_ltr390` | `Ltr390Driver` |
| uv_index, uva_index, uvb_index | Vishay VEML6075 (alternative) | I2C 0x10 | `uv_enabled` | `Veml6075Driver` |
| lightning_strikes, lightning_distance | AS3935 | I2C 0x03 + IRQ GPIO25 | `lightning_enabled` | `As3935Driver` |
| wind_speed | cup anemometer (reed/Hall) | GPIO4, falling edge | `wind_enabled` | `HallAnemometerDriver` |
| wind_direction | resistor-ladder vane, 10k pull-up | ADC GPIO34 | `vane_enabled` | `WindVaneDriver` |
| rainfall | tipping-bucket gauge | GPIO27, falling edge | `rain_enabled` | `RainGaugeDriver` |
| soil_temperature | DS18B20, 4.7k pull-up | 1-Wire GPIO13 | `soil_temperature_enabled` | `Ds18b20Driver` |
| soil_moisture | capacitive probe | ADC GPIO35 | `soil_moisture_enabled` | `AnalogSensorDriver` |
| leaf_wetness | resistive/capacitive grid | ADC GPIO32 | `leaf_wetness_enabled` | `AnalogSensorDriver` |
| solar_irradiance (relative) | BPW34 + transimpedance stage | ADC GPIO39 | `irradiance_enabled` | `AnalogSensorDriver` |
| soil_water_potential | gypsum block + 10k reference | ADC GPIO33; ends driven by MCP23017 GPA0/GPA1 at I2C 0x20 when `soil_potential_ac=1` | `soil_potential_enabled`, `soil_potential_ac` | `GypsumAcDriver` (AC) or `AnalogSensorDriver` (DC) |
| battery_voltage | resistor divider | ADC GPIO36 | `battery_monitor_enabled` | `AnalogSensorDriver` |
| (clock) | DS3231 RTC | I2C 0x68 | `rtc_enabled` | `Ds3231Rtc` |
| (archive) | microSD 16 GB high-endurance | VSPI, CS GPIO15 | `sd_archive_enabled` | `ArchiveManager` + `Esp32SdCard` |
| tvoc | CCS811 (optional, not in the pilot BOM) | I2C 0x5A | `voc_enabled` | `Ccs811Driver` |

Notes that change how the numbers may be read:

- **The gypsum block** is read with alternating polarity when `soil_potential_ac=1`. Read as a
  DC divider, current flows through the block the same way on every sample, which polarises it
  and drifts the reading over a season. With the expander, GPA0 drives the 10k reference end and
  GPA1 the far end of the block; each read is one conversion in each direction followed by both
  ends low, so the net charge is zero. The two conversions are folded back into the millivolts
  the DC divider would have read, so `soil_potential_kpa_per_mv` and `soil_potential_offset_kpa`
  keep their meaning and a fit made on a DC node still applies. The MCP23017 (address pins to
  ground) is used because the push-pull outputs can source the divider current; a PCF8574's weak
  high side cannot. A read whose two halves do not add up to the supply is refused rather than
  reported, because that is what an undriven end looks like.

- **CO2** comes only from the NDIR cell. The CCS811's "eCO2" is an estimate derived from VOC
  and is never published. Automatic baseline correction on the MH-Z19C is switched off at
  boot: calibration is the co-location procedure, not a background job.
- **PM**: both sensors need 30 s of fan time before their first frame; readings before that
  are reported as initialising, not stored. The PMS5003 reports its atmospheric values, not the
  CF=1 ones calibrated for factory air. Its TX goes to GPIO2: use a board with no LED on that
  pin, or remove it.
- **UV**: the LTR390 runs at gain 18 and 20 bits, where its datasheet gives 2300 counts per UV
  index; the VEML6075 uses Vishay's open-air coefficients. Behind a window or diffuser either
  must be refitted.
- **Lightning**: a strike is an interrupt, counted in the loop (never in the ISR). An interval
  with no strike reports a count of 0 and *no* distance - zero would mean "overhead".
- **Irradiance** from a photodiode is relative until its gain is fitted against a pyranometer.
- **Soil moisture / leaf wetness** are percentages between a dry and a wet reading, not
  volumetric water content.
- **Rain** is depth per sampling interval; daily totals are sums on the central.
- **Validation**: zeros on rain, light, UV, irradiance, wind speed and lightning, and a vane
  that holds its heading, are not flagged as a stuck sensor - they are the resting state.

## Pin map (ESP32-WROOM-32)

| GPIO | Use |
|---|---|
| 4 | anemometer pulses (internal pull-up) |
| 5, 18, 19, 23 | SX1276 NSS, SCK, MISO, MOSI (VSPI) |
| 13 | 1-Wire (DS18B20) |
| 14 | SX1276 RESET |
| 15 | microSD chip select |
| 16, 17 | UART2 RX/TX (MH-Z19C) |
| 21, 22 | I2C SDA/SCL, 100 kHz |
| 25 | AS3935 IRQ |
| 26 | SX1276 DIO0 |
| 27 | rain gauge pulses (internal pull-up) |
| 32, 33, 34, 35, 36, 39 | ADC1: leaf wetness, soil potential, vane, soil moisture, battery, irradiance |

Analogue inputs use **ADC1 only**. ADC2 is shared with the Wi-Fi radio and returns errors
whenever Wi-Fi is up; `Esp32AnalogInput` refuses any pin outside 32..39 for that reason.
GPIO 0, 2 and 12 are strapping pins and are left free. GPIO15 is one too, but it only selects
boot-log output and is sampled high, which is also the idle level of the SD chip select.

## SX1276 LoRa module - the pins the firmware uses

`main.cpp` builds the radio when `lora_enabled=1`. Pins are in code rather than in the config
file on purpose: a wrong pin set does not fail loudly, it talks to nothing.

| Signal | GPIO | Notes |
|---|---|---|
| SCK | 18 | VSPI |
| MISO | 19 | VSPI |
| MOSI | 23 | VSPI |
| NSS | 5 | chip select, active low |
| RESET | 14 | held low briefly at `begin()` |
| DIO0 | 26 | TxDone / RxDone |
| DIO1 | not wired | the driver polls IRQ flags |
| BUSY | not wired | an SX126x signal; the SX1276 has none |

SPI runs at **1 MHz**. Buy SX1276/RFM95W modules: SX1262-based boards (Heltec WiFi LoRa 32 V3
and similar) need a different driver.

The region comes from `lora_region` as a **name** - `EU868`, `US915`, `AU915`, `AS923` - and
each maps to its frequency in `kLoRaDefaultsFor`. The default is **AU915**, the band used in
Uruguay. A region this build does not recognise is refused and reported as
`LORA_REGION_UNKNOWN`; it does not fall back to another band. Check power, duty cycle and
channel use with the national regulator (URSEC in Uruguay) before deployment.

Frames are signed per `lora_frame_algorithm`: `hmac` (default, with `sync_device_key`) or
`ed25519` (with the seed in `sync_auth_seed`). It must match `device_key_algorithm` at the
central. A node without usable key material for its algorithm logs `LORA_SIGNING_UNUSABLE` and
does not start the link; a node that was never provisioned sends unsigned frames.

LoRa carries data only while Wi-Fi is not delivering (no link, no `sync_server_url`, or a
failing sync), and skips everything Wi-Fi already delivered, so a node with both uses the
radio as its fallback without spending airtime twice.

What is not proven: that the wiring is correct. `LORA_ENABLED` says the SPI peripheral
initialised and the radio accepted its configuration writes, not that the air link works. The
gateway is an ESP32 + SX1276 bridge (`pio run -e gateway`) on USB to the central's host, feeding
`cauce_server.gateway_runner`; see DEPLOYMENT.md, section 2c.

## Peer exchange (ESP-NOW)

With `peer_enabled=1` neighbours on the same `peer_channel` keep copies of each other's records,
so a node that dies does not take its history with it. ESP-NOW authenticates nothing, so every
frame is signed with the sender's Ed25519 seed (`sync_auth_seed`) and merged only when the
sender is trusted and every record in it is the sender's own. A sender is trusted when it is in
`peer_keys`, or when its announcement carries a credential signed by the CA pinned in
`ca_public_key`: 120 bytes binding its node id to its public key until an expiry, returned as
`peer_credential` when the central issues the node's certificate and set in the node's config
under the same name. Reseeding a node then means issuing it a new certificate, not editing every
neighbour; an expired credential stops being trusted once the clock is valid. `tools/provision.py`
still prints the `peer_keys` line for a batch, and a listed key always wins over a vouched one.
Retiring a node at the central sends `revoke_peer` to the rest of its site; each node then refuses
every credential of the retired id that expires by the date it was given, keeps that list in
`/state/peer_revoked` across reboots, and drops it once no such credential can still be valid. A
node re-provisioned later is trusted again on its new credential. With the microSD archive on,
the records a node holds for its neighbours are copied to `/archive/peers-YYYY-MM-DD.csv`, apart
from its own files, so a neighbour that dies leaves a copy of what it shared on a card. One
round per `peer_interval_s` (60 s) offers two of this node's records to each neighbour. Peer
records stay in LittleFS: they are never synced or archived by the node that merely holds them.

## Time

Hierarchy: **NTP > DS3231 > internal oscillator.** With `rtc_enabled=1` the node seeds its
clock from the DS3231 at boot (unless its oscillator-stop flag says the battery was out, which
is logged as `RTC_TIME_UNTRUSTED`), and writes NTP time back to it once SNTP reports a
completed sync. Records taken before any trusted source carry `TIME_UNCERTAIN` and are repaired
centrally by `time-reconstruct`.

## Power

The pilot station runs from a 20 W panel, a 12 V 7 Ah battery and a 10 A charge controller,
with a DC-DC stage for 5 V (PMS5003, MH-Z19C) and 3.3 V. These are starting values, not a proven
autonomy: the 14-day energy balance in `PILOT_SPEC.md` decides. The battery divider
(`battery_divider_ratio`, 100k/22k by default) feeds `battery_voltage`, whose default valid
range covers both a LiPo cell and a 12 V pack.

## Bill of materials (per node)

The budget document of the pilot carries the priced BOM (about USD 255 per complete station);
this table lists the parts and the reason for each choice.

| Subsystem | Part | Why this one |
|---|---|---|
| MCU | ESP32-WROOM-32, 4 MB | the firmware's target (`esp32dev`) |
| Radio | SX1276/RFM95W 915 MHz + antenna | matches the existing driver |
| T/RH/P | BME280 | reference sensor, driver since v0 |
| PM | PMS5003 | PM1, PM2.5, PM10 at a sixth of an SPS30 |
| CO2 | MH-Z19C | NDIR, not an estimate |
| UV | LTR390 | UV index; in production, unlike the VEML6075 |
| Light / irradiance | BH1750 + BPW34 | lux and relative W/m2 are different quantities |
| Wind / rain | Misol cup anemometer, vane, tipping bucket | one kit, validated on the prototype before buying the rest |
| Soil | capacitive probe, hand-made gypsum block, DS18B20 | moisture, suction and temperature |
| Leaf wetness | grid sensor | |
| Lightning | AS3935 module | |
| Clock / archive | DS3231, microSD 32 GB surveillance-grade | trustworthy time offline; CSV history beside LittleFS |
| Power | 20 W panel, 12 V 7 Ah battery, PWM charge controller, DC-DC, fuse, TVS | |
| Mechanics | weatherproof box, commercial ABS radiation shield, mast | same shield and height on every node |

## Storage

LittleFS on the internal flash is the operational store (about 7,700 records in 512 KiB) and the
source of every sync. With `sd_archive_enabled=1` the card is the history: `ArchiveManager` copies
this node's records to `/archive/YYYY-MM-DD.csv` (UTC days; records without a trusted clock go to
`/archive/untimed.csv`) and keeps its progress in `/archive/watermark` on the card. A missing or
failing card never stops measurement: `/api/v1/health` shows `archive_state: degraded`, the archive
retries with backoff, and catches up from LittleFS when the card works again. Records that
LittleFS retention removed before they were archived are counted in `archive_gap_records`. Cards
are FAT32.

## Reference wiring (ESP32 DevKit → BME280)

```
ESP32 3V3  → BME280 VCC     (check the module is really 3.3 V!)
ESP32 GND  → BME280 GND
ESP32 GPIO21 (SDA) → BME280 SDI
ESP32 GPIO22 (SCL) → BME280 SCK
BME280 SDO → GND            (address 0x76; to 3V3 = 0x77)
```

Every other I2C part shares the same two wires; addresses do not collide.

## Physical considerations (mandatory for scientific quality)

- The T/RH sensor **never** sits in direct sun or next to a heat-radiating wall; 1.5–2.5 m
  height works.
- A radiation screen is mandatory if nodes are to be comparable at all. Same screen, same
  height, or don't compare.
- The PM sensor and the MH-Z19C need airflow but not rain: mount them in the screen's lower tier,
  inlet facing down.
- Per node, write down: installation date/time, height, orientation, vane north offset,
  soil probe depth, dominant surrounding surface (context lives in `NodeConfig`).
- No metrological certificate here: these numbers are for comparative community analysis,
  not official meteorology (see CALIBRATION.md).
