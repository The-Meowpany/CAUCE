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

  // Plain transfers with no register byte. Some parts (the SPS30 among them) address their
  // commands with a 16-bit pointer, which the register calls above cannot express. Not pure
  // virtual so a bus that never meets such a part does not have to pretend it can.
  virtual bool writeBytes(uint8_t deviceAddress, const uint8_t* data, size_t length) {
    (void)deviceAddress;
    (void)data;
    (void)length;
    return false;
  }
  virtual bool readBytes(uint8_t deviceAddress, uint8_t* buffer, size_t length) {
    (void)deviceAddress;
    (void)buffer;
    (void)length;
    return false;
  }
};

}  // namespace cauce::hal
