#include "cauce/drivers/Bme280Driver.h"

namespace cauce::drivers {

namespace {
constexpr uint8_t kChipIdValue = 0x60;
constexpr uint8_t kResetCommand = 0xB6;
constexpr uint8_t kOversamplingX1 = 0x01;
constexpr uint8_t kModeForced = 0x01;
constexpr uint32_t kConversionTimeoutMs = 100;
}  // namespace

Bme280Driver::Bme280Driver(hal::II2cBus& bus, hal::IClock& clock, const char* id,
                           uint8_t i2cAddress)
    : bus_(bus), clock_(clock), address_(i2cAddress) {
  copyString(metadata_.id, sizeof(metadata_.id), id);
  copyString(metadata_.model, sizeof(metadata_.model), "BME280");
  metadata_.variables[0] = Variable::AirTemperature;
  metadata_.variables[1] = Variable::RelativeHumidity;
  metadata_.variables[2] = Variable::Pressure;
  metadata_.variableCount = 3;
  metadata_.resolution = 0.01f;
}

bool Bme280Driver::readTrimming() {
  uint8_t primary[26] = {};
  if (!bus_.readRegisters(address_, 0x88, primary, sizeof(primary))) return false;

  auto getU16le = [&primary](size_t offset) -> uint16_t {
    return static_cast<uint16_t>(primary[offset]) |
           static_cast<uint16_t>(primary[offset + 1]) << 8;
  };
  auto getS16le = [&getU16le](size_t offset) -> int16_t {
    return static_cast<int16_t>(getU16le(offset));
  };

  trim_.digT1 = getU16le(0);
  trim_.digT2 = getS16le(2);
  trim_.digT3 = getS16le(4);
  trim_.digP1 = getU16le(6);
  trim_.digP2 = getS16le(8);
  trim_.digP3 = getS16le(10);
  trim_.digP4 = getS16le(12);
  trim_.digP5 = getS16le(14);
  trim_.digP6 = getS16le(16);
  trim_.digP7 = getS16le(18);
  trim_.digP8 = getS16le(20);
  trim_.digP9 = getS16le(22);
  trim_.digH1 = primary[25];

  uint8_t secondary[10] = {};
  if (!bus_.readRegisters(address_, 0xE1, secondary, sizeof(secondary)))
    return false;

  trim_.digH2 = static_cast<int16_t>(
      static_cast<uint16_t>(secondary[0]) |
      static_cast<uint16_t>(secondary[1]) << 8);
  trim_.digH3 = secondary[2];
  const int16_t h4Raw = static_cast<int16_t>((static_cast<uint16_t>(secondary[3])
                                              << 4) |
                                             (secondary[4] & 0x0F));
  trim_.digH4 =
      (h4Raw & 0x0800) ? static_cast<int16_t>(h4Raw | 0xF000) : h4Raw;
  const int16_t h5Raw =
      static_cast<int16_t>((static_cast<uint16_t>(secondary[5]) << 4) |
                           (secondary[4] >> 4));
  trim_.digH5 =
      (h5Raw & 0x0800) ? static_cast<int16_t>(h5Raw | 0xF000) : h5Raw;
  trim_.digH6 = static_cast<int8_t>(secondary[6]);
  return true;
}

bool Bme280Driver::begin() {
  lastStatus_ = SensorStatus::Initializing;
  uint8_t chipId = 0;
  if (!bus_.readRegisters(address_, static_cast<uint8_t>(Register::ChipId),
                          &chipId, 1) ||
      chipId != kChipIdValue) {
    lastStatus_ = SensorStatus::NotConnected;
    return false;
  }
  const uint8_t reset = kResetCommand;
  bus_.writeRegisters(address_, static_cast<uint8_t>(Register::Reset), &reset, 1);
  if (!readTrimming()) {
    lastStatus_ = SensorStatus::BusError;
    return false;
  }
  const uint8_t ctrlHum = kOversamplingX1;
  if (!bus_.writeRegisters(address_, static_cast<uint8_t>(Register::CtrlHum),
                           &ctrlHum, 1)) {
    lastStatus_ = SensorStatus::BusError;
    return false;
  }
  const uint8_t config = 0x00;
  bus_.writeRegisters(address_, static_cast<uint8_t>(Register::Config), &config, 1);
  lastStatus_ = SensorStatus::Ok;
  return true;
}

SensorStatus Bme280Driver::health() { return lastStatus_; }

bool Bme280Driver::triggerConversion() {
  const uint8_t ctrlMeas =
      static_cast<uint8_t>((kOversamplingX1 << 5) | (kOversamplingX1 << 2) |
                           kModeForced);
  return bus_.writeRegisters(address_, static_cast<uint8_t>(Register::CtrlMeas),
                             &ctrlMeas, 1);
}

bool Bme280Driver::waitForConversion() {
  const uint32_t startMs = clock_.monotonicMs();
  while (clock_.monotonicMs() - startMs < kConversionTimeoutMs) {
    uint8_t status = 0;
    if (!bus_.readRegisters(address_, static_cast<uint8_t>(Register::Status),
                            &status, 1)) {
      lastStatus_ = SensorStatus::BusError;
      return false;
    }
    if ((status & 0x08) == 0) return true;
  }
  lastStatus_ = SensorStatus::BusError;
  return false;
}

bool Bme280Driver::burstRead(Bme280Raw& raw) {
  uint8_t data[8] = {};
  if (!bus_.readRegisters(address_, static_cast<uint8_t>(Register::PressMsb),
                          data, sizeof(data))) {
    lastStatus_ = SensorStatus::BusError;
    return false;
  }
  raw.pressure = (static_cast<uint32_t>(data[0]) << 12) |
                 (static_cast<uint32_t>(data[1]) << 4) |
                 (static_cast<uint32_t>(data[2]) >> 4);
  raw.temperature = (static_cast<uint32_t>(data[3]) << 12) |
                    (static_cast<uint32_t>(data[4]) << 4) |
                    (static_cast<uint32_t>(data[5]) >> 4);
  raw.humidity = (static_cast<uint32_t>(data[6]) << 8) |
                 static_cast<uint32_t>(data[7]);
  if (raw.temperature == 0x80000 || raw.pressure == 0x80000 ||
      raw.humidity == 0x8000) {
    lastStatus_ = SensorStatus::InvalidData;
    return false;
  }
  return true;
}

bool Bme280Driver::read(Variable variable, Reading& outReading) {
  outReading.ok = false;
  outReading.value = 0.0f;
  outReading.status = health();
  if (lastStatus_ != SensorStatus::Ok) return false;
  if (variable != Variable::AirTemperature &&
      variable != Variable::RelativeHumidity && variable != Variable::Pressure) {
    outReading.status = SensorStatus::InvalidData;
    return false;
  }
  if (!triggerConversion()) {
    lastStatus_ = SensorStatus::BusError;
    outReading.status = lastStatus_;
    return false;
  }
  if (!waitForConversion()) {
    outReading.status = lastStatus_;
    return false;
  }
  Bme280Raw raw{};
  if (!burstRead(raw)) {
    outReading.status = lastStatus_;
    return false;
  }
  int32_t tFine = 0;
  bme280CompensateTemperature(trim_, raw.temperature, tFine);
  switch (variable) {
    case Variable::AirTemperature:
      outReading.value = bme280TemperatureC(trim_, raw.temperature);
      break;
    case Variable::Pressure:
      outReading.value = bme280PressurePa(trim_, raw.pressure, tFine) / 100.0f;
      break;
    case Variable::RelativeHumidity:
      outReading.value = bme280HumidityPct(trim_, raw.humidity, tFine);
      break;
    default:
      break;
  }
  outReading.ok = true;
  outReading.status = SensorStatus::Ok;
  return true;
}

const SensorMetadata& Bme280Driver::metadata() const { return metadata_; }

}  // namespace cauce::drivers
