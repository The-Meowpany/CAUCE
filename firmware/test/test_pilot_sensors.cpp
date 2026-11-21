#include <cmath>
#include <cstring>
#include <vector>

#include <unity.h>

#include "cauce/core/NodeConfig.h"
#include "cauce/core/Types.h"
#include "cauce/core/ValidationEngine.h"
#include "cauce/drivers/As3935Driver.h"
#include "cauce/drivers/Ds18b20Driver.h"
#include "cauce/drivers/Ds3231Rtc.h"
#include "cauce/drivers/GypsumAcDriver.h"
#include "cauce/drivers/Ltr390Driver.h"
#include "cauce/drivers/Pms5003Driver.h"
#include "cauce/drivers/Mhz19Driver.h"
#include "cauce/drivers/Sps30Driver.h"
#include "cauce/drivers/Veml6075Driver.h"
#include "cauce/drivers/WeatherKitDrivers.h"
#include "cauce/hal/ManualClock.h"
#include "cauce/hal/ScriptedI2c.h"
#include "cauce/hal/ScriptedPeripherals.h"

using namespace cauce;
using namespace cauce::drivers;

namespace {

constexpr uint64_t kEpoch = 1787356800000ULL;

// An SPS30 on the bus: answers the 16-bit command pointer with the frame the test set up.
class FakeSps30 final : public hal::ScriptedI2cDevice {
 public:
  void writeRegister(uint8_t, const uint8_t*, size_t) override {}
  bool readRegister(uint8_t, uint8_t*, size_t) override { return false; }

  bool writeRaw(const uint8_t* data, size_t length) override {
    if (length < 2) return false;
    lastCommand = static_cast<uint16_t>((data[0] << 8) | data[1]);
    commands.push_back(lastCommand);
    return true;
  }
  bool readRaw(uint8_t* buffer, size_t length) override {
    std::vector<uint8_t> words;
    if (lastCommand == 0x0202) {
      words = {0x00, static_cast<uint8_t>(ready ? 0x01 : 0x00)};
    } else if (lastCommand == 0x0300) {
      for (int i = 0; i < 10; ++i) {
        const float value = i < 4 ? mass[i] : 0.0f;
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        for (int b = 3; b >= 0; --b) words.push_back(static_cast<uint8_t>(bits >> (8 * b)));
      }
    } else {
      return false;
    }
    std::vector<uint8_t> framed;
    for (size_t w = 0; w + 1 < words.size(); w += 2) {
      framed.push_back(words[w]);
      framed.push_back(words[w + 1]);
      uint8_t crc = Sps30Driver::crc8(&words[w], 2);
      if (corruptCrc && w == 0) crc ^= 0xFF;
      framed.push_back(crc);
    }
    if (framed.size() != length) return false;
    std::memcpy(buffer, framed.data(), length);
    return true;
  }

  bool ready{true};
  bool corruptCrc{false};
  float mass[4]{1.5f, 12.25f, 20.0f, 31.75f};
  uint16_t lastCommand{0};
  std::vector<uint16_t> commands;
};

std::vector<uint8_t> mhzReply(uint16_t ppm) {
  std::vector<uint8_t> r = {0xFF, 0x86, static_cast<uint8_t>(ppm >> 8),
                            static_cast<uint8_t>(ppm & 0xFF), 0x47, 0x00, 0x00, 0x00, 0x00};
  r[8] = Mhz19Driver::checksum(r.data());
  return r;
}

void loadScratchpad(hal::ScriptedOneWire& bus, int16_t raw, bool corrupt = false) {
  uint8_t pad[9] = {static_cast<uint8_t>(raw & 0xFF), static_cast<uint8_t>((raw >> 8) & 0xFF),
                    0x4B, 0x46, 0x7F, 0xFF, 0x0C, 0x10, 0};
  pad[8] = Ds18b20Driver::crc8(pad, 8);
  if (corrupt) pad[8] ^= 0x01;
  for (uint8_t b : pad) bus.toRead.push_back(b);
}

}  // namespace

// ---------------------------------------------------------------------------
// Catalogue
// ---------------------------------------------------------------------------

void test_pilot_every_variable_has_a_name_a_unit_and_round_trips() {
  for (uint8_t i = 0; i < kVariableCount; ++i) {
    const Variable v = static_cast<Variable>(i);
    TEST_ASSERT_NOT_EQUAL(0, std::strcmp("unknown", variableName(v)));
    TEST_ASSERT_NOT_EQUAL(0, std::strcmp("?", variableUnit(v)));
    TEST_ASSERT_EQUAL(static_cast<int>(v), static_cast<int>(parseVariable(variableName(v))));
  }
}

void test_pilot_existing_variable_numbers_did_not_move() {
  // The number is what the stored frame and the LoRa record carry.
  TEST_ASSERT_EQUAL(5, static_cast<int>(Variable::WindSpeed));
  TEST_ASSERT_EQUAL(6, static_cast<int>(Variable::Tvoc));
  TEST_ASSERT_EQUAL(7, static_cast<int>(Variable::Co2));
  TEST_ASSERT_EQUAL(23, static_cast<int>(Variable::LightningDistance));
}

void test_pilot_every_new_variable_has_a_finite_default_range() {
  const Thresholds t = defaultThresholds();
  for (uint8_t i = static_cast<uint8_t>(Variable::Co2); i < kVariableCount; ++i) {
    TEST_ASSERT_TRUE(t.rangeMin[i] > -1e30f);
    TEST_ASSERT_TRUE(t.rangeMax[i] < 1e30f);
    TEST_ASSERT_TRUE(t.rangeMin[i] < t.rangeMax[i]);
  }
}

// ---------------------------------------------------------------------------
// Validation: resting values are not stuck sensors
// ---------------------------------------------------------------------------

