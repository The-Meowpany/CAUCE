#include "cauce/hal/ScriptedI2c.h"

namespace cauce::hal {

ScriptedI2cBus& ScriptedI2cBus::attach(uint8_t address, ScriptedI2cDevice* device) {
  devices_[address] = device;
  return *this;
}

void ScriptedI2cBus::detach(uint8_t address) { devices_.erase(address); }

bool ScriptedI2cBus::begin() { return true; }

bool ScriptedI2cBus::writeRegisters(uint8_t deviceAddress, uint8_t startRegister,
                                    const uint8_t* data, size_t length) {
  auto it = devices_.find(deviceAddress);
  if (it == devices_.end()) return false;
  it->second->writeRegister(startRegister, data, length);
  return true;
}

bool ScriptedI2cBus::readRegisters(uint8_t deviceAddress, uint8_t startRegister,
                                   uint8_t* buffer, size_t length) {
  auto it = devices_.find(deviceAddress);
  if (it == devices_.end()) return false;
  return it->second->readRegister(startRegister, buffer, length);
}

bool ScriptedI2cBus::isPresent(uint8_t deviceAddress) {
  return devices_.count(deviceAddress) > 0;
}

void RegisterFileDevice::writeRegister(uint8_t reg, const uint8_t* data,
                                       size_t length) {
  for (size_t i = 0; i < length; ++i) {
    uint8_t target = static_cast<uint8_t>(reg + i);
    if (target < kMaxRegisters) registers_[target] = data[i];
  }
}

bool RegisterFileDevice::readRegister(uint8_t reg, uint8_t* buffer, size_t length) {
  for (size_t i = 0; i < length; ++i) {
    uint8_t source = static_cast<uint8_t>(reg + i);
    if (source >= kMaxRegisters) return false;
    buffer[i] = registers_[source];
  }
  return true;
}

}  // namespace cauce::hal
