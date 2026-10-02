#include "cauce/core/TextBuffer.h"

#include <cstdio>
#include <cstring>

namespace cauce {

namespace {

// One place that turns "would have written" into "did write". Every caller
// goes through this, so `used` can never exceed `capacity`.
size_t commit(TextBuffer& buffer, int wouldWrite) {
  if (buffer.data == nullptr || buffer.capacity == 0) {
    buffer.truncated = true;
    return 0;
  }
  const size_t space = buffer.remaining();
  if (space == 0) {
    buffer.truncated = true;
    return 0;
  }
  size_t written = static_cast<size_t>(wouldWrite);
  if (written + 1 > space) {
    // Truncated: the terminator still has to fit inside the buffer.
    written = space - 1;
    buffer.truncated = true;
  }
  buffer.used += written;
  buffer.data[buffer.used] = '\0';
  return written;
}

}  // namespace

size_t appendText(TextBuffer& buffer, const char* format, ...) {
  if (buffer.data == nullptr || buffer.capacity == 0) {
    buffer.truncated = true;
    return 0;
  }
  if (buffer.remaining() == 0) {
    buffer.truncated = true;
    return 0;
  }

  va_list args;
  va_start(args, format);
  const int wouldWrite =
      std::vsnprintf(buffer.data + buffer.used, buffer.remaining(), format, args);
  va_end(args);

  if (wouldWrite < 0) {
    buffer.truncated = true;
    return 0;
  }
  return commit(buffer, wouldWrite);
}

size_t appendRaw(TextBuffer& buffer, const char* text) {
  if (text == nullptr) return 0;
  const size_t length = std::strlen(text);
  if (length == 0) return 0;
  if (buffer.data == nullptr || buffer.capacity == 0) {
    buffer.truncated = true;
    return 0;
  }
  const size_t space = buffer.remaining();
  if (space == 0) {
    buffer.truncated = true;
    return 0;
  }
  size_t written = length;
  if (written + 1 > space) {
    written = space - 1;
    buffer.truncated = true;
  }
  std::memcpy(buffer.data + buffer.used, text, written);
  buffer.used += written;
  buffer.data[buffer.used] = '\0';
  return written;
}

}  // namespace cauce