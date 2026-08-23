#pragma once

#include <cstdint>
#include <cstring>

namespace cauce {

enum class Variable : uint8_t {
  AirTemperature = 0,
  RelativeHumidity = 1,
  Pressure = 2,
  Light = 3,
  BatteryVoltage = 4,
  Unknown = 255,
};

inline constexpr uint8_t kVariableCount = 5;

enum class Quality : uint8_t {
  Valid = 0,
  Calibrated = 1,
  Uncalibrated = 2,
  Estimated = 3,
  Suspect = 4,
  Invalid = 5,
  Missing = 6,
};

struct Versions {
  static constexpr const char* kFirmware = "0.1.0";
  static constexpr const char* kHardwareRevision = "rev-a";
  static constexpr uint8_t kProtocol = 1;
  static constexpr uint8_t kConfigSchema = 1;
};

const char* variableName(Variable v);
const char* variableUnit(Variable v);
Variable parseVariable(const char* name);
const char* qualityName(Quality q);

inline void copyString(char* dest, size_t capacity, const char* source) {
  if (!dest || capacity == 0) return;
  if (!source) {
    dest[0] = '\0';
    return;
  }
  size_t i = 0;
  for (; i + 1 < capacity && source[i] != '\0'; ++i) dest[i] = source[i];
  dest[i] = '\0';
}

}  // namespace cauce
