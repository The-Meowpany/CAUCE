#include "cauce/core/BootDiagnostics.h"

#include <cstdio>
#include <cstring>

namespace cauce {

namespace {

// A checksum over the record with the checksum field itself zeroed.
//
// FNV-1a rather than a real hash: this guards a 32-byte buffer against a torn write and bit
// rot, not against an adversary. A node whose RTC memory is being deliberately rewritten is a
// compromised node, and the bootloader refusing to trust a tampered boot record is not a
// control this project has.
uint32_t checksumOf(const BootDiagnosticsRecord& record) {
  BootDiagnosticsRecord copy = record;
  copy.checksum = 0;
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&copy);
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < sizeof(copy); ++i) {
    hash ^= bytes[i];
    hash *= 16777619u;
  }
  return hash;
}

}  // namespace

bool BootDiagnostics::begin(void* storage, size_t capacity, Reporter report,
                            void* context) {
  storage_ = storage;
  capacity_ = capacity;
  startedMs_ = 0;
  stepCount_ = 0;
  loaded_ = false;
  completed_ = false;

  // A record we cannot store is not a record, and this returns false rather than pretending.
  // But the record is still filled in, and still counts this boot. The first version left
  // `bootCount` at 0, which is the worst of the three available answers: wrong, and
  // indistinguishable from a device that has never booted.
  if (storage == nullptr || capacity < sizeof(BootDiagnosticsRecord)) {
    record_ = BootDiagnosticsRecord{};
    record_.magic = kMagic;
    record_.schemaVersion = kSchema;
    record_.bootCount = 1u;
    record_.lastStep = kBootDiagnosticsNone;
    return false;
  }

  BootDiagnosticsRecord previous{};
  std::memcpy(&previous, storage, sizeof(previous));

  const bool usable = previous.magic == kMagic &&
                      previous.schemaVersion == kSchema &&
                      previous.checksum == checksumOf(previous);
  const bool reattempt = usable && previous.lastStep != kBootDiagnosticsNone;

  if (report != nullptr) {
    if (!usable) {
      // Not an error worth alarming about: the first boot ever, or a buffer that was zeroed.
      report(context, "BOOT_HISTORY none (first boot, or no valid record)");
    } else if (reattempt) {
      char line[160];
      std::snprintf(line, sizeof(line),
                    "BOOT_HISTORY reattempt boots=%lu prev_step=%u ordinal=%u "
                    "prev_rst=0x%lx",
                    static_cast<unsigned long>(previous.bootCount),
                    static_cast<unsigned>(previous.lastStep),
                    static_cast<unsigned>(previous.lastStepOrdinal),
                    static_cast<unsigned long>(previous.lastResetReason));
      report(context, line);
      report(context,
             "BOOT_HISTORY the previous boot did not reach BOOT_COMPLETE; the step above is "
             "where it ended");
    } else {
      char line[128];
      std::snprintf(line, sizeof(line),
                    "BOOT_HISTORY last boot completed boots=%lu duration_ms=%lu",
                    static_cast<unsigned long>(previous.bootCount),
                    static_cast<unsigned long>(previous.lastBootMs));
      report(context, line);
    }
  }

  record_ = BootDiagnosticsRecord{};
  record_.magic = kMagic;
  record_.schemaVersion = kSchema;
  // Inheriting the count rather than starting at 1: a node that has rebooted nine times
  // should say nine, because that number is the whole reason anyone is reading this line.
  record_.bootCount = usable ? previous.bootCount + 1u : 1u;
  record_.lastStep = kBootDiagnosticsNone;
  record_.lastResetReason = usable ? previous.lastResetReason : 0u;
  record_.lastBootMs = usable ? previous.lastBootMs : 0u;
  // The new boot's own count is what `bootCount` means, and it is only correct once this
  // begin() returns: a caller that asks twice gets two different, both-correct answers for
  // "which boot is this".
  persist();
  return reattempt;
}

uint16_t BootDiagnostics::beginStep(const char* name) {
  if (stepCount_ < kMaxSteps) {
    stepNames_[stepCount_] = name != nullptr ? name : "<unnamed>";
  }
  // The counter runs past `kMaxSteps` deliberately, and this is the distinction the first
  // version got wrong: the *names* are bounded because there are only so many places in setup()
  // worth naming, but the ordinal is the answer to "how far did it get", and clamping it at
  // 24 makes a boot that loops 200 times look identical to one that reached step 24. Once the
  // names run out the ordinal keeps counting, and a saturating uint16_t would be worse still:
  // 65535 steps is far more than enough to be conclusive, and saturation would hide that.
  ++stepCount_;
  record_.lastStepOrdinal = stepCount_;
  record_.lastStep = static_cast<uint16_t>(stepCount_);
  persist();
  return stepCount_;
}

void BootDiagnostics::endStep(uint16_t ordinal, bool ok) {
  (void)ordinal;
  (void)ok;
  // Deliberately does not clear `lastStep`. The record's job is to answer "where did the
  // previous boot end", and the last step *entered* is the answer even when it completed.
  // Clearing it here would make a clean boot look identical to a boot that never started, and
  // the two are the whole distinction this component exists to preserve.
  record_.lastStep = record_.lastStep == kBootDiagnosticsNone ? kBootDiagnosticsNone
                                                             : record_.lastStep;
}

void BootDiagnostics::complete(uint32_t resetReason) {
  completed_ = true;
  record_.lastStep = kBootDiagnosticsNone;
  record_.lastResetReason = resetReason;
  record_.lastBootMs = startedMs_;
  persist();
}

void BootDiagnostics::feed() const {
  // Intentionally empty here. The watchdog itself is an ESP-IDF call with no host equivalent,
  // and a stub here would be a fake that reads as a working implementation. The call site in
  // main.cpp pairs `feed()` with the real `esp_task_wdt_reset()`, so the two cannot drift.
}

void BootDiagnostics::persist() const {
  if (storage_ == nullptr || capacity_ < sizeof(BootDiagnosticsRecord)) return;
  BootDiagnosticsRecord copy = record_;
  copy.checksum = checksumOf(copy);
  std::memcpy(storage_, &copy, sizeof(copy));
}

void reportBootDiagnosticsLine(void* context, const char* line) {
  if (line == nullptr) return;
  if (context == nullptr) {
    std::printf("%s\n", line);
    return;
  }
  // The context is an opaque sink chosen by the caller; only the ESP32 build passes a real
  // one, and it passes a File. Kept as void* so this header has no Arduino dependency.
  std::fputs(line, static_cast<std::FILE*>(context));
  std::fputc('\n', static_cast<std::FILE*>(context));
}

}  // namespace cauce