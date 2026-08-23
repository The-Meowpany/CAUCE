#pragma once

#include <cstddef>
#include <cstdint>

namespace cauce::hal {

class II2cBus {
 public:
  virtual ~II2cBus() = default;
  virtual bool begin() = 0;
  virtual bool writeRegisters(uint8_t deviceAddress, uint8_t startRegister,
                              const uint8_t* data, size_t length) = 0;
  virtual bool readRegisters(uint8_t deviceAddress, uint8_t startRegister,
                             uint8_t* buffer, size_t length) = 0;
  virtual bool isPresent(uint8_t deviceAddress) = 0;
};

}  // namespace cauce::hal
