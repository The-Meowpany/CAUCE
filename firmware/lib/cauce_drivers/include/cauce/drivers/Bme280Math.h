#pragma once

#include <cstdint>

namespace cauce::drivers {

struct Bme280Trim {
  uint16_t digT1{0};
  int16_t digT2{0};
  int16_t digT3{0};
  uint16_t digP1{0};
  int16_t digP2{0};
  int16_t digP3{0};
  int16_t digP4{0};
  int16_t digP5{0};
  int16_t digP6{0};
  int16_t digP7{0};
  int16_t digP8{0};
  int16_t digP9{0};
  uint8_t digH1{0};
  int16_t digH2{0};
  uint8_t digH3{0};
  int16_t digH4{0};
  int16_t digH5{0};
  int8_t digH6{0};
};

struct Bme280Raw {
  uint32_t pressure{0};
  uint32_t temperature{0};
  uint32_t humidity{0};
};

int32_t bme280CompensateTemperature(const Bme280Trim& trim, uint32_t adcTemperature,
                                    int32_t& tFineOut);
float bme280TemperatureC(const Bme280Trim& trim, uint32_t adcTemperature);
uint32_t bme280CompensatePressure(const Bme280Trim& trim, uint32_t adcPressure,
                                  int32_t tFine);
float bme280PressurePa(const Bme280Trim& trim, uint32_t adcPressure,
                       int32_t tFine);
uint32_t bme280CompensateHumidity(const Bme280Trim& trim, uint32_t adcHumidity,
                                  int32_t tFine);
float bme280HumidityPct(const Bme280Trim& trim, uint32_t adcHumidity,
                        int32_t tFine);

double bme280TemperatureCReference(const Bme280Trim& trim,
                                   uint32_t adcTemperature);
double bme280PressurePaReference(const Bme280Trim& trim, uint32_t adcPressure,
                                 int32_t tFine);
double bme280HumidityPctReference(const Bme280Trim& trim, uint32_t adcHumidity,
                                  int32_t tFine);

}  // namespace cauce::drivers
