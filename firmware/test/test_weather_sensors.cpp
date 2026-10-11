#include <cmath>
#include <cstring>

#include <unity.h>

#include "cauce/drivers/Bh1750Driver.h"
#include "cauce/drivers/Ccs811Driver.h"
#include "cauce/drivers/HallAnemometerDriver.h"
#include "cauce/drivers/WindAccumulator.h"
#include "cauce/hal/ManualClock.h"
#include "cauce/hal/ScriptedI2c.h"

using namespace cauce;
using namespace cauce::drivers;

namespace {

// Feeds pulses into a driver's accumulator the way an ISR would, so the counting path is the
// one under test rather than a test-only shortcut through read().
class TestPulseSource final : public IPulseSource {
 public:
  void attachPulseSink(WindPulseAccumulator* sink) override { sink_ = sink; }
  void pulse() {
    if (sink_) sink_->addPulse(clockNowMs_);
  }
  void setNow(uint32_t nowMs) { clockNowMs_ = nowMs; }

 private:
  WindPulseAccumulator* sink_{nullptr};
  uint32_t clockNowMs_{0};
};

}  // namespace

// ---------------------------------------------------------------------------
// Wind geometry
// ---------------------------------------------------------------------------

void test_the_calibration_constant_is_the_documented_formula() {
  // pi * D metres of travel per revolution, divided among the pulses that mark it.
  const float d = 0.15f;
  const uint8_t n = 3;
  const float expected = (3.14159265358979f * d) / n;
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, expected,
                           deriveMetresPerSecondPerPulsePerSecond(d, n));
}

void test_a_zero_geometry_yields_no_calibration_rather_than_a_huge_one() {
  // Dividing by a zero diameter or a zero pulse count would otherwise produce infinity, and an
  // infinite metres-per-pulse turns every later reading into infinity too.
  TEST_ASSERT_EQUAL_FLOAT(0.0f,
                          deriveMetresPerSecondPerPulsePerSecond(0.0f, 3));
  TEST_ASSERT_EQUAL_FLOAT(0.0f,
                          deriveMetresPerSecondPerPulsePerSecond(0.15f, 0));
}

// ---------------------------------------------------------------------------
// Wind counting
// ---------------------------------------------------------------------------

void test_known_pulse_train_converts_to_the_expected_speed() {
  // 60 pulses in 10 s is 6 Hz; with pi*0.15/3 m per pulse that is about 0.942 m/s.
  WindPulseAccumulator acc;
  acc.setGeometry(0.15f, 3);
  acc.addPulse(0);
  for (int i = 1; i < 60; ++i) acc.addPulse(1000 * static_cast<uint32_t>(i) / 60);
  const WindPulseAccumulator::Speed s = acc.takeSpeed(10000);
  TEST_ASSERT_TRUE(s.valid);
  TEST_ASSERT_EQUAL_UINT32(60, s.pulses);
  TEST_ASSERT_EQUAL_UINT32(10000, s.intervalMs);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.942f, s.metresPerSecond);
}

void test_still_air_is_reported_as_zero_not_as_a_missing_reading() {
  // An interval long enough, with no pulses at all, is a real measurement of calm - and calm is
  // the ordinary state of a cup anemometer, so this path has to work without a pulse ever
  // arriving to open the window.
  WindPulseAccumulator acc;
  acc.setGeometry(0.15f, 3);
  acc.beginInterval(0);
  const WindPulseAccumulator::Speed s = acc.takeSpeed(5000);
  TEST_ASSERT_TRUE(s.valid);
  TEST_ASSERT_EQUAL_UINT32(0, s.pulses);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, s.metresPerSecond);
}

void test_calm_is_reportable_repeatedly_not_only_once() {
  WindPulseAccumulator acc;
  acc.setGeometry(0.15f, 3);
  acc.beginInterval(0);
  for (uint32_t t = 5000; t <= 20000; t += 5000) {
    const WindPulseAccumulator::Speed s = acc.takeSpeed(t);
    TEST_ASSERT_TRUE(s.valid);
    TEST_ASSERT_EQUAL_UINT32(0, s.pulses);
  }
}

void test_a_short_interval_is_refused_rather_than_reported_as_calm() {
  // The distinction the whole driver exists to make: not-yet-measured is not zero wind.
  WindPulseAccumulator acc;
  acc.setGeometry(0.15f, 3);
  acc.addPulse(0);
  acc.addPulse(10);
  const WindPulseAccumulator::Speed s = acc.takeSpeed(100);
  TEST_ASSERT_FALSE(s.valid);
}

