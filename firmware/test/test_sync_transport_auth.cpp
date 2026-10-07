// The certificate headers a sync POST must carry, tested where there is no ESP32.
//
// The bridge that decides which headers go out and in what order.
//
// It exists because the ESP32 transport cannot be tested. It needs an `HTTPClient`, a radio
// and a server, none of which the host suite has - so the header list, the order, and the rule
// that a failure emits nothing all lived in a `.cpp` guarded by `#ifdef ARDUINO_ARCH_ESP32`
// and were untested. The first version of this put the credential in `cauce_hal` to reach the
// transport, and closed a dependency cycle; this one inverts it instead.
//
// What that costs is the ability to change the header list without touching `cauce_hal`, which
// is why the names live in `SyncTransportAuth.h` and are asserted below against the spelling
// the central actually reads.

#include <cstring>
#include <string>
#include <unity.h>
#include <vector>

#include "cauce/app/NodeCertificateAuth.h"
#include "cauce/app/NodeAuthenticator.h"
#include "cauce/hal/SyncTransportAuth.h"

namespace cauce::app {

namespace {

using hal::IHeaderSink;
using hal::ISyncTransport;

// Records instead of sending. The point of injecting this is to assert on the emitted
// sequence; a sink that could only be the real client could not be asserted on at all.
class RecordingSink final : public IHeaderSink {
 public:
  void addHeader(const char* name, const char* value) override {
    names.emplace_back(name ? name : "<null>");
    values.emplace_back(value ? value : "<null>");
  }

  bool has(const char* name) const {
    for (size_t i = 0; i < names.size(); ++i) {
      if (names[i] == name) return true;
    }
    return false;
  }

  std::string valueOf(const char* name) const {
    for (size_t i = 0; i < names.size(); ++i) {
      if (names[i] == name) return values[i];
    }
    return {};
  }

  size_t count() const { return names.size(); }

  std::vector<std::string> names;
  std::vector<std::string> values;
};

// Base64 decode for the assertion below. The authenticator only encodes - decoding is not
// part of what it does on a node - so the test brings its own rather than adding a function
// to the component for the convenience of a test. Written out because the module's encoder is
// host-tested against RFC 4648 vectors, and a decoder that shared a bug with it would prove
// nothing.
std::vector<uint8_t> decodeBase64(const std::string& text) {
  static const char* kAlphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::vector<uint8_t> out;
  uint32_t acc = 0;
  int bits = 0;
  for (char c : text) {
    if (c == '=' ) break;
    const char* pos = std::strchr(kAlphabet, c);
    if (pos == nullptr) continue;
    acc = (acc << 6) | static_cast<uint32_t>(pos - kAlphabet);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
    }
  }
  return out;
}

size_t decodeBase64(const std::string& text, uint8_t* out, size_t capacity) {
  const std::vector<uint8_t> raw = decodeBase64(text);
  if (raw.size() > capacity) return 0;
  for (size_t i = 0; i < raw.size(); ++i) out[i] = raw[i];
  return raw.size();
}

// A central that answers with the serialised challenge the backend really emits.
class FakeSource final : public INodeChallengeSource {
 public:
  bool reachable{true};
  bool answerWithNonsense{false};
  int calls{0};

