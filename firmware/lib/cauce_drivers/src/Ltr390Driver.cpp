#include "cauce/drivers/Ltr390Driver.h"

#include "cauce/core/Types.h"

namespace cauce::drivers {

Ltr390Driver::Ltr390Driver(hal::II2cBus& bus, const char* id) : bus_(bus) {
  copyString(metadata_.id, sizeof(metadata_.id), id);
  copyString(metadata_.model, sizeof(metadata_.model), "LTR390");
  metadata_.variables[0] = Variable::UvIndex;
  metadata_.variableCount = 1;
  metadata_.resolution = 1.0f / kCountsPerUvIndex;
}

float Ltr390Driver::uvIndex(uint32_t counts) {
  return static_cast<float>(counts & 0xFFFFFu) / kCountsPerUvIndex;
}

bool Ltr390Driver::writeRegister(uint8_t reg, uint8_t value) {
  return bus_.writeRegisters(kAddress, reg, &value, 1);
}

bool Ltr390Driver::begin() {
  uint8_t partId = 0;
  if (!bus_.isPresent(kAddress) || !bus_.readRegisters(kAddress, kRegPartId, &partId, 1)) {
    lastStatus_ = SensorStatus::NotConnected;
    return false;
  }
  // A different part answering at 0x53 would be read as UV counts that mean nothing.
  if ((partId & 0xF0) != kPartIdHigh) {
    lastStatus_ = SensorStatus::InvalidData;
    return false;
  }
  if (!writeRegister(kRegGain, kGain18) || !writeRegister(kRegMeasRate, kMeasRate20Bit500Ms) ||
      !writeRegister(kRegMainCtrl, kMainCtrlUvsEnable)) {
    lastStatus_ = SensorStatus::BusError;
    return false;
  }
  lastStatus_ = SensorStatus::Ok;
  return true;
}

SensorStatus Ltr390Driver::health() { return lastStatus_; }

bool Ltr390Driver::read(Variable variable, Reading& outReading) {
  outReading = Reading{};
  if (variable != Variable::UvIndex) {
    outReading.status = SensorStatus::InvalidData;
    return false;
  }
  uint8_t status = 0;
  uint8_t data[3] = {};
  if (!bus_.readRegisters(kAddress, kRegMainStatus, &status, 1)) {
    lastStatus_ = SensorStatus::BusError;
    outReading.status = lastStatus_;
    return false;
  }
  if ((status & kDataReady) == 0) {
    // Not an error: the 500 ms conversion has not finished since the last read.
    outReading.status = SensorStatus::Initializing;
    return false;
  }
  if (!bus_.readRegisters(kAddress, kRegUvsData, data, sizeof(data))) {
    lastStatus_ = SensorStatus::BusError;
    outReading.status = lastStatus_;
    return false;
  }
  const uint32_t counts = static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
                          (static_cast<uint32_t>(data[2] & 0x0F) << 16);
  lastStatus_ = SensorStatus::Ok;
  outReading.ok = true;
  outReading.status = SensorStatus::Ok;
  outReading.value = uvIndex(counts);
  return true;
}

const SensorMetadata& Ltr390Driver::metadata() const { return metadata_; }

}  // namespace cauce::drivers
