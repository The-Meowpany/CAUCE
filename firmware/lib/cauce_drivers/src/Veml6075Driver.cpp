#include "cauce/drivers/Veml6075Driver.h"

#include "cauce/core/Types.h"

namespace cauce::drivers {

namespace {

constexpr uint8_t kRegConf = 0x00;
constexpr uint8_t kRegUva = 0x07;
constexpr uint8_t kRegUvb = 0x09;
constexpr uint8_t kRegComp1 = 0x0A;
constexpr uint8_t kRegComp2 = 0x0B;
constexpr uint8_t kRegId = 0x0C;

// Power on, 100 ms integration, normal dynamic range, continuous mode.
constexpr uint8_t kConf100ms = 0x10;

// Application note "Designing the VEML6075 into an Application", open-air coefficients.
constexpr float kA = 2.22f;
constexpr float kB = 1.33f;
constexpr float kC = 2.95f;
constexpr float kD = 1.74f;
constexpr float kUvaResponsivity = 0.001461f;
constexpr float kUvbResponsivity = 0.002591f;

}  // namespace

Veml6075Driver::Index Veml6075Driver::convert(const Raw& raw) {
  const float uvaCalc = static_cast<float>(raw.uva) - kA * raw.comp1 - kB * raw.comp2;
  const float uvbCalc = static_cast<float>(raw.uvb) - kC * raw.comp1 - kD * raw.comp2;
  Index out;
  // The compensation can push a dark reading slightly negative; there is no negative UV.
  out.uva = uvaCalc > 0.0f ? uvaCalc * kUvaResponsivity : 0.0f;
  out.uvb = uvbCalc > 0.0f ? uvbCalc * kUvbResponsivity : 0.0f;
  out.uvi = (out.uva + out.uvb) / 2.0f;
  return out;
}

Veml6075Driver::Veml6075Driver(hal::II2cBus& bus, const char* id) : bus_(bus) {
  copyString(metadata_.id, sizeof(metadata_.id), id);
  copyString(metadata_.model, sizeof(metadata_.model), "VEML6075");
  metadata_.variables[0] = Variable::UvIndex;
  metadata_.variables[1] = Variable::UvaIndex;
  metadata_.variables[2] = Variable::UvbIndex;
  metadata_.variableCount = 3;
  metadata_.resolution = 0.01f;
}

bool Veml6075Driver::readWord(uint8_t reg, uint16_t& out) {
  uint8_t b[2] = {0, 0};
  if (!bus_.readRegisters(kAddress, reg, b, 2)) return false;
  out = static_cast<uint16_t>(b[0] | (static_cast<uint16_t>(b[1]) << 8));  // little-endian
  return true;
}

bool Veml6075Driver::begin() {
  if (!bus_.isPresent(kAddress)) {
    lastStatus_ = SensorStatus::NotConnected;
    return false;
  }
  uint16_t id = 0;
  if (!readWord(kRegId, id)) {
    lastStatus_ = SensorStatus::BusError;
    return false;
  }
  if ((id & 0xFF) != kDeviceId) {
    // Something answers at 0x10 but it is not this part; reading it as UV would be invented data.
    lastStatus_ = SensorStatus::InvalidData;
    return false;
  }
  const uint8_t conf[2] = {kConf100ms, 0x00};
  if (!bus_.writeRegisters(kAddress, kRegConf, conf, 2)) {
    lastStatus_ = SensorStatus::BusError;
    return false;
  }
  configured_ = true;
  lastStatus_ = SensorStatus::Ok;
  return true;
}

SensorStatus Veml6075Driver::health() {
  if (!configured_) return lastStatus_;
  lastStatus_ = bus_.isPresent(kAddress) ? SensorStatus::Ok : SensorStatus::NotConnected;
  return lastStatus_;
}

bool Veml6075Driver::read(Variable variable, Reading& outReading) {
  outReading = Reading{};
  if (variable != Variable::UvIndex && variable != Variable::UvaIndex &&
      variable != Variable::UvbIndex) {
    outReading.status = SensorStatus::InvalidData;
    return false;
  }
  if (!configured_ && !begin()) {
    outReading.status = lastStatus_;
    return false;
  }
  Raw raw;
  if (!readWord(kRegUva, raw.uva) || !readWord(kRegUvb, raw.uvb) ||
      !readWord(kRegComp1, raw.comp1) || !readWord(kRegComp2, raw.comp2)) {
    lastStatus_ = SensorStatus::BusError;
    outReading.status = lastStatus_;
    return false;
  }
  const Index index = convert(raw);
  outReading.ok = true;
  outReading.status = SensorStatus::Ok;
  outReading.value = variable == Variable::UvaIndex   ? index.uva
                     : variable == Variable::UvbIndex ? index.uvb
                                                      : index.uvi;
  return true;
}

const SensorMetadata& Veml6075Driver::metadata() const { return metadata_; }

}  // namespace cauce::drivers
