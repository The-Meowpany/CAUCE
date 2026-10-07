// Making a boot loop legible, which is the same as making it fixable.
//
// THE PROBLEM
//
// A board that reboots in a loop leaves no evidence. The serial log is gone with the previous
// boot, so what survives is whatever the bootloader printed before the panic handler: a reset
// reason and nothing else. "rst:0x8 TG1WDT_SYS_RESET" says a watchdog fired. It does not say
// which step was running, and by the time anyone attaches a monitor the answer has scrolled
// past six identical boots.
//
// This component keeps the answer across the reset. The last step reached is written to a byte
// buffer the caller owns - RTC memory on a real board, which survives a watchdog reset and not
// a power cycle - together with a boot counter and a flag saying whether the previous boot
// ever reached its end. On the next boot, before anything else happens, it reports what the
// previous one was doing.
//
// WHY IT IS IN `cauce_core` AND NOT IN main.cpp
//
// Because the interesting part is not the printing. It is the rule that decides when a boot is
// considered complete, and that rule is exactly the sort of thing that is wrong once and never
// noticed, because the symptom - a board that reboots - looks the same whether the completion
// flag is correct or not. Here it is a host test.

#pragma once

#include <cstddef>
#include <cstdint>

namespace cauce {

// The record kept across a reset.
//
// Fixed size and trivially copyable: the caller memcpy's this into whatever survives a reset,
// and no field may contain a pointer, because a pointer into the previous boot's heap is
// exactly the kind of thing that reads as plausible garbage rather than as corruption.
struct BootDiagnosticsRecord {
  // A cheap sentinel. A buffer full of zeroes is not a record; this is what distinguishes
  // "no previous boot" from "a previous boot that reached step 0".
  uint32_t magic;
  uint16_t schemaVersion;
  uint16_t reserved;
  uint32_t bootCount;
  // Which step the previous boot was inside when it ended, or `kStepNone` if it finished.
  uint16_t lastStep;
  uint16_t lastStepOrdinal;
  // Milliseconds from the previous boot's start to its end. Zero when it did not complete,
  // because "how long did it last" is meaningless for a boot that was reset.
  uint32_t lastBootMs;
  // The reset reason the caller observed, forwarded so the record carries it even if the next
  // boot's own reading is inconclusive.
  uint32_t lastResetReason;
  uint32_t checksum;
};

enum : uint16_t { kBootDiagnosticsNone = 0xFFFF };

class BootDiagnostics {
 public:
  static constexpr uint16_t kSchema = 1;
  static constexpr uint32_t kMagic = 0x424F4F54;  // "BOOT"

  // Reads any previous record out of `storage` and reports it through `report`.
  //
  // `report` is a callback rather than a `Serial&` so the host suite can capture the text, and
  // so this component has no dependency on anything but `cstdio`.
  using Reporter = void (*)(void* context, const char* line);

  // Returns true when a previous boot is known and it did not complete - that is, when this
  // boot is a re-attempt. False on the first boot, and false when the previous boot finished.
  bool begin(void* storage, size_t capacity, Reporter report, void* context);

  // Records that a named step has started. Returns the ordinal assigned to it, which is what
  // `end` needs, and which is more useful than the name for spotting "always dies in the third
  // thing" across boots.
  uint16_t beginStep(const char* name);

  // Records that the step ended, successfully or not. `ok` false means the step returned
  // early, which is different from never returning at all and worth distinguishing.
  void endStep(uint16_t ordinal, bool ok);

  // Records that setup reached its end. After this the previous boot counts as complete, so
  // the *next* boot will not report it as a re-attempt.
  void complete(uint32_t resetReason);

  // Feeds a watchdog. Thin wrapper so every feed goes through one place and the record is
  // kept in step with the log; a caller that feeds without recording produces a log that
  // claims progress the record does not have.
  void feed() const;

  // The live record, for a caller that wants to inspect it.
  const BootDiagnosticsRecord& record() const { return record_; }

  // The names of the steps seen this boot, for the report. Bounded: a boot that enters an
  // unbounded loop of steps would otherwise grow this without limit.
  static constexpr size_t kMaxSteps = 24;

 private:
  void persist() const;

  void* storage_{nullptr};
  size_t capacity_{0};
  uint32_t startedMs_{0};
  bool loaded_{false};
  bool completed_{false};
  uint16_t stepCount_{0};
  const char* stepNames_[kMaxSteps]{};
  BootDiagnosticsRecord record_{};
};

// Over the serial log on a board, and to stdout on the host. Kept out of the class so the
// class has no platform dependency.
void reportBootDiagnosticsLine(void* context, const char* line);

}  // namespace cauce