void test_pilot_dry_days_and_dark_nights_are_not_stuck_sensors() {
  ValidationEngine engine(defaultThresholds());
  const Variable resting[] = {Variable::Rainfall, Variable::Light, Variable::WindSpeed,
                              Variable::LightningStrikes, Variable::SolarIrradiance};
  for (Variable v : resting) {
    ValidationContext ctx;
    ctx.timeValid = true;
    ctx.nowUtcMs = kEpoch;
    ctx.identicalStreak = 10;
    ctx.streakValue = 0.0f;
    Measurement m{};
    m.variable = v;
    m.value = 0.0f;
    m.timestampUtcMs = kEpoch;
    const ValidationResult r = engine.evaluate(m, ctx);
    TEST_ASSERT_EQUAL(0, r.reasonBits & kReasonStuckValue);
  }
}

void test_pilot_a_frozen_nonzero_reading_is_still_flagged() {
  ValidationEngine engine(defaultThresholds());
  ValidationContext ctx;
  ctx.timeValid = true;
  ctx.nowUtcMs = kEpoch;
  ctx.identicalStreak = 10;
  ctx.streakValue = 812.0f;
  Measurement m{};
  m.variable = Variable::Co2;
  m.value = 812.0f;
  m.timestampUtcMs = kEpoch;
  const ValidationResult r = engine.evaluate(m, ctx);
  TEST_ASSERT_NOT_EQUAL(0, r.reasonBits & kReasonStuckValue);
  TEST_ASSERT_EQUAL(static_cast<int>(Quality::Suspect), static_cast<int>(r.quality));
}

void test_pilot_a_twelve_volt_battery_is_in_range() {
  ValidationEngine engine(defaultThresholds());
  ValidationContext ctx;
  Measurement m{};
  m.variable = Variable::BatteryVoltage;
  m.value = 12.6f;
  const ValidationResult r = engine.evaluate(m, ctx);
  TEST_ASSERT_EQUAL(0, r.reasonBits & kReasonOutOfRange);
}

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

void test_pilot_sensor_switches_and_calibrations_survive_a_roundtrip() {
  NodeConfig original{};
  copyString(original.nodeId, sizeof(original.nodeId), "CAUCE-003");
  original.lightEnabled = true;
  original.windEnabled = true;
  original.pmEnabled = true;
  original.co2Enabled = true;
  original.rainEnabled = true;
  original.rainMmPerTip = 0.5f;
  original.windPulsesPerRevolution = 2;
  original.soilMoistureDryMv = 2750.0f;
  original.batteryDividerRatio = 4.2f;
  char text[8192];
  TEST_ASSERT_TRUE(serializeConfig(original, text, sizeof(text)));
  NodeConfig parsed{};
  TEST_ASSERT_TRUE(parseConfig(text, parsed));
  TEST_ASSERT_TRUE(parsed.lightEnabled);
  TEST_ASSERT_TRUE(parsed.windEnabled);
  TEST_ASSERT_TRUE(parsed.pmEnabled);
  TEST_ASSERT_TRUE(parsed.co2Enabled);
  TEST_ASSERT_TRUE(parsed.rainEnabled);
  TEST_ASSERT_FALSE(parsed.uvEnabled);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.5f, parsed.rainMmPerTip);
  TEST_ASSERT_EQUAL(2, parsed.windPulsesPerRevolution);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 2750.0f, parsed.soilMoistureDryMv);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 4.2f, parsed.batteryDividerRatio);
}

template <size_t N>
void fillField(char (&field)[N], char c) {
  std::memset(field, c, N - 1);
  field[N - 1] = '\0';
}

void test_pilot_a_full_config_fits_the_node_buffer() {
  // Every string field at its maximum length, not just the certificate: a key added later is
  // exactly what pushes the serialised config past the buffer the node writes it into.
  NodeConfig full{};
  fillField(full.nodeId, 'n');
  fillField(full.siteId, 's');
  fillField(full.landCover, 'l');
  fillField(full.shadeCondition, 'h');
  fillField(full.wifiSsid, 'w');
  fillField(full.wifiPassword, 'p');
  fillField(full.ntpServer, 't');
  fillField(full.syncServerUrl, 'u');
  fillField(full.syncDeviceKey, 'a');
  fillField(full.syncAuthSeedHex, 'a');
  fillField(full.syncCertificate, 'c');
  fillField(full.caPublicKeyHex, 'a');
  fillField(full.otaManifestKey, 'a');
  fillField(full.otaPublicKey, 'a');
  fillField(full.otaPublicKeyNext, 'a');
  fillField(full.otaManifestUrl, 'o');
  fillField(full.peerStatePath, 'q');
  fillField(full.peerKeys, 'k');
  fillField(full.peerCredentialHex, 'a');
  fillField(full.loraRegion, 'r');
  fillField(full.loraFrameAlgorithm, 'f');
  for (size_t i = 0; i < sizeof(full.thresholds.rangeMax) / sizeof(full.thresholds.rangeMax[0]);
       ++i) {
    full.thresholds.rangeMin[i] = -12345.67f;
    full.thresholds.rangeMax[i] = 12345.67f;
  }
  char text[8192];
  TEST_ASSERT_TRUE(serializeConfig(full, text, sizeof(text)));
}

void test_pilot_the_default_region_is_the_uruguayan_band() {
  NodeConfig cfg{};
  TEST_ASSERT_EQUAL_STRING("AU915", cfg.loraRegion);
}

// ---------------------------------------------------------------------------
// SPS30
// ---------------------------------------------------------------------------

void test_pilot_sps30_reports_all_four_mass_fractions_from_one_frame() {
  hal::ManualClock clock(kEpoch);
  hal::ScriptedI2cBus bus;
  FakeSps30 device;
  bus.attach(Sps30Driver::kAddress, &device);
  Sps30Driver sps(bus, clock, "SPS30-1");
  TEST_ASSERT_TRUE(sps.begin());
  TEST_ASSERT_EQUAL_HEX16(0x0010, device.commands.front());
  clock.advanceMs(Sps30Driver::kWarmupMs);

  Reading r;
  TEST_ASSERT_TRUE(sps.read(Variable::Pm1, r));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.5f, r.value);
  const size_t commandsAfterFirst = device.commands.size();
  TEST_ASSERT_TRUE(sps.read(Variable::Pm25, r));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 12.25f, r.value);
  TEST_ASSERT_TRUE(sps.read(Variable::Pm4, r));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 20.0f, r.value);
  TEST_ASSERT_TRUE(sps.read(Variable::Pm10, r));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 31.75f, r.value);
  TEST_ASSERT_EQUAL(commandsAfterFirst, device.commands.size());
}

