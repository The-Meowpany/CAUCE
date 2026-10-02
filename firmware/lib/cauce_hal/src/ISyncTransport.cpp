#include "cauce/hal/ISyncTransport.h"

#include <cstring>

namespace cauce::hal {

bool CommandBatch::add(uint32_t id, const char* kindText,
                       const char* payloadText) {
  if (count >= kMaxCommands) return false;
  if (kindText == nullptr) return false;
  size_t i = 0;
  for (; kindText[i] != '\0' && i + 1 < kMaxKind; ++i) kind[count][i] = kindText[i];
  kind[count][i] = '\0';
  commandId[count] = id;
  const char* source = payloadText == nullptr ? "" : payloadText;
  size_t j = 0;
  for (; source[j] != '\0' && j + 1 < kMaxPayload; ++j)
    payload[count][j] = source[j];
  payload[count][j] = '\0';
  ++count;
  return true;
}

}  // namespace cauce::hal