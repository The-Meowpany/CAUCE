#include <cmath>
#include <cstring>

#include <unity.h>

#include "cauce/drivers/Bme280Driver.h"
#include "cauce/hal/ManualClock.h"
#include "cauce/hal/ScriptedI2c.h"

using namespace cauce;
using namespace cauce::drivers;

namespace {

Bme280Trim datasheetTrim() {
  Bme280Trim t{};
  t.digT1 = 27504;
  t.digT2 = 26435;
  t.digT3 = -1000;
  t.digP1 = 36477;
  t.digP2 = -10685;
  t.digP3 = 3024;
  t.digP4 = 2855;
  t.digP5 = 140;
  t.digP6 = -7;
  t.digP7 = 15500;
  t.digP8 = -14600;
  t.digP9 = 6000;
  t.digH1 = 200;
  t.digH2 = 365;
  t.digH3 = 0;
  t.digH4 = 290;
  t.digH5 = 214;
  t.digH6 = -120;
  return t;
}

}  // namespace

void test_temperature_matches_datasheet_example() {
  const Bme280Trim trim = datasheetTrim();
  const float integerResult =
      bme280TemperatureC(trim, 519888);
  const double referenceResult =
      bme280TemperatureCReference(trim, 519888);
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 25.08f, integerResult);
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 25.08f, referenceResult);
}

void test_integer_and_reference_pressure_agree() {
  const Bme280Trim trim = datasheetTrim();
  int32_t tFine = 0;
  bme280CompensateTemperature(trim, 519888, tFine);
  const uint32_t fixedPointPa =
      bme280CompensatePressure(trim, 415148, tFine);
  const float integerPa = static_cast<float>(fixedPointPa) / 256.0f;
  const double referencePa = bme280PressurePaReference(trim, 415148, tFine);
  TEST_ASSERT_FLOAT_WITHIN(2.0f, static_cast<float>(referencePa), integerPa);
  TEST_ASSERT_FLOAT_WITHIN(300.0f, 100653.0f, integerPa);
}

void test_humidity_in_plausible_range_and_models_agree() {
  const Bme280Trim trim = datasheetTrim();
  int32_t tFine = 0;
  bme280CompensateTemperature(trim, 519888, tFine);
  const float integerPct = bme280HumidityPct(trim, 35000, tFine);
  const double referencePct = bme280HumidityPctReference(trim, 35000, tFine);
  TEST_ASSERT_TRUE(integerPct > 0.0f && integerPct < 100.0f);
  TEST_ASSERT_FLOAT_WITHIN(0.5f, static_cast<float>(referencePct), integerPct);
}

namespace {

hal::ScriptedI2cBus bus;
hal::RegisterFileDevice device;
hal::ManualClock clockMs(1787356800000ULL);

void loadBme280Registers(uint32_t adcT, uint32_t adcP, uint32_t adcH) {
  const Bme280Trim t = datasheetTrim();
  device.setRegister(0xD0, 0x60);

  auto putU16le = [](uint8_t reg, uint16_t v) {
    device.setRegister(reg, static_cast<uint8_t>(v & 0xFF));
    device.setRegister(static_cast<uint8_t>(reg + 1),
                       static_cast<uint8_t>(v >> 8));
  };
  putU16le(0x88, t.digT1);
  putU16le(0x8A, static_cast<uint16_t>(t.digT2));
  putU16le(0x8C, static_cast<uint16_t>(t.digT3));
  putU16le(0x8E, t.digP1);
  putU16le(0x90, static_cast<uint16_t>(t.digP2));
  putU16le(0x92, static_cast<uint16_t>(t.digP3));
  putU16le(0x94, static_cast<uint16_t>(t.digP4));
  putU16le(0x96, static_cast<uint16_t>(t.digP5));
  putU16le(0x98, static_cast<uint16_t>(t.digP6));
  putU16le(0x9A, static_cast<uint16_t>(t.digP7));
  putU16le(0x9C, static_cast<uint16_t>(t.digP8));
  putU16le(0x9E, static_cast<uint16_t>(t.digP9));

  device.setRegister(0xA1, t.digH1);
  putU16le(0xE1, static_cast<uint16_t>(t.digH2));
  device.setRegister(0xE3, t.digH3);
  const unsigned h4 = static_cast<unsigned>(t.digH4) & 0x0FFF;
  const unsigned h5 = static_cast<unsigned>(t.digH5) & 0x0FFF;
  device.setRegister(0xE4, static_cast<uint8_t>((h4 >> 4) & 0xFF));
  device.setRegister(0xE6, static_cast<uint8_t>((h5 >> 4) & 0xFF));
  device.setRegister(0xE5, static_cast<uint8_t>(((h5 & 0xF) << 4) | (h4 & 0xF)));
  device.setRegister(0xE7, static_cast<uint8_t>(static_cast<int8_t>(t.digH6)));

  device.setRegister(0xF7, static_cast<uint8_t>((adcP >> 12) & 0xFF));
  device.setRegister(0xF8, static_cast<uint8_t>((adcP >> 4) & 0xFF));
  device.setRegister(0xF9, static_cast<uint8_t>((adcP << 4) & 0xF0));
  device.setRegister(0xFA, static_cast<uint8_t>((adcT >> 12) & 0xFF));
  device.setRegister(0xFB, static_cast<uint8_t>((adcT >> 4) & 0xFF));
  device.setRegister(0xFC, static_cast<uint8_t>((adcT << 4) & 0xF0));
  device.setRegister(0xFD, static_cast<uint8_t>((adcH >> 8) & 0xFF));
  device.setRegister(0xFE, static_cast<uint8_t>(adcH & 0xFF));
}

}  // namespace

void test_driver_reads_compensated_values_over_i2c() {
  loadBme280Registers(519888, 415148, 35000);
  device.setRegister(0xF3, 0x00);
  bus.attach(Bme280Driver::kDefaultAddress, &device);

  Bme280Driver driver(bus, clockMs, "BME-T");
  TEST_ASSERT_TRUE(driver.begin());
  TEST_ASSERT_EQUAL(SensorStatus::Ok, driver.health());

  Reading r{};
  TEST_ASSERT_TRUE(driver.read(Variable::AirTemperature, r));
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 25.08f, r.value);

  TEST_ASSERT_TRUE(driver.read(Variable::Pressure, r));
  TEST_ASSERT_FLOAT_WITHIN(5.0f, 1006.53f, r.value);

  TEST_ASSERT_TRUE(driver.read(Variable::RelativeHumidity, r));
  TEST_ASSERT_TRUE(r.value >= 0.0f && r.value <= 100.0f);
}

void test_driver_reports_disconnected_sensor() {
  bus.detach(Bme280Driver::kDefaultAddress);
  Bme280Driver driver(bus, clockMs, "BME-T2");
  TEST_ASSERT_FALSE(driver.begin());
  TEST_ASSERT_EQUAL(SensorStatus::NotConnected, driver.health());

  Reading r{};
  TEST_ASSERT_FALSE(driver.read(Variable::AirTemperature, r));
  TEST_ASSERT_EQUAL(SensorStatus::NotConnected, r.status);
}

void registerBme280Tests() {

  RUN_TEST(test_temperature_matches_datasheet_example);
  RUN_TEST(test_integer_and_reference_pressure_agree);
  RUN_TEST(test_humidity_in_plausible_range_and_models_agree);
  RUN_TEST(test_driver_reads_compensated_values_over_i2c);
  RUN_TEST(test_driver_reports_disconnected_sensor);
}