void test_pilot_sps30_refuses_to_report_during_fan_warmup() {
  hal::ManualClock clock(kEpoch);
  hal::ScriptedI2cBus bus;
  FakeSps30 device;
  bus.attach(Sps30Driver::kAddress, &device);
  Sps30Driver sps(bus, clock, "SPS30-1");
  sps.begin();
  Reading r;
  TEST_ASSERT_FALSE(sps.read(Variable::Pm25, r));
  TEST_ASSERT_EQUAL(static_cast<int>(SensorStatus::Initializing), static_cast<int>(r.status));
}

void test_pilot_sps30_drops_a_frame_with_a_bad_crc() {
  hal::ManualClock clock(kEpoch);
  hal::ScriptedI2cBus bus;
  FakeSps30 device;
  bus.attach(Sps30Driver::kAddress, &device);
  Sps30Driver sps(bus, clock, "SPS30-1");
  sps.begin();
  clock.advanceMs(Sps30Driver::kWarmupMs);
  device.corruptCrc = true;
  Reading r;
  TEST_ASSERT_FALSE(sps.read(Variable::Pm25, r));
  TEST_ASSERT_FALSE(r.ok);
}

void test_pilot_sps30_crc_matches_the_datasheet_example() {
  // Datasheet example: CRC(0xBEEF) = 0x92.
  const uint8_t word[2] = {0xBE, 0xEF};
  TEST_ASSERT_EQUAL_HEX8(0x92, Sps30Driver::crc8(word, 2));
}

// ---------------------------------------------------------------------------
// MH-Z19C
// ---------------------------------------------------------------------------

void test_pilot_mhz19_reads_co2_after_preheat_with_abc_disabled() {
  hal::ManualClock clock(kEpoch);
  hal::ScriptedUart uart;
  uart.replies.push_back({});  // ABC command: no reply
  uart.replies.push_back(mhzReply(415));
  Mhz19Driver co2(uart, clock, "MHZ19-1");
  TEST_ASSERT_TRUE(co2.begin());
  TEST_ASSERT_EQUAL(9600u, uart.baud());
  TEST_ASSERT_EQUAL_HEX8(0x79, uart.written[2]);
  TEST_ASSERT_EQUAL_HEX8(0x00, uart.written[3]);

  Reading r;
  TEST_ASSERT_FALSE(co2.read(Variable::Co2, r));
  TEST_ASSERT_EQUAL(static_cast<int>(SensorStatus::Initializing), static_cast<int>(r.status));

  clock.advanceMs(Mhz19Driver::kPreheatMs);
  uart.replies.push_back(mhzReply(812));
  TEST_ASSERT_TRUE(co2.read(Variable::Co2, r));
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 812.0f, r.value);
}

void test_pilot_mhz19_rejects_a_reply_with_a_bad_checksum() {
  hal::ManualClock clock(kEpoch);
  hal::ScriptedUart uart;
  uart.replies.push_back({});
  uart.replies.push_back(mhzReply(415));
  Mhz19Driver co2(uart, clock, "MHZ19-1");
  co2.begin();
  clock.advanceMs(Mhz19Driver::kPreheatMs);
  std::vector<uint8_t> bad = mhzReply(900);
  bad[8] ^= 0x01;
  uart.replies.push_back(bad);
  Reading r;
  TEST_ASSERT_FALSE(co2.read(Variable::Co2, r));
}

void test_pilot_mhz19_discards_a_stale_reply_before_asking() {
  hal::ManualClock clock(kEpoch);
  hal::ScriptedUart uart;
  uart.replies.push_back({});
  uart.replies.push_back(mhzReply(415));
  Mhz19Driver co2(uart, clock, "MHZ19-1");
  co2.begin();
  clock.advanceMs(Mhz19Driver::kPreheatMs);
  uart.preload(mhzReply(5000));  // a late answer to an earlier request
  uart.replies.push_back(mhzReply(640));
  Reading r;
  TEST_ASSERT_TRUE(co2.read(Variable::Co2, r));
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 640.0f, r.value);
}

void test_pilot_mhz19_without_a_sensor_does_not_start() {
  hal::ManualClock clock(kEpoch);
  hal::ScriptedUart uart;
  uart.setResponding(false);
  Mhz19Driver co2(uart, clock, "MHZ19-1");
  TEST_ASSERT_FALSE(co2.begin());
  TEST_ASSERT_EQUAL(static_cast<int>(SensorStatus::NotConnected),
                    static_cast<int>(co2.health()));
}

// ---------------------------------------------------------------------------
// VEML6075
// ---------------------------------------------------------------------------

void test_pilot_veml6075_conversion_follows_the_application_note() {
  Veml6075Driver::Raw raw;
  raw.uva = 1000;
  raw.uvb = 1200;
  raw.comp1 = 100;
  raw.comp2 = 50;
  const Veml6075Driver::Index i = Veml6075Driver::convert(raw);
  const float uvaCalc = 1000.0f - 2.22f * 100 - 1.33f * 50;
  const float uvbCalc = 1200.0f - 2.95f * 100 - 1.74f * 50;
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, uvaCalc * 0.001461f, i.uva);
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, uvbCalc * 0.002591f, i.uvb);
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, (i.uva + i.uvb) / 2.0f, i.uvi);
}

void test_pilot_veml6075_never_reports_negative_uv_in_the_dark() {
  Veml6075Driver::Raw raw;
  raw.uva = 3;
  raw.uvb = 2;
  raw.comp1 = 10;
  raw.comp2 = 10;
  const Veml6075Driver::Index i = Veml6075Driver::convert(raw);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, i.uva);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, i.uvb);
}