void test_an_interval_that_is_too_short_still_consumes_its_pulses() {
  // If a rejected interval kept its pulses, the next interval would inherit them and overstate
  // the speed. The pulses must be discarded along with the interval.
  WindPulseAccumulator acc;
  acc.setGeometry(0.15f, 3);
  acc.addPulse(0);
  acc.addPulse(10);
  const WindPulseAccumulator::Speed rejected = acc.takeSpeed(100);
  TEST_ASSERT_FALSE(rejected.valid);

  // A clean 10 s window with no pulses must read calm, not inherit the two discarded pulses.
  const WindPulseAccumulator::Speed next = acc.takeSpeed(10100);
  TEST_ASSERT_TRUE(next.valid);
  TEST_ASSERT_EQUAL_UINT32(0, next.pulses);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, next.metresPerSecond);
}

void test_reading_before_any_pulse_reports_not_yet_measuring() {
  WindPulseAccumulator acc;
  acc.setGeometry(0.15f, 3);
  const WindPulseAccumulator::Speed s = acc.takeSpeed(10000);
  TEST_ASSERT_FALSE(s.valid);
  TEST_ASSERT_EQUAL_UINT32(0, s.intervalMs);
}

void test_the_driver_refuses_a_variable_it_does_not_carry() {
  hal::ManualClock clock;
  TestPulseSource source;
  HallAnemometerDriver driver(clock, "WIND-1", source);
  Reading r;
  TEST_ASSERT_FALSE(driver.read(Variable::AirTemperature, r));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SensorStatus::InvalidData),
                          static_cast<uint8_t>(r.status));
}

void test_the_driver_reports_wind_speed_after_pulses_arrive() {
  hal::ManualClock clock;
  TestPulseSource source;
  HallAnemometerDriver driver(clock, "WIND-1", source);
  TEST_ASSERT_TRUE(driver.begin());

  source.setNow(0);
  driver.accumulator().addPulse(0);
  for (int i = 1; i < 60; ++i) {
    const uint32_t nowMs = static_cast<uint32_t>(1000 * i / 60);
    source.setNow(nowMs);
    driver.accumulator().addPulse(nowMs);
  }
  clock.advanceMs(10000);

  Reading r;
  TEST_ASSERT_TRUE(driver.read(Variable::WindSpeed, r));
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_FLOAT_WITHIN(0.02f, 0.942f, r.value);
}

void test_the_driver_declares_the_wind_variable() {
  hal::ManualClock clock;
  TestPulseSource source;
  HallAnemometerDriver driver(clock, "WIND-1", source);
  TEST_ASSERT_EQUAL_UINT8(1, driver.metadata().variableCount);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Variable::WindSpeed),
                          static_cast<uint8_t>(driver.metadata().variables[0]));
  TEST_ASSERT_EQUAL_STRING("m/s", variableUnit(Variable::WindSpeed));
  TEST_ASSERT_EQUAL_STRING("wind_speed", variableName(Variable::WindSpeed));
}

// ---------------------------------------------------------------------------
// BH1750
// ---------------------------------------------------------------------------

void test_the_bh1750_converts_using_the_datasheet_divisor() {
  hal::ScriptedI2cBus bus;
  hal::RegisterFileDevice dev;
  bus.attach(Bh1750Driver::kDefaultAddress, &dev);
  hal::ManualClock clock;
  Bh1750Driver driver(bus, clock, "LIGHT-1");

  TEST_ASSERT_TRUE(driver.begin());
  const uint16_t raw = 300;
  const uint8_t bytes[2] = {static_cast<uint8_t>(raw >> 8), static_cast<uint8_t>(raw & 0xFF)};
  dev.setRegistersFromBytes(0x00, bytes, 2);

  Reading r;
  TEST_ASSERT_TRUE(driver.read(Variable::Light, r));
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 250.0f, r.value);
}

void test_the_bh1750_refuses_a_missing_device() {
  hal::ScriptedI2cBus bus;
  hal::ManualClock clock;
  Bh1750Driver driver(bus, clock, "LIGHT-1");
  TEST_ASSERT_FALSE(driver.begin());
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SensorStatus::NotConnected),
                          static_cast<uint8_t>(driver.health()));
}

void test_the_bh1750_publishes_the_accuracy_the_field_plan_must_budget_for() {
  hal::ScriptedI2cBus bus;
  hal::ManualClock clock;
  Bh1750Driver driver(bus, clock, "LIGHT-1");
  // The datasheet is +/-20 %. Anything else in this field would be a number a calibrator
  // budgets against, so it is asserted rather than left to drift.
  TEST_ASSERT_EQUAL_FLOAT(20.0f, driver.metadata().accuracyNote);
}

// ---------------------------------------------------------------------------
// CCS811
// ---------------------------------------------------------------------------

