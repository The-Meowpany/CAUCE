#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <vector>

#include "cauce/hal/IPeripherals.h"

namespace cauce::hal {

// Test doubles for the peripherals in IPeripherals.h. Header-only and host-only in practice;
// they hold whatever the test scripted and record what the driver sent.

class ScriptedUart final : public IUart {
 public:
  bool begin(uint32_t baud) override {
    baud_ = baud;
    return true;
  }
  size_t write(const uint8_t* data, size_t length) override {
    written.insert(written.end(), data, data + length);
    if (respond_ && !replies.empty()) {
      const std::vector<uint8_t> reply = replies.front();
      replies.pop_front();
      pending_.insert(pending_.end(), reply.begin(), reply.end());
    }
    return length;
  }
  size_t read(uint8_t* buffer, size_t length, uint32_t timeoutMs) override {
    (void)timeoutMs;
    size_t n = 0;
    while (n < length && !pending_.empty()) {
      buffer[n++] = pending_.front();
      pending_.pop_front();
    }
    return n;
  }
  void discardInput() override {
    discarded += pending_.size();
    pending_.clear();
  }

  // Bytes that are already waiting before the next request, as a stale reply would be.
  void preload(const std::vector<uint8_t>& bytes) {
    pending_.insert(pending_.end(), bytes.begin(), bytes.end());
  }
  void setResponding(bool respond) { respond_ = respond; }
  uint32_t baud() const { return baud_; }

  std::vector<uint8_t> written;
  std::deque<std::vector<uint8_t>> replies;
  size_t discarded{0};

 private:
  std::deque<uint8_t> pending_;
  uint32_t baud_{0};
  bool respond_{true};
};

class ScriptedAnalogInput final : public IAnalogInput {
 public:
  bool readMillivolts(uint8_t channel, float& outMillivolts) override {
    auto it = values.find(channel);
    if (it == values.end()) return false;
    outMillivolts = it->second;
    return true;
  }
  std::map<uint8_t, float> values;
};

// Plays back a byte sequence for readByte() and records every written byte.
class ScriptedOneWire final : public IOneWire {
 public:
  bool reset() override {
    ++resets;
    return present;
  }
  void writeByte(uint8_t value) override { written.push_back(value); }
  uint8_t readByte() override {
    if (toRead.empty()) return 0xFF;  // an idle bus reads as ones
    const uint8_t b = toRead.front();
    toRead.pop_front();
    return b;
  }

  bool present{true};
  int resets{0};
  std::vector<uint8_t> written;
  std::deque<uint8_t> toRead;
};

}  // namespace cauce::hal
