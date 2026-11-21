#include "cauce/core/Types.h"

namespace cauce {

namespace {

struct VariableEntry {
  Variable var;
  const char* name;
  const char* unit;
};

// One table instead of three switches, so a variable cannot gain a name and forget its unit.
// Units stay ASCII: they are written into CSV exports and HTTP bodies that a serial console
// also has to show.
constexpr VariableEntry kVariables[] = {
    {Variable::AirTemperature, "air_temperature", "C"},
    {Variable::RelativeHumidity, "relative_humidity", "%RH"},
    {Variable::Pressure, "pressure", "hPa"},
    {Variable::Light, "illuminance", "lx"},
    {Variable::BatteryVoltage, "battery_voltage", "V"},
    {Variable::WindSpeed, "wind_speed", "m/s"},
    {Variable::Tvoc, "tvoc", "ppb"},
    {Variable::Co2, "co2", "ppm"},
    {Variable::Pm1, "pm1", "ug/m3"},
    {Variable::Pm25, "pm2_5", "ug/m3"},
    {Variable::Pm4, "pm4", "ug/m3"},
    {Variable::Pm10, "pm10", "ug/m3"},
    {Variable::UvIndex, "uv_index", "UVI"},
    {Variable::UvaIndex, "uva_index", "UVI"},
    {Variable::UvbIndex, "uvb_index", "UVI"},
    {Variable::SolarIrradiance, "solar_irradiance", "W/m2"},
    {Variable::WindDirection, "wind_direction", "deg"},
    {Variable::Rainfall, "rainfall", "mm"},
    {Variable::SoilMoisture, "soil_moisture", "%"},
    {Variable::SoilWaterPotential, "soil_water_potential", "kPa"},
    {Variable::SoilTemperature, "soil_temperature", "C"},
    {Variable::LeafWetness, "leaf_wetness", "%"},
    {Variable::LightningStrikes, "lightning_strikes", "count"},
    {Variable::LightningDistance, "lightning_distance", "km"},
};

static_assert(sizeof(kVariables) / sizeof(kVariables[0]) == kVariableCount,
              "every variable needs a name and a unit");

const VariableEntry* find(Variable v) {
  for (const auto& e : kVariables) {
    if (e.var == v) return &e;
  }
  return nullptr;
}

}  // namespace

const char* variableName(Variable v) {
  const VariableEntry* e = find(v);
  return e ? e->name : "unknown";
}

const char* variableUnit(Variable v) {
  const VariableEntry* e = find(v);
  return e ? e->unit : "?";
}

Variable parseVariable(const char* name) {
  if (!name) return Variable::Unknown;
  for (const auto& e : kVariables) {
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
