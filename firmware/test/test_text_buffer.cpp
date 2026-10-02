#include <cstring>
#include <unity.h>

#include "cauce/core/TextBuffer.h"

using namespace cauce;

namespace {

// A 40-byte canary region right after the buffer, so an overflow is observed
// rather than assumed.
struct Guarded {
  char buffer[64];
  char canary[40];
};

void arm(Guarded& g) {
  std::memset(g.canary, 0x5A, sizeof(g.canary));
  std::memset(g.buffer, 0, sizeof(g.buffer));
}

bool canaryIntact(const Guarded& g) {
  for (size_t i = 0; i < sizeof(g.canary); ++i) {
    if (g.canary[i] != static_cast<char>(0x5A)) return false;
  }
  return true;
}

}  // namespace

void test_appends_and_tracks_used() {
  char buffer[64];
  TextBuffer text(buffer, sizeof(buffer));
  TEST_ASSERT_EQUAL_UINT32(0u, text.used);
  TEST_ASSERT_EQUAL(5, appendText(text, "%s", "hello"));
  TEST_ASSERT_EQUAL_UINT32(5u, text.used);
  TEST_ASSERT_EQUAL_STRING("hello", text.c_str());
  TEST_ASSERT_EQUAL(6, appendText(text, " %s", "world"));
  TEST_ASSERT_EQUAL_STRING("hello world", text.c_str());
  TEST_ASSERT_FALSE(text.truncated);
  TEST_ASSERT_EQUAL_UINT32(11u, text.used);
}

void test_appends_respect_the_capacity() {
  char buffer[8];
  TextBuffer text(buffer, sizeof(buffer));
  // "0123456789" does not fit in 8 bytes with its terminator.
  const size_t written = appendText(text, "%s", "0123456789");
  TEST_ASSERT_EQUAL(7, written);
  TEST_ASSERT_EQUAL_UINT32(7u, text.used);
  TEST_ASSERT_TRUE(text.truncated);
  TEST_ASSERT_EQUAL_STRING("0123456", text.c_str());
  TEST_ASSERT_EQUAL_UINT32(7, std::strlen(buffer));
}

// The bug this type exists for: an unguarded snprintf return value walks past
// the buffer and the next call receives an underflowed remaining size.
void test_never_writes_past_the_buffer() {
  Guarded g;
  arm(g);
  TextBuffer text(g.buffer, sizeof(g.buffer));

  for (int i = 0; i < 50; ++i) {
    appendText(text, "%s", "0123456789");
    appendRaw(text, "0123456789");
  }
  TEST_ASSERT_TRUE_MESSAGE(canaryIntact(g), "wrote past the buffer end");
  TEST_ASSERT_TRUE(text.truncated);
  TEST_ASSERT_TRUE(text.used <= sizeof(g.buffer));
  TEST_ASSERT_TRUE(text.full());
}

void test_appending_after_the_buffer_is_full_is_a_no_op() {
  char buffer[4];
  TextBuffer text(buffer, sizeof(buffer));
  appendRaw(text, "abcd");  // leaves room for the terminator only
  const size_t used = text.used;
  TEST_ASSERT_EQUAL(0, appendText(text, "%s", "more"));
  TEST_ASSERT_EQUAL(0, appendRaw(text, "more"));
  TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(used), text.used);
  TEST_ASSERT_EQUAL_STRING("abc", text.c_str());
}

void test_append_raw_copies_exactly() {
  char buffer[32];
  TextBuffer text(buffer, sizeof(buffer));
  TEST_ASSERT_EQUAL(0, appendRaw(text, ""));
  TEST_ASSERT_EQUAL(0, appendRaw(text, nullptr));
  TEST_ASSERT_EQUAL(3, appendRaw(text, "abc"));
  TEST_ASSERT_EQUAL_STRING("abc", text.c_str());
}

void test_a_null_or_zero_buffer_is_survivable() {
  TextBuffer empty;
  TEST_ASSERT_EQUAL(0, appendText(empty, "%s", "x"));
  TEST_ASSERT_TRUE(empty.truncated);
  TEST_ASSERT_EQUAL_STRING("", empty.c_str());

  char buffer[8] = {0};
  TextBuffer zero(buffer, 0);
  TEST_ASSERT_EQUAL(0, appendText(zero, "%s", "x"));
  TEST_ASSERT_TRUE(zero.truncated);
}

void test_formatted_numbers_are_appended() {
  char buffer[32];
  TextBuffer text(buffer, sizeof(buffer));
  appendText(text, "{\"count\":%u,", 42u);
  appendText(text, "\"ok\":%s}", "true");
  TEST_ASSERT_EQUAL_STRING("{\"count\":42,\"ok\":true}", text.c_str());
}

// Mirrors the shape of the validation-error build that triggered the alert:
// four messages of up to 63 characters into a 512-byte buffer.
void test_the_alert_scenario_stays_inside_the_buffer() {
  char errors[512];
  Guarded canary;
  std::memset(canary.canary, 0x5A, sizeof(canary.canary));
  TextBuffer text(errors, sizeof(errors));

  appendText(text, "{\"errors\":[");
  for (int i = 0; i < 4; ++i) {
    if (text.full()) break;
    if (i > 0) appendRaw(text, ",");
    // Worst case a 63-character message could reach: 139 characters escaped.
    char escaped[140];
    std::memset(escaped, 'x', sizeof(escaped) - 1);
    escaped[sizeof(escaped) - 1] = '\0';
    appendRaw(text, escaped);
  }
  appendRaw(text, "]}");

  TEST_ASSERT_TRUE(text.used <= sizeof(errors));
  // Four 139-character messages plus brackets is 573 bytes into 512, so this
  // build must report truncation rather than write past the end.
  TEST_ASSERT_TRUE(text.truncated);
  TEST_ASSERT_EQUAL(sizeof(errors) - 1, text.used);
  // The terminator is inside the buffer.
  TEST_ASSERT_EQUAL(text.used, std::strlen(errors));
  TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(strlen(errors)),
                           static_cast<uint32_t>(text.used));
}

void registerTextBufferTests() {
  RUN_TEST(test_appends_and_tracks_used);
  RUN_TEST(test_appends_respect_the_capacity);
  RUN_TEST(test_never_writes_past_the_buffer);
  RUN_TEST(test_appending_after_the_buffer_is_full_is_a_no_op);
  RUN_TEST(test_append_raw_copies_exactly);
  RUN_TEST(test_a_null_or_zero_buffer_is_survivable);
  RUN_TEST(test_formatted_numbers_are_appended);
  RUN_TEST(test_the_alert_scenario_stays_inside_the_buffer);
}