  ISyncTransport::Result fetchChallenge(const char* baseUrl, const char* nodeId,
                                        NodeChallenge& out) override {
    ++calls;
    if (!reachable) return ISyncTransport::Result::NetworkError;

    const std::string body =
        answerWithNonsense
            ? std::string("{\"node_id\":\"CAUCE-001\"}")
            : std::string(
                  "{\"node_id\": \"CAUCE-001\", \"nonce\": \"NOnCE\", "
                  "\"expires_utc_ms\": 1787356860000, "
                  "\"sign_this\": \"cauce-sync-challenge\\nCAUCE-001\\nNOnCE\\n"
                  "1787356860000\"}");
    if (!NodeAuthenticator::jsonStringField(body.c_str(), "node_id", out.nodeId,
                                           sizeof(out.nodeId))) {
      return ISyncTransport::Result::ServerError;
    }
    if (!NodeAuthenticator::jsonStringField(body.c_str(), "nonce", out.nonce,
                                            sizeof(out.nonce))) {
      return ISyncTransport::Result::ServerError;
    }
    if (!NodeAuthenticator::jsonStringField(body.c_str(), "sign_this", out.signThis,
                                            sizeof(out.signThis))) {
      return ISyncTransport::Result::ServerError;
    }
    if (!NodeAuthenticator::jsonUintField(body.c_str(), "expires_utc_ms",
                                          out.expiresUtcMs)) {
      return ISyncTransport::Result::ServerError;
    }
    return ISyncTransport::Result::Ok;
  }
};

struct Rig {
  NodeAuthenticator authenticator;
  FakeSource source;
  RecordingSink sink;
  NodeCertificateAuth bridge;
};

constexpr uint8_t kSeed[32] = {7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
                               7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7};
constexpr const char* kCertificate = R"({"node_id":"CAUCE-001","public_key":"aa"})";

void configure(Rig& rig) {
  rig.authenticator.setCredential(kSeed, kCertificate);
  rig.authenticator.setIdentity("https://central.example", "CAUCE-001");
  rig.bridge.setAuthenticator(&rig.authenticator);
  rig.bridge.setChallengeSource(&rig.source);
}

void test_a_signed_challenge_emits_the_four_headers_in_order() {
  Rig rig;
  configure(rig);
  TEST_ASSERT_TRUE(rig.bridge.addAuthHeaders("https://central.example", "CAUCE-001", 5000,
                                             rig.sink));

  TEST_ASSERT_EQUAL_UINT32(4, rig.sink.count());
  TEST_ASSERT_EQUAL_STRING(hal::headers::kNode, rig.sink.names[0].c_str());
  TEST_ASSERT_EQUAL_STRING(hal::headers::kCertificate, rig.sink.names[1].c_str());
  TEST_ASSERT_EQUAL_STRING(hal::headers::kNonce, rig.sink.names[2].c_str());
  TEST_ASSERT_EQUAL_STRING(hal::headers::kNonceSignature, rig.sink.names[3].c_str());
}

// The header *names* are the wire contract with FastAPI's `x_cauce_*` parameters. A typo here
// is a 401 that reports "certificate auth with incomplete headers" and nothing more, so the
// names are asserted against the spelling the central expects rather than left to review.
void test_the_header_names_are_the_ones_the_central_reads() {
  TEST_ASSERT_EQUAL_STRING("X-Cauce-Node", hal::headers::kNode);
  TEST_ASSERT_EQUAL_STRING("X-Cauce-Certificate", hal::headers::kCertificate);
  TEST_ASSERT_EQUAL_STRING("X-Cauce-Nonce", hal::headers::kNonce);
  TEST_ASSERT_EQUAL_STRING("X-Cauce-Nonce-Signature", hal::headers::kNonceSignature);
}

// The central commits to certificate auth the moment it sees any certificate header, and then
// will not fall back to the shared secret. So a partial set is worse than an empty one: it
// would be sent, and the central would reject a signature the node believes it produced.
void test_an_unreachable_central_emits_nothing_at_all() {
  Rig rig;
  configure(rig);
  rig.source.reachable = false;

  TEST_ASSERT_FALSE(rig.bridge.addAuthHeaders("https://central.example", "CAUCE-001", 5000,
                                              rig.sink));
  TEST_ASSERT_EQUAL_UINT32(0, rig.sink.count());
}

void test_an_unusable_answer_emits_nothing_at_all() {
  Rig rig;
  configure(rig);
  rig.source.answerWithNonsense = true;

  TEST_ASSERT_FALSE(rig.bridge.addAuthHeaders("https://central.example", "CAUCE-001", 5000,
                                              rig.sink));
  TEST_ASSERT_EQUAL_UINT32(0, rig.sink.count());
}

// Stale buffers from a previous call must not survive into the next one. A caller that
// reuses the struct, which is the natural thing to do, would otherwise re-send a signature
// for a challenge that has already been spent.
void test_the_buffers_are_cleared_before_they_are_filled() {
  Rig rig;
  configure(rig);
  // One successful call populates the buffers; the second must not be able to re-emit them.
  // The buffers are private now, so this is asserted through what reaches the sink - which is
  // the only place a stale value could actually do harm anyway.
  TEST_ASSERT_TRUE(rig.bridge.addAuthHeaders("https://central.example", "CAUCE-001", 5000,
                                             rig.sink));
  const size_t afterFirst = rig.sink.count();
  TEST_ASSERT_EQUAL_UINT32(4, afterFirst);

  rig.source.reachable = false;
  TEST_ASSERT_FALSE(rig.bridge.addAuthHeaders("https://central.example", "CAUCE-001", 5000,
                                              rig.sink));
  // Four, still four. A stale signature would make this eight.
  TEST_ASSERT_EQUAL_UINT32(afterFirst, rig.sink.count());
}

void test_an_unconfigured_authenticator_emits_nothing() {
  Rig rig;
  // No credential: a node provisioned before this existed, or one whose flash was blanked.
  rig.bridge.setAuthenticator(&rig.authenticator);
  TEST_ASSERT_FALSE(rig.bridge.addAuthHeaders("https://central.example", "CAUCE-001", 5000,
                                              rig.sink));
  TEST_ASSERT_EQUAL_UINT32(0, rig.sink.count());
  // It must not have asked either: there is nothing it could answer with.
  TEST_ASSERT_EQUAL_INT(0, rig.source.calls);
}

// The source is passed as a parameter rather than configured once at setup, so the function
// cannot sign a challenge from a source other than the one it was handed. Proven by counting
// calls: the authenticator must ask this source, exactly once.
void test_the_challenge_comes_from_the_source_that_was_passed_in() {
  Rig rig;
  configure(rig);
  rig.bridge.setChallengeSource(&rig.source);
  // A stale source on the authenticator must not be able to answer for the one passed in.
  rig.authenticator.setChallengeSource(nullptr);

  TEST_ASSERT_TRUE(rig.bridge.addAuthHeaders("https://central.example", "CAUCE-001", 5000,
                                             rig.sink));
  TEST_ASSERT_EQUAL_INT(1, rig.source.calls);
}

// The signature in the header has to verify against the node's own public key, over the exact
// bytes the central will verify. Checking it here means a defect in the header path cannot
// produce a well-formed header carrying a bad signature.
void test_the_emitted_signature_verifies_over_the_challenge() {
  Rig rig;
  configure(rig);
  TEST_ASSERT_TRUE(rig.bridge.addAuthHeaders("https://central.example", "CAUCE-001", 5000,
                                             rig.sink));

  const std::string signature = rig.sink.valueOf(hal::headers::kNonceSignature);
  uint8_t decoded[64];
  const size_t n = decodeBase64(signature, decoded, sizeof(decoded));
  TEST_ASSERT_EQUAL_UINT32(64, n);

  uint8_t publicKey[32];
  TEST_ASSERT_TRUE(ed25519PublicKeyFromSeed(publicKey, kSeed));
  const std::string message =
      "cauce-sync-challenge\nCAUCE-001\nNOnCE\n1787356860000";
  TEST_ASSERT_TRUE(ed25519Verify(
      publicKey, reinterpret_cast<const uint8_t*>(message.data()), message.size(),
      reinterpret_cast<const uint8_t*>(decoded)));
}

}  // namespace

void registerSyncTransportAuthTests() {
  UNITY_BEGIN();
  RUN_TEST(test_a_signed_challenge_emits_the_four_headers_in_order);
  RUN_TEST(test_the_header_names_are_the_ones_the_central_reads);
  RUN_TEST(test_an_unreachable_central_emits_nothing_at_all);
  RUN_TEST(test_an_unusable_answer_emits_nothing_at_all);
  RUN_TEST(test_the_buffers_are_cleared_before_they_are_filled);
  RUN_TEST(test_an_unconfigured_authenticator_emits_nothing);
  RUN_TEST(test_the_challenge_comes_from_the_source_that_was_passed_in);
  RUN_TEST(test_the_emitted_signature_verifies_over_the_challenge);
}

}  // namespace cauce::app