void test_pilot_veml6075_refuses_a_part_with_the_wrong_id() {
  hal::ScriptedI2cBus bus;
  hal::RegisterFileDevice device;
  bus.attach(Veml6075Driver::kAddress, &device);
  device.setRegister(0x0C, 0x55);
  Veml6075Driver uv(bus, "VEML-1");
  TEST_ASSERT_FALSE(uv.begin());
  device.setRegister(0x0C, 0x26);
  TEST_ASSERT_TRUE(uv.begin());
  TEST_ASSERT_EQUAL_HEX8(0x10, device.getRegister(0x00));
}

// ---------------------------------------------------------------------------
// AS3935
// ---------------------------------------------------------------------------

void test_pilot_as3935_counts_strikes_and_reports_the_nearest() {
  hal::ScriptedI2cBus bus;
  hal::RegisterFileDevice device;
  bus.attach(As3935Driver::kDefaultAddress, &device);
  As3935Driver lightning(bus, "AS3935-1", false);
  TEST_ASSERT_TRUE(lightning.begin());
  // Outdoor gain in bits 5:1 of register 0, disturbers masked.
  TEST_ASSERT_EQUAL_HEX8(0x0E << 1, device.getRegister(0x00));
  TEST_ASSERT_TRUE((device.getRegister(0x03) & 0x20) != 0);

  device.setRegister(0x03, 0x28);  // lightning, mask bit kept
  device.setRegister(0x07, 14);
  lightning.onInterrupt();
  lightning.service();
  device.setRegister(0x07, 6);
  lightning.onInterrupt();
  lightning.service();
  device.setRegister(0x03, 0x24);  // a disturber is not a strike
  lightning.onInterrupt();
  lightning.service();

  Reading r;
  TEST_ASSERT_TRUE(lightning.read(Variable::LightningStrikes, r));
  TEST_ASSERT_EQUAL_FLOAT(2.0f, r.value);
  TEST_ASSERT_TRUE(lightning.read(Variable::LightningDistance, r));
  TEST_ASSERT_EQUAL_FLOAT(6.0f, r.value);
  TEST_ASSERT_EQUAL(1u, lightning.disturberCount());

  // The interval closed: next cycle starts from zero and has no distance.
  TEST_ASSERT_TRUE(lightning.read(Variable::LightningStrikes, r));
  TEST_ASSERT_EQUAL_FLOAT(0.0f, r.value);
  TEST_ASSERT_FALSE(lightning.read(Variable::LightningDistance, r));
  TEST_ASSERT_EQUAL(static_cast<int>(SensorStatus::NoEvent), static_cast<int>(r.status));
}

void test_pilot_as3935_ignores_service_without_an_interrupt() {
  hal::ScriptedI2cBus bus;
  hal::RegisterFileDevice device;
  bus.attach(As3935Driver::kDefaultAddress, &device);
  As3935Driver lightning(bus, "AS3935-1", false);
  lightning.begin();
  device.setRegister(0x03, 0x08);
  device.setRegister(0x07, 5);
  lightning.service();  // no IRQ seen: the register is not a fresh event
  Reading r;
  lightning.read(Variable::LightningStrikes, r);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, r.value);
}

// ---------------------------------------------------------------------------
// DS18B20
// ---------------------------------------------------------------------------

void test_pilot_ds18b20_reads_the_previous_conversion_without_blocking() {
  hal::ScriptedOneWire bus;
  Ds18b20Driver soil(bus, "DS18B20-1");
  TEST_ASSERT_TRUE(soil.begin());
  TEST_ASSERT_EQUAL_HEX8(0x44, bus.written.back());
  loadScratchpad(bus, static_cast<int16_t>(0x0191));  // 25.0625 C
  Reading r;
  TEST_ASSERT_TRUE(soil.read(Variable::SoilTemperature, r));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 25.0625f, r.value);
  // The next conversion was started as part of the read.
  TEST_ASSERT_EQUAL_HEX8(0x44, bus.written.back());
}

void test_pilot_ds18b20_handles_negative_temperatures() {
  hal::ScriptedOneWire bus;
  Ds18b20Driver soil(bus, "DS18B20-1");
  soil.begin();
  loadScratchpad(bus, static_cast<int16_t>(0xFF5E));  // -10.125 C
  Reading r;
  TEST_ASSERT_TRUE(soil.read(Variable::SoilTemperature, r));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, -10.125f, r.value);
}

void test_pilot_ds18b20_rejects_crc_errors_and_the_power_on_value() {
  hal::ScriptedOneWire bus;
  Ds18b20Driver soil(bus, "DS18B20-1");
  soil.begin();
  loadScratchpad(bus, static_cast<int16_t>(0x0191), true);
  Reading r;
  TEST_ASSERT_FALSE(soil.read(Variable::SoilTemperature, r));
  loadScratchpad(bus, static_cast<int16_t>(0x0550));
  TEST_ASSERT_FALSE(soil.read(Variable::SoilTemperature, r));
  TEST_ASSERT_EQUAL(static_cast<int>(SensorStatus::Initializing), static_cast<int>(r.status));
}

void test_pilot_ds18b20_without_a_probe_reports_not_connected() {
  hal::ScriptedOneWire bus;
  bus.present = false;
  Ds18b20Driver soil(bus, "DS18B20-1");
  TEST_ASSERT_FALSE(soil.begin());
  TEST_ASSERT_EQUAL(static_cast<int>(SensorStatus::NotConnected),
                    static_cast<int>(soil.health()));
}

// ---------------------------------------------------------------------------
// Rain, vane, analogue probes
// ---------------------------------------------------------------------------

