#include "cauce/drivers/Ccs811Driver.h"

#include "cauce/core/Types.h"

namespace cauce::drivers {

namespace {
constexpr uint8_t kModeIdle = 0x00;
constexpr uint8_t kModePeriodic1s = 0x01;
constexpr uint8_t kModePeriodic10s = 0x02;
constexpr uint8_t kModeSingle = 0x04;

constexpr uint8_t kStatusDataReady = 0x08;
constexpr uint8_t kStatusAppValid = 0x10;
}  // namespace

Ccs811Driver::Ccs811Driver(hal::II2cBus& bus, hal::IClock& clock, const char* id,
                           uint8_t i2cAddress)
    : bus_(bus), clock_(clock), address_(i2cAddress) {
  copyString(metadata_.id, sizeof(metadata_.id), id);
  copyString(metadata_.model, sizeof(metadata_.model), "CCS811");
  // Only TVOC. See the header: the eCO2 axis is readable in silicon and absent here on
  // purpose, because it is an estimate of a gas this part cannot measure.
  metadata_.variables[0] = Variable::Tvoc;
  metadata_.variableCount = 1;
  metadata_.accuracyNote = 0.0f;  // the part publishes no accuracy figure for TVOC
  metadata_.resolution = 1.0f;    // ppb
}

bool Ccs811Driver::writeMode(uint8_t mode) {
  if (!bus_.writeRegisters(address_, static_cast<uint8_t>(Register::MeasureMode), &mode, 1)) {
    lastStatus_ = SensorStatus::BusError;
    return false;
  }
  clock_.sleepMs(2);
  return true;
}

bool Ccs811Driver::begin() {
  if (!bus_.isPresent(address_)) {
    lastStatus_ = SensorStatus::NotConnected;
    return false;
  }
  startedMs_ = clock_.monotonicMs();
  if (!writeMode(kModePeriodic10s)) return false;
  configured_ = true;
  lastStatus_ = SensorStatus::Initializing;
  return true;
}

bool Ccs811Driver::readStatus(uint8_t& outStatus) {
  outStatus = 0;
  if (!bus_.readRegisters(address_, static_cast<uint8_t>(Register::Status), &outStatus, 1)) {
    lastStatus_ = SensorStatus::BusError;
    return false;
  }
  return true;
}

bool Ccs811Driver::readAlgorithmResult(uint16_t& outTvoc, uint16_t& outEco2) {
  outTvoc = 0;
  outEco2 = 0;
  uint8_t buffer[4] = {0, 0, 0, 0};
  if (!bus_.readRegisters(address_, static_cast<uint8_t>(Register::AlgorithmResult), buffer, 4)) {
    lastStatus_ = SensorStatus::BusError;
    return false;
  }
  outTvoc = static_cast<uint16_t>((static_cast<uint16_t>(buffer[0]) << 8) | buffer[1]);
  outEco2 = static_cast<uint16_t>((static_cast<uint16_t>(buffer[2]) << 8) | buffer[3]);
  return true;
}

bool Ccs811Driver::warmedUp() const {
  if (!configured_) return false;
  return (clock_.monotonicMs() - startedMs_) >= kWarmupMs;
}

SensorStatus Ccs811Driver::health() {
  if (!configured_) return lastStatus_;
  uint8_t status = 0;
  if (!readStatus(status)) return lastStatus_;
  if ((status & kStatusAppValid) == 0) {
    lastStatus_ = SensorStatus::Initializing;
    return lastStatus_;
  }
  lastStatus_ = warmedUp() ? SensorStatus::Ok : SensorStatus::Initializing;
  return lastStatus_;
}

bool Ccs811Driver::read(Variable variable, Reading& outReading) {
  outReading = Reading{};

  // The eCO2 value lives in the same two bytes as nothing useful to this node, and the one
  // variable it does publish is VOC. There is no CO2 axis here to ask for.
  if (variable != Variable::Tvoc) {
    outReading.status = SensorStatus::InvalidData;
    return false;
  }

  if (!configured_ && !begin()) return false;

  if (!warmedUp()) {
    // Reporting zero here would be a lie with a unit attached: a reader could not tell an
    // unheated sensor from genuinely clean air. Initializing says what is actually true.
    outReading.status = SensorStatus::Initializing;
    return false;
  }

  uint8_t status = 0;
  if (!readStatus(status)) {
    outReading.status = lastStatus_;
    return false;
  }
  if ((status & kStatusDataReady) == 0) {
    // A 10 s periodic mode will not have fresh data on every call. Asking sooner is not an
    // error worth reporting as a fault, so this is Initializing too.
    outReading.status = SensorStatus::Initializing;
    return false;
  }

  uint16_t tvoc = 0;
  uint16_t eco2 = 0;
  (void)eco2;  // read and discarded on purpose; see the class comment
  if (!readAlgorithmResult(tvoc, eco2)) {
    outReading.status = lastStatus_;
    return false;
  }

  outReading.ok = true;
  outReading.status = SensorStatus::Ok;
  outReading.value = static_cast<float>(tvoc);
  return true;
}

const SensorMetadata& Ccs811Driver::metadata() const { return metadata_; }

}  // namespace cauce::drivers