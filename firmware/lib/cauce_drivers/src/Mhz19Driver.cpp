#include "cauce/drivers/Mhz19Driver.h"

#include "cauce/core/Types.h"

namespace cauce::drivers {

namespace {

constexpr uint8_t kStart = 0xFF;
constexpr uint8_t kSensorNumber = 0x01;
constexpr uint8_t kReadCo2 = 0x86;
constexpr uint8_t kSetAbc = 0x79;

}  // namespace

uint8_t Mhz19Driver::checksum(const uint8_t* frame) {
  uint8_t sum = 0;
  for (int i = 1; i < 8; ++i) sum = static_cast<uint8_t>(sum + frame[i]);
  return static_cast<uint8_t>((0xFF - sum) + 1);
}

Mhz19Driver::Mhz19Driver(hal::IUart& uart, hal::IClock& clock, const char* id)
    : uart_(uart), clock_(clock) {
  copyString(metadata_.id, sizeof(metadata_.id), id);
  copyString(metadata_.model, sizeof(metadata_.model), "MH-Z19C");
  metadata_.variables[0] = Variable::Co2;
  metadata_.variableCount = 1;
  // Datasheet: +/-(50 ppm + 5 % of reading).
  metadata_.accuracyNote = 50.0f;
  metadata_.resolution = 1.0f;
}

bool Mhz19Driver::transact(uint8_t command, const uint8_t* args, uint8_t reply[9],
                           bool expectReply) {
  uint8_t frame[9] = {kStart, kSensorNumber, command, 0, 0, 0, 0, 0, 0};
  if (args != nullptr) {
    for (int i = 0; i < 5; ++i) frame[3 + i] = args[i];
  }
  frame[8] = checksum(frame);
  uart_.discardInput();
  if (uart_.write(frame, sizeof(frame)) != sizeof(frame)) return false;
  if (!expectReply) return true;
  if (uart_.read(reply, 9, kReplyTimeoutMs) != 9) return false;
  return reply[0] == kStart && reply[1] == command && reply[8] == checksum(reply);
}

bool Mhz19Driver::begin() {
  if (!uart_.begin(kBaud)) {
    lastStatus_ = SensorStatus::BusError;
    return false;
  }
  uint8_t reply[9];
  const uint8_t abcOff[5] = {0x00, 0, 0, 0, 0};
  // The ABC command has no reply on the C revision, so its success is not checked here; the
  // first CO2 read below is what proves the sensor is on the line.
  transact(kSetAbc, abcOff, reply, false);
  if (!transact(kReadCo2, nullptr, reply, true)) {
    lastStatus_ = SensorStatus::NotConnected;
    return false;
  }
  started_ = true;
  startedAtMs_ = clock_.monotonicMs();
  lastStatus_ = SensorStatus::Initializing;
  return true;
}

SensorStatus Mhz19Driver::health() {
  if (!started_) return lastStatus_;
  if (clock_.monotonicMs() - startedAtMs_ < kPreheatMs) {
    lastStatus_ = SensorStatus::Initializing;
  } else if (lastStatus_ == SensorStatus::Initializing) {
    lastStatus_ = SensorStatus::Ok;
  }
  return lastStatus_;
}

bool Mhz19Driver::read(Variable variable, Reading& outReading) {
  outReading = Reading{};
  if (variable != Variable::Co2) {
    outReading.status = SensorStatus::InvalidData;
    return false;
  }
  if (!started_ && !begin()) {
    outReading.status = lastStatus_;
    return false;
  }
  if (clock_.monotonicMs() - startedAtMs_ < kPreheatMs) {
    outReading.status = SensorStatus::Initializing;
    return false;
  }
  uint8_t reply[9];
  if (!transact(kReadCo2, nullptr, reply, true)) {
    lastStatus_ = SensorStatus::BusError;
    outReading.status = lastStatus_;
    return false;
  }
  lastStatus_ = SensorStatus::Ok;
  outReading.ok = true;
  outReading.status = SensorStatus::Ok;
  outReading.value = static_cast<float>((static_cast<uint16_t>(reply[2]) << 8) | reply[3]);
  return true;
}

const SensorMetadata& Mhz19Driver::metadata() const { return metadata_; }

}  // namespace cauce::drivers