void test_pilot_rain_reports_depth_per_interval_and_debounces() {
  RainGaugeDriver rain("RAIN-1", 0.2794f);
  rain.onTip(1000);
  rain.onTip(1010);  // contact bounce
  rain.onTip(5000);
  rain.onTip(9000);
  Reading r;
  TEST_ASSERT_TRUE(rain.read(Variable::Rainfall, r));
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 3 * 0.2794f, r.value);
  TEST_ASSERT_TRUE(rain.read(Variable::Rainfall, r));
  TEST_ASSERT_EQUAL_FLOAT(0.0f, r.value);
  TEST_ASSERT_EQUAL(3u, rain.totalTips());
}

void test_pilot_vane_maps_each_heading_and_applies_the_mounting_offset() {
  hal::ScriptedAnalogInput adc;
  WindVaneDriver vane(adc, 34, "VANE-1", 3300.0f, 10000.0f, 0.0f);
  // East: 1000 ohm against a 10k pull-up.
  adc.values[34] = 3300.0f * 1000.0f / 11000.0f;
  Reading r;
  TEST_ASSERT_TRUE(vane.read(Variable::WindDirection, r));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 90.0f, r.value);

  WindVaneDriver rotated(adc, 34, "VANE-2", 3300.0f, 10000.0f, 300.0f);
  TEST_ASSERT_TRUE(rotated.read(Variable::WindDirection, r));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 30.0f, r.value);
}

void test_pilot_vane_reports_an_open_cable_instead_of_a_direction() {
  hal::ScriptedAnalogInput adc;
  WindVaneDriver vane(adc, 34, "VANE-1");
  adc.values[34] = 3295.0f;
  Reading r;
  TEST_ASSERT_FALSE(vane.read(Variable::WindDirection, r));
  TEST_ASSERT_EQUAL(static_cast<int>(SensorStatus::NotConnected), static_cast<int>(r.status));
}

void test_pilot_soil_moisture_two_point_calibration_is_inverted_and_clamped() {
  hal::ScriptedAnalogInput adc;
  AnalogSensorDriver soil(adc, 35, "SOIL-1", "CAP-SOIL", Variable::SoilMoisture,
                          AnalogSensorDriver::Mapping::TwoPointPercent, 2600.0f, 1100.0f);
  Reading r;
  adc.values[35] = 1850.0f;
  TEST_ASSERT_TRUE(soil.read(Variable::SoilMoisture, r));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 50.0f, r.value);
  adc.values[35] = 900.0f;
  TEST_ASSERT_TRUE(soil.read(Variable::SoilMoisture, r));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 100.0f, r.value);
  adc.values[35] = 3000.0f;
  TEST_ASSERT_TRUE(soil.read(Variable::SoilMoisture, r));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, r.value);
}

void test_pilot_battery_divider_is_a_linear_mapping() {
  hal::ScriptedAnalogInput adc;
  AnalogSensorDriver battery(adc, 36, "VBAT", "DIVIDER", Variable::BatteryVoltage,
                             AnalogSensorDriver::Mapping::Linear, 5.545f / 1000.0f, 0.0f);
  adc.values[36] = 2272.0f;
  Reading r;
  TEST_ASSERT_TRUE(battery.read(Variable::BatteryVoltage, r));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 12.60f, r.value);
}

void test_pilot_a_calibration_with_no_span_is_refused() {
  hal::ScriptedAnalogInput adc;
  AnalogSensorDriver leaf(adc, 39, "LEAF-1", "LEAF", Variable::LeafWetness,
                          AnalogSensorDriver::Mapping::TwoPointPercent, 1500.0f, 1500.0f);
  adc.values[39] = 1400.0f;
  Reading r;
  TEST_ASSERT_FALSE(leaf.read(Variable::LeafWetness, r));
  TEST_ASSERT_EQUAL(static_cast<int>(SensorStatus::InvalidData), static_cast<int>(r.status));
}

// ---------------------------------------------------------------------------
// DS3231
// ---------------------------------------------------------------------------

void test_pilot_rtc_calendar_conversion_round_trips() {
  const uint64_t t = Ds3231Rtc::toEpochMs(2026, 11, 20, 23, 59, 58);
  int y, mo, d, h, mi, s;
  Ds3231Rtc::fromEpochMs(t, y, mo, d, h, mi, s);
  TEST_ASSERT_EQUAL(2026, y);
  TEST_ASSERT_EQUAL(11, mo);
  TEST_ASSERT_EQUAL(20, d);
  TEST_ASSERT_EQUAL(23, h);
  TEST_ASSERT_EQUAL(59, mi);
  TEST_ASSERT_EQUAL(58, s);
  // 2024-02-29 is a leap day.
  TEST_ASSERT_EQUAL_UINT64(1709164800000ULL, Ds3231Rtc::toEpochMs(2024, 2, 29, 0, 0, 0));
}

void test_pilot_rtc_write_then_read_and_the_stop_flag() {
  hal::ScriptedI2cBus bus;
  hal::RegisterFileDevice device;
  bus.attach(Ds3231Rtc::kAddress, &device);
  Ds3231Rtc rtc(bus);
  device.setRegister(0x0F, 0x80);  // oscillator stopped: battery was out
  TEST_ASSERT_FALSE(rtc.timeTrustworthy());
  const uint64_t when = Ds3231Rtc::toEpochMs(2026, 3, 7, 14, 5, 9);
  TEST_ASSERT_TRUE(rtc.writeUtcMs(when));
  TEST_ASSERT_TRUE(rtc.timeTrustworthy());
  TEST_ASSERT_EQUAL_HEX8(0x09, device.getRegister(0x00));  // BCD seconds
  TEST_ASSERT_EQUAL_HEX8(0x14, device.getRegister(0x02));  // BCD hours
  uint64_t back = 0;
  TEST_ASSERT_TRUE(rtc.readUtcMs(back));
  TEST_ASSERT_EQUAL_UINT64(when, back);
}

void test_pilot_rtc_refuses_an_impossible_date() {
  hal::ScriptedI2cBus bus;
  hal::RegisterFileDevice device;
  bus.attach(Ds3231Rtc::kAddress, &device);
  Ds3231Rtc rtc(bus);
  device.setRegister(0x05, 0x13);  // month 13
  device.setRegister(0x04, 0x01);
  uint64_t out = 0;
  TEST_ASSERT_FALSE(rtc.readUtcMs(out));
}

