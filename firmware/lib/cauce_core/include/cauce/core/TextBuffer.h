#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdint>

namespace cauce {

// Bounded text building.
//
// `snprintf` returns how many bytes it *would* have written, so accumulating
// its return value and then subtracting from the remaining capacity looks safe
// and is not: once the counter passes the buffer size, `capacity - used`
// underflows on an unsigned type and the next call is handed an enormous limit.
// Nothing checks it, so the code stays safe only for as long as every appended
// string happens to be short enough -- an invariant nothing states or tests.
//
// These helpers never let the running total exceed the capacity, so a caller
// that appends an unexpectedly long string gets a truncated result instead of
// a stack overflow. Truncation is reported rather than silent.
struct TextBuffer {
  char* data{nullptr};
  size_t capacity{0};
  size_t used{0};
  bool truncated{false};

  TextBuffer() = default;
  TextBuffer(char* buffer, size_t bufferCapacity)
      : data(buffer), capacity(bufferCapacity) {}

  size_t remaining() const { return used < capacity ? capacity - used : 0; }
  bool full() const { return used + 1 >= capacity; }
  const char* c_str() const { return data ? data : ""; }
};

// Appends at most what fits and never lets `used` run past `capacity`.
// Returns the number of bytes actually written, excluding the terminator.
size_t appendText(TextBuffer& buffer, const char* format, ...)
    __attribute__((format(printf, 2, 3)));

// Appends a string that is already JSON-escaped, honouring the same bound.
size_t appendRaw(TextBuffer& buffer, const char* text);

}  // namespace cauce