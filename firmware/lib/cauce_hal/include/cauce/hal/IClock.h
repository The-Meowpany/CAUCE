#pragma once

#include <cstddef>
#include <cstdint>

namespace cauce::hal {

class IClock {
 public:
  virtual ~IClock() = default;
  virtual uint32_t monotonicMs() const = 0;
  virtual uint64_t utcMs() const = 0;
  virtual bool utcTimeValid() const = 0;
  virtual void setUtcMs(uint64_t epochMs) = 0;
};

}  // namespace cauce::hal