// An MCP23017 driving the two ends of a divider made of R_ref and the block, with the ADC on the
// midpoint. The voltage the ADC sees follows whatever the expander is driving, as on the board.
class FakeGypsumBoard final : public hal::ScriptedI2cDevice, public hal::IAnalogInput {
 public:
  FakeGypsumBoard(float refOhms, float blockOhms) : ref_(refOhms), block_(blockOhms) {}

  void writeRegister(uint8_t reg, const uint8_t* data, size_t length) override {
    if (length != 1) return;
    writes.push_back({reg, data[0]});
    if (reg == GypsumAcDriver::kRegIodir) iodir = data[0];
    if (reg == GypsumAcDriver::kRegOlat) olat = data[0];
  }
  bool readRegister(uint8_t, uint8_t*, size_t) override { return false; }

  bool readMillivolts(uint8_t, float& out) override {
    const bool a = (olat & GypsumAcDriver::kPinA) && !(iodir & GypsumAcDriver::kPinA);
    const bool b = (olat & GypsumAcDriver::kPinB) && !(iodir & GypsumAcDriver::kPinB) &&
                   !farEndOpen;
    const float va = a ? 3300.0f : 0.0f;
    const float vb = b ? 3300.0f : 0.0f;
    out = vb + (va - vb) * block_ / (ref_ + block_);
    return true;
  }

  struct Write {
    uint8_t reg;
    uint8_t value;
  };
  std::vector<Write> writes;
  uint8_t iodir{0xFF};  // MCP23017 power-on: every pin an input
  uint8_t olat{0x00};
  bool farEndOpen{false};

 private:
  float ref_;
  float block_;
};

void test_pilot_gypsum_ac_uses_the_mcp23017_register_map() {
  // IODIRA and OLATA with IOCON.BANK = 0. 0x0A would be IOCON on this part.
  TEST_ASSERT_EQUAL_HEX8(0x00, GypsumAcDriver::kRegIodir);
  TEST_ASSERT_EQUAL_HEX8(0x14, GypsumAcDriver::kRegOlat);
}

void test_pilot_gypsum_ac_latches_low_before_enabling_outputs() {
  hal::ScriptedI2cBus bus;
  FakeGypsumBoard board(10000.0f, 5000.0f);
  bus.attach(GypsumAcDriver::kDefaultAddress, &board);
  GypsumAcDriver gypsum(bus, board, 33, "GYPSUM-1", 0.6f, 0.0f);
  TEST_ASSERT_TRUE(gypsum.begin());
  TEST_ASSERT_EQUAL(2u, board.writes.size());
  TEST_ASSERT_EQUAL_HEX8(GypsumAcDriver::kRegOlat, board.writes[0].reg);
  TEST_ASSERT_EQUAL_HEX8(0x00, board.writes[0].value);
  TEST_ASSERT_EQUAL_HEX8(GypsumAcDriver::kRegIodir, board.writes[1].reg);
  TEST_ASSERT_EQUAL_HEX8(0xFC, board.writes[1].value);  // GP0 and GP1 out, the rest untouched
}

void test_pilot_gypsum_ac_matches_the_dc_reading_with_zero_net_current() {
  hal::ScriptedI2cBus bus;
  FakeGypsumBoard board(10000.0f, 5000.0f);
  bus.attach(GypsumAcDriver::kDefaultAddress, &board);
  GypsumAcDriver gypsum(bus, board, 33, "GYPSUM-1", 0.6f, 0.0f);
  TEST_ASSERT_TRUE(gypsum.begin());
  board.writes.clear();

  Reading r;
  TEST_ASSERT_TRUE(gypsum.read(Variable::SoilWaterPotential, r));
  // A DC divider from 3V3 through 10k into a 5k block reads 1100 mV.
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 1100.0f, gypsum.lastEquivalentMv());
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 660.0f, r.value);
  // One phase each way and then both ends low: no net charge through the block.
  TEST_ASSERT_EQUAL(3u, board.writes.size());
  TEST_ASSERT_EQUAL_HEX8(GypsumAcDriver::kPinA, board.writes[0].value);
  TEST_ASSERT_EQUAL_HEX8(GypsumAcDriver::kPinB, board.writes[1].value);
  TEST_ASSERT_EQUAL_HEX8(0x00, board.writes[2].value);
  TEST_ASSERT_EQUAL_HEX8(0x00, board.olat);
}

void test_pilot_gypsum_ac_refuses_an_undriven_end_and_a_missing_expander() {
  hal::ScriptedI2cBus bus;
  FakeGypsumBoard board(10000.0f, 5000.0f);
  GypsumAcDriver absent(bus, board, 33, "GYPSUM-1", 0.6f, 0.0f);
  TEST_ASSERT_FALSE(absent.begin());
  TEST_ASSERT_EQUAL(static_cast<int>(SensorStatus::NotConnected),
                    static_cast<int>(absent.health()));

  bus.attach(GypsumAcDriver::kDefaultAddress, &board);
  GypsumAcDriver gypsum(bus, board, 33, "GYPSUM-1", 0.6f, 0.0f);
  TEST_ASSERT_TRUE(gypsum.begin());
  board.farEndOpen = true;
  Reading r;
  TEST_ASSERT_FALSE(gypsum.read(Variable::SoilWaterPotential, r));
  TEST_ASSERT_EQUAL(static_cast<int>(SensorStatus::InvalidData), static_cast<int>(r.status));
  TEST_ASSERT_EQUAL_HEX8(0x00, board.olat);

  // The expander dropping off the bus mid-season is a bus error, not a dry soil reading.
  board.farEndOpen = false;
  bus.detach(GypsumAcDriver::kDefaultAddress);
  TEST_ASSERT_FALSE(gypsum.read(Variable::SoilWaterPotential, r));
  TEST_ASSERT_EQUAL(static_cast<int>(SensorStatus::BusError), static_cast<int>(r.status));
}

