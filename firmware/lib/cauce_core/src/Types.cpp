#include "cauce/core/Types.h"

namespace cauce {

const char* variableName(Variable v) {
  switch (v) {
    case Variable::AirTemperature:
      return "air_temperature";
    case Variable::RelativeHumidity:
      return "relative_humidity";
    case Variable::Pressure:
      return "pressure";
    case Variable::Light:
      return "illuminance";
    case Variable::BatteryVoltage:
      return "battery_voltage";
    default:
      return "unknown";
  }
}

const char* variableUnit(Variable v) {
  switch (v) {
    case Variable::AirTemperature:
      return "C";
    case Variable::RelativeHumidity:
      return "%RH";
    case Variable::Pressure:
      return "hPa";
    case Variable::Light:
      return "lx";
    case Variable::BatteryVoltage:
      return "V";
    default:
      return "?";
  }
}

Variable parseVariable(const char* name) {
  if (!name) return Variable::Unknown;
  struct Entry {
    const char* name;
    Variable var;
  };
  static constexpr Entry kEntries[] = {
      {"air_temperature", Variable::AirTemperature},
      {"relative_humidity", Variable::RelativeHumidity},
      {"pressure", Variable::Pressure},
      {"illuminance", Variable::Light},
      {"battery_voltage", Variable::BatteryVoltage},
  };
  for (const auto& e : kEntries) {
    if (strcmp(name, e.name) == 0) return e.var;
  }
  return Variable::Unknown;
}

const char* qualityName(Quality q) {
  switch (q) {
    case Quality::Valid:
      return "VALID";
    case Quality::Calibrated:
      return "CALIBRATED";
    case Quality::Uncalibrated:
      return "UNCALIBRATED";
    case Quality::Estimated:
      return "ESTIMATED";
    case Quality::Suspect:
      return "SUSPECT";
    case Quality::Invalid:
      return "INVALID";
    case Quality::Missing:
      return "MISSING";
    default:
      return "UNKNOWN";
  }
}

}  // namespace cauce