void test_the_ccs811_reports_tvoc_and_never_co2() {
  hal::ScriptedI2cBus bus;
  hal::RegisterFileDevice dev;
  bus.attach(Ccs811Driver::kDefaultAddress, &dev);
  hal::ManualClock clock(0, 0);
  Ccs811Driver driver(bus, clock, "VOC-1");

  TEST_ASSERT_TRUE(driver.begin());
  clock.advanceMs(Ccs811Driver::kWarmupMs + 1000);

  dev.setRegister(0x00, 0x18);  // app valid + data ready
  const uint8_t result[4] = {0x00, 0x64, 0x04, 0x00};  // 100 ppb TVOC, 1024 eCO2
  dev.setRegistersFromBytes(0x02, result, 4);

  Reading r;
  TEST_ASSERT_TRUE(driver.read(Variable::Tvoc, r));
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 100.0f, r.value);

  // The eCO2 axis is readable in the same bytes and is deliberately not exposed: it is derived
  // from VOC, not measured, and a node that reports it would publish a CO2 figure nobody can
  // defend. There is no Variable to ask for it with, which is the point.
  TEST_ASSERT_EQUAL_UINT8(1, driver.metadata().variableCount);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Variable::Tvoc),
                          static_cast<uint8_t>(driver.metadata().variables[0]));
}

void test_the_ccs811_refuses_to_read_before_it_has_warmed_up() {
  hal::ScriptedI2cBus bus;
  hal::RegisterFileDevice dev;
  bus.attach(Ccs811Driver::kDefaultAddress, &dev);
  hal::ManualClock clock;
  Ccs811Driver driver(bus, clock, "VOC-1");
  TEST_ASSERT_TRUE(driver.begin());

  // Still warming. Reporting 0 ppb here would be indistinguishable from genuinely clean air.
  Reading r;
  TEST_ASSERT_FALSE(driver.read(Variable::Tvoc, r));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SensorStatus::Initializing),
                          static_cast<uint8_t>(r.status));
  TEST_ASSERT_FALSE(driver.warmedUp());
}

void test_the_ccs811_reports_initializing_when_no_sample_is_ready_yet() {
  hal::ScriptedI2cBus bus;
  hal::RegisterFileDevice dev;
  bus.attach(Ccs811Driver::kDefaultAddress, &dev);
  hal::ManualClock clock;
  Ccs811Driver driver(bus, clock, "VOC-1");
  TEST_ASSERT_TRUE(driver.begin());
  clock.advanceMs(Ccs811Driver::kWarmupMs + 1000);

  dev.setRegister(0x00, 0x10);  // app valid, data not ready
  Reading r;
  TEST_ASSERT_FALSE(driver.read(Variable::Tvoc, r));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SensorStatus::Initializing),
                          static_cast<uint8_t>(r.status));
}

void test_the_ccs811_has_no_co2_variable_at_all() {
  // The schema-level half of the same decision: if a CO2 quantity existed, some other driver
  // could publish it and the argument in the header would quietly stop holding.
  TEST_ASSERT_EQUAL_UINT8(255, static_cast<uint8_t>(Variable::Unknown));
  TEST_ASSERT_EQUAL_INT(Variable::Unknown, parseVariable("co2"));
  TEST_ASSERT_EQUAL_INT(Variable::Unknown, parseVariable("eco2"));
  TEST_ASSERT_EQUAL_INT(Variable::Tvoc, parseVariable("tvoc"));
  TEST_ASSERT_EQUAL_STRING("ppb", variableUnit(Variable::Tvoc));
}

void registerWeatherSensorsTests() {
  UNITY_BEGIN();
  RUN_TEST(test_the_calibration_constant_is_the_documented_formula);
  RUN_TEST(test_a_zero_geometry_yields_no_calibration_rather_than_a_huge_one);
  RUN_TEST(test_known_pulse_train_converts_to_the_expected_speed);
  RUN_TEST(test_still_air_is_reported_as_zero_not_as_a_missing_reading);
  RUN_TEST(test_calm_is_reportable_repeatedly_not_only_once);
  RUN_TEST(test_a_short_interval_is_refused_rather_than_reported_as_calm);
  RUN_TEST(test_an_interval_that_is_too_short_still_consumes_its_pulses);
  RUN_TEST(test_reading_before_any_pulse_reports_not_yet_measuring);
  RUN_TEST(test_the_driver_refuses_a_variable_it_does_not_carry);
  RUN_TEST(test_the_driver_reports_wind_speed_after_pulses_arrive);
  RUN_TEST(test_the_driver_declares_the_wind_variable);
  RUN_TEST(test_the_bh1750_converts_using_the_datasheet_divisor);
  RUN_TEST(test_the_bh1750_refuses_a_missing_device);
  RUN_TEST(test_the_bh1750_publishes_the_accuracy_the_field_plan_must_budget_for);
  RUN_TEST(test_the_ccs811_reports_tvoc_and_never_co2);
  RUN_TEST(test_the_ccs811_refuses_to_read_before_it_has_warmed_up);
  RUN_TEST(test_the_ccs811_reports_initializing_when_no_sample_is_ready_yet);
  RUN_TEST(test_the_ccs811_has_no_co2_variable_at_all);
  UNITY_END();
}