void test_pilot_gypsum_ac_switch_survives_a_roundtrip() {
  NodeConfig original{};
  copyString(original.nodeId, sizeof(original.nodeId), "CAUCE-003");
  original.soilPotentialEnabled = true;
  original.soilPotentialAc = true;
  char text[8192];
  TEST_ASSERT_TRUE(serializeConfig(original, text, sizeof(text)));
  NodeConfig parsed{};
  TEST_ASSERT_TRUE(parseConfig(text, parsed));
  TEST_ASSERT_TRUE(parsed.soilPotentialAc);
  NodeConfig defaults{};
  TEST_ASSERT_FALSE(defaults.soilPotentialAc);
}

// A PMS5003 in active mode: every read finds a fresh frame on the line, as the real sensor
// streams one per second whatever was discarded before.
class StreamingPms final : public hal::IUart {
 public:
  bool begin(uint32_t) override { return true; }
  size_t write(const uint8_t*, size_t length) override { return length; }
  size_t read(uint8_t* buffer, size_t length, uint32_t) override {
    if (silent) return 0;
    size_t n = 0;
    for (size_t i = 0; i < noise.size() && n < length; ++i) buffer[n++] = noise[i];
    for (size_t i = 0; i < frame.size() && n < length; ++i) buffer[n++] = frame[i];
    return n;
  }
  void discardInput() override {}

  static std::vector<uint8_t> makeFrame(uint16_t pm1, uint16_t pm25, uint16_t pm10) {
    std::vector<uint8_t> f(32, 0);
    f[0] = 0x42;
    f[1] = 0x4D;
    f[3] = 28;
    const uint16_t atm[3] = {pm1, pm25, pm10};
    for (int i = 0; i < 3; ++i) {
      f[10 + 2 * i] = static_cast<uint8_t>(atm[i] >> 8);
      f[11 + 2 * i] = static_cast<uint8_t>(atm[i] & 0xFF);
      // CF=1 values deliberately different, so reading the wrong block fails the test.
      f[4 + 2 * i] = 0x7F;
    }
    uint16_t sum = 0;
    for (int i = 0; i < 30; ++i) sum = static_cast<uint16_t>(sum + f[i]);
    f[30] = static_cast<uint8_t>(sum >> 8);
    f[31] = static_cast<uint8_t>(sum & 0xFF);
    return f;
  }

  std::vector<uint8_t> noise;
  std::vector<uint8_t> frame;
  bool silent{false};
};

void test_pilot_pms5003_reports_atmospheric_pm_after_warmup() {
  hal::ManualClock clock(kEpoch);
  StreamingPms uart;
  uart.noise = {0x00, 0x42, 0x13};  // a torn frame before the real one
  uart.frame = StreamingPms::makeFrame(7, 12, 18);
  Pms5003Driver pms(uart, clock, "PMS-1");
  TEST_ASSERT_TRUE(pms.begin());
  Reading r;
  TEST_ASSERT_FALSE(pms.read(Variable::Pm25, r));  // the fan is still spinning up
  clock.advanceMs(Pms5003Driver::kWarmupMs);
  TEST_ASSERT_TRUE(pms.read(Variable::Pm1, r));
  TEST_ASSERT_EQUAL_FLOAT(7.0f, r.value);
  TEST_ASSERT_TRUE(pms.read(Variable::Pm25, r));
  TEST_ASSERT_EQUAL_FLOAT(12.0f, r.value);
  TEST_ASSERT_TRUE(pms.read(Variable::Pm10, r));
  TEST_ASSERT_EQUAL_FLOAT(18.0f, r.value);
  TEST_ASSERT_FALSE(pms.read(Variable::Pm4, r));  // not measured by this part
  TEST_ASSERT_EQUAL(3, pms.metadata().variableCount);
}

void test_pilot_pms5003_refuses_a_bad_checksum_and_a_silent_line() {
  std::vector<uint8_t> frame = StreamingPms::makeFrame(1, 2, 3);
  Pms5003Driver::Frame out;
  TEST_ASSERT_TRUE(Pms5003Driver::parse(frame.data(), frame.size(), out));
  frame[12] ^= 0x01;
  TEST_ASSERT_FALSE(Pms5003Driver::parse(frame.data(), frame.size(), out));

  hal::ManualClock clock(kEpoch);
  StreamingPms uart;
  uart.silent = true;
  Pms5003Driver absent(uart, clock, "PMS-1");
  TEST_ASSERT_FALSE(absent.begin());
  TEST_ASSERT_EQUAL(static_cast<int>(SensorStatus::NotConnected),
                    static_cast<int>(absent.health()));
}

void test_pilot_ltr390_converts_uvs_counts_to_a_uv_index() {
  hal::ScriptedI2cBus bus;
  hal::RegisterFileDevice device;
  device.setRegister(Ltr390Driver::kRegPartId, 0xB2);
  bus.attach(Ltr390Driver::kAddress, &device);
  Ltr390Driver uv(bus, "LTR390-1");
  TEST_ASSERT_TRUE(uv.begin());
  TEST_ASSERT_EQUAL_HEX8(Ltr390Driver::kMainCtrlUvsEnable, device.getRegister(Ltr390Driver::kRegMainCtrl));
  TEST_ASSERT_EQUAL_HEX8(Ltr390Driver::kGain18, device.getRegister(Ltr390Driver::kRegGain));

  Reading r;
  TEST_ASSERT_FALSE(uv.read(Variable::UvIndex, r));  // conversion not finished yet
  // 13800 counts = UV index 6 at gain 18, 20 bits.
  device.setRegister(Ltr390Driver::kRegMainStatus, Ltr390Driver::kDataReady);
  device.setRegister(Ltr390Driver::kRegUvsData, 13800 & 0xFF);
  device.setRegister(Ltr390Driver::kRegUvsData + 1, (13800 >> 8) & 0xFF);
  device.setRegister(Ltr390Driver::kRegUvsData + 2, 0);
  TEST_ASSERT_TRUE(uv.read(Variable::UvIndex, r));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 6.0f, r.value);
}

