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
  WindSpeed = 5,
  Tvoc = 6,
  // Pilot station suite. Appended rather than inserted: the numeric value travels in the
  // 68-byte frame and the compact LoRa record, so renumbering would reinterpret stored data.
  Co2 = 7,
  Pm1 = 8,
  Pm25 = 9,
  Pm4 = 10,
  Pm10 = 11,
  UvIndex = 12,
  UvaIndex = 13,
  UvbIndex = 14,
  SolarIrradiance = 15,
  WindDirection = 16,
  Rainfall = 17,
  SoilMoisture = 18,
  SoilWaterPotential = 19,
  SoilTemperature = 20,
  LeafWetness = 21,
  LightningStrikes = 22,
  LightningDistance = 23,
  Unknown = 255,
};

static constexpr uint8_t kVariableCount = 24;

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
