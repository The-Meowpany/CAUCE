#include "cauce/app/OtaRollback.h"

namespace cauce::app {

BootVerdict OtaRollbackGuard::evaluate(uint32_t attempt) const {
  if (!control_.isPendingVerify()) return BootVerdict::NotPending;
  if (control_.selfTestPassed()) return BootVerdict::MarkValid;
  if (attempt + 1 >= maxAttempts_) return BootVerdict::Rollback;
  return BootVerdict::StayPending;
}

bool OtaRollbackGuard::apply(BootVerdict verdict) const {
  switch (verdict) {
    case BootVerdict::MarkValid:
      return control_.markAppValid();
    case BootVerdict::Rollback:
      return control_.requestRollback();
    case BootVerdict::NotPending:
    case BootVerdict::StayPending:
      return true;
  }
  return false;
}

}  // namespace cauce::app