void test_pilot_ltr390_refuses_another_part_at_its_address() {
  hal::ScriptedI2cBus bus;
  hal::RegisterFileDevice device;
  device.setRegister(Ltr390Driver::kRegPartId, 0x12);
  bus.attach(Ltr390Driver::kAddress, &device);
  Ltr390Driver uv(bus, "LTR390-1");
  TEST_ASSERT_FALSE(uv.begin());
  hal::ScriptedI2cBus empty;
  Ltr390Driver missing(empty, "LTR390-1");
  TEST_ASSERT_FALSE(missing.begin());
}

void test_pilot_cheap_sensor_switches_survive_a_roundtrip() {
  NodeConfig original{};
  copyString(original.nodeId, sizeof(original.nodeId), "CAUCE-004");
  original.pmEnabled = true;
  original.pmPms5003 = true;
  original.uvEnabled = true;
  original.uvLtr390 = true;
  char text[8192];
  TEST_ASSERT_TRUE(serializeConfig(original, text, sizeof(text)));
  NodeConfig parsed{};
  TEST_ASSERT_TRUE(parseConfig(text, parsed));
  TEST_ASSERT_TRUE(parsed.pmPms5003);
  TEST_ASSERT_TRUE(parsed.uvLtr390);
  NodeConfig defaults{};
  TEST_ASSERT_FALSE(defaults.pmPms5003);
  TEST_ASSERT_FALSE(defaults.uvLtr390);
}

void registerPilotSensorTests() {
  RUN_TEST(test_pilot_every_variable_has_a_name_a_unit_and_round_trips);
  RUN_TEST(test_pilot_existing_variable_numbers_did_not_move);
  RUN_TEST(test_pilot_every_new_variable_has_a_finite_default_range);
  RUN_TEST(test_pilot_dry_days_and_dark_nights_are_not_stuck_sensors);
  RUN_TEST(test_pilot_a_frozen_nonzero_reading_is_still_flagged);
  RUN_TEST(test_pilot_a_twelve_volt_battery_is_in_range);
  RUN_TEST(test_pilot_sensor_switches_and_calibrations_survive_a_roundtrip);
  RUN_TEST(test_pilot_a_full_config_fits_the_node_buffer);
  RUN_TEST(test_pilot_the_default_region_is_the_uruguayan_band);
  RUN_TEST(test_pilot_sps30_reports_all_four_mass_fractions_from_one_frame);
  RUN_TEST(test_pilot_sps30_refuses_to_report_during_fan_warmup);
  RUN_TEST(test_pilot_sps30_drops_a_frame_with_a_bad_crc);
  RUN_TEST(test_pilot_sps30_crc_matches_the_datasheet_example);
  RUN_TEST(test_pilot_mhz19_reads_co2_after_preheat_with_abc_disabled);
  RUN_TEST(test_pilot_mhz19_rejects_a_reply_with_a_bad_checksum);
  RUN_TEST(test_pilot_mhz19_discards_a_stale_reply_before_asking);
  RUN_TEST(test_pilot_mhz19_without_a_sensor_does_not_start);
  RUN_TEST(test_pilot_veml6075_conversion_follows_the_application_note);
  RUN_TEST(test_pilot_veml6075_never_reports_negative_uv_in_the_dark);
  RUN_TEST(test_pilot_veml6075_refuses_a_part_with_the_wrong_id);
  RUN_TEST(test_pilot_as3935_counts_strikes_and_reports_the_nearest);
  RUN_TEST(test_pilot_as3935_ignores_service_without_an_interrupt);
  RUN_TEST(test_pilot_ds18b20_reads_the_previous_conversion_without_blocking);
  RUN_TEST(test_pilot_ds18b20_handles_negative_temperatures);
  RUN_TEST(test_pilot_ds18b20_rejects_crc_errors_and_the_power_on_value);
  RUN_TEST(test_pilot_ds18b20_without_a_probe_reports_not_connected);
  RUN_TEST(test_pilot_rain_reports_depth_per_interval_and_debounces);
  RUN_TEST(test_pilot_vane_maps_each_heading_and_applies_the_mounting_offset);
  RUN_TEST(test_pilot_vane_reports_an_open_cable_instead_of_a_direction);
  RUN_TEST(test_pilot_soil_moisture_two_point_calibration_is_inverted_and_clamped);
  RUN_TEST(test_pilot_battery_divider_is_a_linear_mapping);
  RUN_TEST(test_pilot_a_calibration_with_no_span_is_refused);
  RUN_TEST(test_pilot_rtc_calendar_conversion_round_trips);
  RUN_TEST(test_pilot_rtc_write_then_read_and_the_stop_flag);
  RUN_TEST(test_pilot_rtc_refuses_an_impossible_date);
  RUN_TEST(test_pilot_gypsum_ac_uses_the_mcp23017_register_map);
  RUN_TEST(test_pilot_gypsum_ac_latches_low_before_enabling_outputs);
  RUN_TEST(test_pilot_gypsum_ac_matches_the_dc_reading_with_zero_net_current);
  RUN_TEST(test_pilot_gypsum_ac_refuses_an_undriven_end_and_a_missing_expander);
  RUN_TEST(test_pilot_gypsum_ac_switch_survives_a_roundtrip);
  RUN_TEST(test_pilot_pms5003_reports_atmospheric_pm_after_warmup);
  RUN_TEST(test_pilot_pms5003_refuses_a_bad_checksum_and_a_silent_line);
  RUN_TEST(test_pilot_ltr390_converts_uvs_counts_to_a_uv_index);
  RUN_TEST(test_pilot_ltr390_refuses_another_part_at_its_address);
  RUN_TEST(test_pilot_cheap_sensor_switches_survive_a_roundtrip);
}
