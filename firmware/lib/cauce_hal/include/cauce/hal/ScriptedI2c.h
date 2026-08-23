#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "cauce/hal/IFileSystem.h"
#include "cauce/hal/II2cBus.h"

namespace cauce::hal {

class ScriptedI2cDevice;

class ScriptedI2cBus final : public II2cBus {
 public:
  ScriptedI2cBus& attach(uint8_t address, ScriptedI2cDevice* device);
  void detach(uint8_t address);

  bool begin() override;
  bool writeRegisters(uint8_t deviceAddress, uint8_t startRegister,
                      const uint8_t* data, size_t length) override;
  bool readRegisters(uint8_t deviceAddress, uint8_t startRegister,
                     uint8_t* buffer, size_t length) override;
  bool isPresent(uint8_t deviceAddress) override;

 private:
  std::map<uint8_t, ScriptedI2cDevice*> devices_;
};

class ScriptedI2cDevice {
 public:
  virtual ~ScriptedI2cDevice() = default;
  virtual void writeRegister(uint8_t reg, const uint8_t* data, size_t length) = 0;
  virtual bool readRegister(uint8_t reg, uint8_t* buffer, size_t length) = 0;
};

class RegisterFileDevice final : public ScriptedI2cDevice {
 public:
  static constexpr size_t kMaxRegisters = 256;

  void setRegister(uint8_t reg, uint8_t value) { registers_[reg] = value; }
  void setRegistersFromBytes(uint8_t startReg, const uint8_t* bytes, size_t length) {
    for (size_t i = 0; i < length; ++i) registers_[static_cast<uint8_t>(startReg + i)] = bytes[i];
  }
  uint8_t getRegister(uint8_t reg) const { return registers_[reg]; }

  void writeRegister(uint8_t reg, const uint8_t* data, size_t length) override;
  bool readRegister(uint8_t reg, uint8_t* buffer, size_t length) override;

 private:
  uint8_t registers_[kMaxRegisters] = {};
};

}  // namespace cauce::hal
