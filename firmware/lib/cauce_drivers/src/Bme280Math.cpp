#include <cmath>

#include "cauce/drivers/Bme280Math.h"

namespace cauce::drivers {

int32_t bme280CompensateTemperature(const Bme280Trim& trim,
                                    uint32_t adcTemperature, int32_t& tFineOut) {
  const int32_t var1 =
      ((static_cast<int32_t>(adcTemperature >> 3) -
        (static_cast<int32_t>(trim.digT1) << 1)) *
       trim.digT2) >>
      11;
  const int32_t diff = static_cast<int32_t>(adcTemperature >> 4) - trim.digT1;
  const int64_t sq = (static_cast<int64_t>(diff) * diff) >> 12;
  const int32_t var2 = static_cast<int32_t>((sq * trim.digT3) >> 14);
  const int32_t tFine = var1 + var2;
  tFineOut = tFine;
  return (tFine * 5 + 128) >> 8;
}

float bme280TemperatureC(const Bme280Trim& trim, uint32_t adcTemperature) {
  int32_t tFine = 0;
  return static_cast<float>(bme280CompensateTemperature(trim, adcTemperature, tFine)) /
         100.0f;
}

uint32_t bme280CompensatePressure(const Bme280Trim& trim, uint32_t adcPressure,
                                  int32_t tFine) {
  int64_t var1 = static_cast<int64_t>(tFine) - 128000;
  int64_t var2 = var1 * var1 * static_cast<int64_t>(trim.digP6);
  var2 = var2 + ((var1 * static_cast<int64_t>(trim.digP5)) << 17);
  var2 = var2 + (static_cast<int64_t>(trim.digP4) << 35);
  var1 = ((var1 * var1 * static_cast<int64_t>(trim.digP3)) >> 8) +
         ((var1 * static_cast<int64_t>(trim.digP2)) << 12);
  var1 = (((static_cast<int64_t>(1) << 47) + var1)) *
         static_cast<int64_t>(trim.digP1) >>
         33;
  if (var1 == 0) return 0;

  int64_t p = 1048576 - static_cast<int64_t>(adcPressure);
  p = (((p << 31) - var2) * 3125) / var1;
  var1 = (static_cast<int64_t>(trim.digP9) * (p >> 13) * (p >> 13)) >> 25;
  var2 = (static_cast<int64_t>(trim.digP8) * p) >> 19;
  p = ((p + var1 + var2) >> 8) + (static_cast<int64_t>(trim.digP7) << 4);
  return static_cast<uint32_t>(p);
}

float bme280PressurePa(const Bme280Trim& trim, uint32_t adcPressure, int32_t tFine) {
  return static_cast<float>(bme280CompensatePressure(trim, adcPressure, tFine)) /
         256.0f;
}

uint32_t bme280CompensateHumidity(const Bme280Trim& trim, uint32_t adcHumidity,
                                  int32_t tFine) {
  const int64_t h4 = trim.digH4;
  const int64_t h5 = trim.digH5;
  int64_t v1 = tFine - 76800;
  v1 =
      ((((static_cast<int64_t>(adcHumidity) << 14) - (h4 << 20) - (h5 * v1)) +
        16384) >>
       15) *
      ((((((v1 * static_cast<int64_t>(trim.digH6)) >> 10) *
          (((v1 * static_cast<int64_t>(trim.digH3)) >> 11) + 32768))) >>
        10) +
       2097152) *
          static_cast<int64_t>(trim.digH2) +
      8192 >>
      14;
  v1 = v1 -
       (((((v1 >> 15) * (v1 >> 15)) >> 7) * static_cast<int64_t>(trim.digH1)) >> 4);
  if (v1 < 0) v1 = 0;
  if (v1 > 419430400LL) v1 = 419430400LL;
  return static_cast<uint32_t>(v1 >> 12);
}

float bme280HumidityPct(const Bme280Trim& trim, uint32_t adcHumidity, int32_t tFine) {
  return static_cast<float>(bme280CompensateHumidity(trim, adcHumidity, tFine)) /
         1024.0f;
}

double bme280TemperatureCReference(const Bme280Trim& trim,
                                   uint32_t adcTemperature) {
  const double var1 =
      (static_cast<double>(adcTemperature >> 3) -
       static_cast<double>(trim.digT1) * 2.0) *
      static_cast<double>(trim.digT2) / 2048.0;
  const double diff =
      static_cast<double>(adcTemperature >> 4) - static_cast<double>(trim.digT1);
  const double sq = diff * diff / 4096.0;
  const double var2 = sq * static_cast<double>(trim.digT3) / 16384.0;
  return (var1 + var2) / 5120.0;
}

double bme280PressurePaReference(const Bme280Trim& trim, uint32_t adcPressure,
                                 int32_t tFine) {
  double v1 = static_cast<double>(tFine) - 128000.0;
  double v2 = v1 * v1 * static_cast<double>(trim.digP6);
  v2 += v1 * static_cast<double>(trim.digP5) * 131072.0;
  v2 += static_cast<double>(trim.digP4) * 34359738368.0;
  v1 = (v1 * v1 * static_cast<double>(trim.digP3)) / 256.0 +
       v1 * static_cast<double>(trim.digP2) * 4096.0;
  v1 = (140737488355328.0 + v1) * static_cast<double>(trim.digP1) /
       8589934592.0;
  if (v1 == 0.0) return 0.0;
  double p = 1048576.0 - static_cast<double>(adcPressure);
  p = (p * 2147483648.0 - v2) * 3125.0 / v1;
  const double p13 = std::floor(p / 8192.0);
  v1 = static_cast<double>(trim.digP9) * p13 * p13 / 33554432.0;
  v2 = static_cast<double>(trim.digP8) * p / 524288.0;
  p = ((p + v1 + v2) / 256.0 + static_cast<double>(trim.digP7) * 16.0) / 256.0;
  return p;
}

double bme280HumidityPctReference(const Bme280Trim& trim, uint32_t adcHumidity,
                                  int32_t tFine) {
  const double h64 = static_cast<double>(tFine) - 76800.0;
  const double aNum = static_cast<double>(static_cast<int64_t>(adcHumidity) << 14) -
                      static_cast<double>(trim.digH4) * 1048576.0 -
                      static_cast<double>(trim.digH5) * h64;
  const double a = (aNum + 16384.0) / 32768.0;
  const double b1 = std::floor(h64 * static_cast<double>(trim.digH6) / 1024.0);
  const double b2 =
      std::floor(h64 * static_cast<double>(trim.digH3) / 2048.0) + 32768.0;
  const double b3 = std::floor(b1 * b2 / 1024.0);
  const double c = (b3 + 2097152.0) * static_cast<double>(trim.digH2) + 8192.0;
  double v1 = a * c / 16384.0;
  const double q = std::floor(v1 / 32768.0);
  v1 -= (q * q / 128.0) * static_cast<double>(trim.digH1) / 16.0;
  if (v1 < 0.0) v1 = 0.0;
  if (v1 > 419430400.0) v1 = 419430400.0;
  return v1 / 4194304.0;
}

}  // namespace cauce::drivers

