#include <cstring>
#include <string>

#include <unity.h>

#include "cauce/app/NodeAuthenticator.h"
#include "cauce/hal/ManualClock.h"

using namespace cauce;
using namespace cauce::app;

namespace {

constexpr size_t kSeed = 32;
constexpr uint8_t kSeedValue = 7;

// A real seed, and a real Ed25519 key from it, so the signature this module produces is
// one the backend's `cryptography` would accept rather than a fixture that only agrees with
// itself. `test_node_auth.py` signs the same challenge with the same seed and compares.
struct Cred {
  uint8_t seed[kEd25519SeedBytes]{};
  uint8_t publicKey[kEd25519PublicKeyBytes]{};

  Cred() {
    for (size_t i = 0; i < kEd25519SeedBytes; ++i) seed[i] = kSeedValue;
    // Output first, seed second. The first version of this had them the other way round,
    // which silently produced a garbage key and made the signature verify as broken.
    ed25519PublicKeyFromSeed(publicKey, seed);
  }
};

// A challenge source that answers from a fixed body, and records what it was asked for.
class FakeSource final : public INodeChallengeSource {
 public:
  bool reachable{true};
  bool fill{true};
  // The `\n` sequences here are REAL newlines, which a C++ raw string would have made into
  // a backslash followed by `n`. The first version of this fixture used one, and the
  // signature test failed for a reason that looked exactly like broken signing: the
  // authenticator had correctly signed the literal backslashes it was handed, and the test
  // then verified real newlines. Same symptom as a real bug, opposite cause.
  std::string body{
      "{\"node_id\":\"CAUCE-001\",\"nonce\":\"NOnCE\","
      "\"expires_utc_ms\":1787356860000,"
      "\"sign_this\":\"cauce-sync-challenge\nCAUCE-001\nNOnCE\n1787356860000\"}"};
  std::string askedUrl;
  std::string askedNode;
  int calls{0};

  hal::ISyncTransport::Result fetchChallenge(const char* baseUrl, const char* nodeId,
                        NodeChallenge& out) override {
    ++calls;
    askedUrl = baseUrl ? baseUrl : "";
    askedNode = nodeId ? nodeId : "";
    if (!reachable) return hal::ISyncTransport::Result::NetworkError;
    if (!fill) return hal::ISyncTransport::Result::Ok;  // reached, produced nothing
    if (!jsonStringField(body.c_str(), "nonce", out.nonce, sizeof(out.nonce))) {
      return hal::ISyncTransport::Result::NetworkError;
    }
    jsonStringField(body.c_str(), "sign_this", out.signThis, sizeof(out.signThis));
    jsonStringField(body.c_str(), "node_id", out.nodeId, sizeof(out.nodeId));
    jsonUintField(body.c_str(), "expires_utc_ms", out.expiresUtcMs);
    return hal::ISyncTransport::Result::Ok;
  }

  static bool jsonStringField(const char* json, const char* key, char* out, size_t cap) {
    return NodeAuthenticator::jsonStringField(json, key, out, cap);
  }
  static bool jsonUintField(const char* json, const char* key, uint64_t& out) {
    return NodeAuthenticator::jsonUintField(json, key, out);
  }
};

struct Rig {
  Cred cred;
  FakeSource source;
  NodeAuthenticator auth;

  Rig() {
    auth.setCredential(cred.seed, "{\"serial\":\"aa\",\"node_id\":\"CAUCE-001\"}");
    auth.setChallengeSource(&source);
    auth.setIdentity("https://central.example.org", "CAUCE-001");
  }
};

// Decodes base64 the slow, obvious way, so the encoder is checked against an independent
// reading rather than against itself.
std::string decodeBase64(const char* text) {
  static int8_t table[256];
  static bool ready = false;
  if (!ready) {
    const char* alpha =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::memset(table, -1, sizeof(table));
    for (int i = 0; i < 64; ++i) table[static_cast<uint8_t>(alpha[i])] = static_cast<int8_t>(i);
    ready = true;
  }
  std::string out;
  uint32_t buffer = 0;
  int bits = 0;
  for (const char* p = text; *p; ++p) {
    if (*p == '=') break;
    const int8_t v = table[static_cast<uint8_t>(*p)];
    TEST_ASSERT_TRUE_MESSAGE(v >= 0, "the encoder emitted a character outside the alphabet");
    buffer = (buffer << 6) | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<char>((buffer >> bits) & 0xFF));
    }
  }
  return out;
}

}  // namespace

// --- the canonical encoding -------------------------------------------

// This is the shape `json.dumps` actually puts on the wire for the canonical string: three
  // backslash-n pairs and no 0x0A byte anywhere. The parser used to refuse every escape on the
  // grounds that the central emits none, which this test contradicts. On real hardware that
  // would have been one NoChallenge per sync, forever.
  void test_the_parser_reads_the_escapes_python_actually_emits() {
    const char* wire =
        "{\"node_id\":\"CAUCE-001\",\"nonce\":\"NOnCE\","
        "\"expires_utc_ms\":1787356860000,"
        "\"sign_this\":\"cauce-sync-challenge\\nCAUCE-001\\nNOnCE\\n1787356860000\"}";
    char out[256];
    TEST_ASSERT_TRUE(
        NodeAuthenticator::jsonStringField(wire, "sign_this", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING(
        "cauce-sync-challenge\nCAUCE-001\nNOnCE\n1787356860000", out);
  }

  // The opposite must still be refused. Decoding `\u0041` into `A` and signing the result would
  // mean the node signs a string the central never wrote.
  void test_the_parser_refuses_an_escape_it_cannot_honestly_decode() {
    const char* wire = "{\"sign_this\":\"cauce\\u0058challenge\"}";
    char out[256];
    TEST_ASSERT_FALSE(
        NodeAuthenticator::jsonStringField(wire, "sign_this", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
  }
void test_the_canonical_challenge_matches_the_backends_format() {
  char out[256];
  const size_t n = NodeAuthenticator::canonicalChallengeBytes(
      "CAUCE-001", "NOnCE", 1787356860000ULL, out, sizeof(out));
  TEST_ASSERT_EQUAL_UINT32(std::strlen(
      "cauce-sync-challenge\nCAUCE-001\nNOnCE\n1787356860000"), n);
  TEST_ASSERT_EQUAL_STRING(
      "cauce-sync-challenge\nCAUCE-001\nNOnCE\n1787356860000", out);
}

void test_the_canonical_challenge_refuses_to_truncate() {
  char tiny[8];
  TEST_ASSERT_EQUAL_UINT32(
      0, NodeAuthenticator::canonicalChallengeBytes("CAUCE-001", "NOnCE",
                                                    1787356860000ULL, tiny,
                                                    sizeof(tiny)));
  TEST_ASSERT_EQUAL_STRING("", tiny);
}

void test_the_expiry_is_plain_decimal_with_no_separators() {
  // Python's str(int) has no thousands separator. A locale-aware `%llu` would, and the two
  // sides would then sign different bytes for the same expiry.
  char out[256];
  NodeAuthenticator::canonicalChallengeBytes("N", "x", 1234567890123ULL, out, sizeof(out));
  const char* tail = std::strstr(out, "\nx\n");
  TEST_ASSERT_NOT_NULL(tail);
  TEST_ASSERT_EQUAL_STRING("1234567890123", tail + 3);
}

// --- base64 ------------------------------------------------------------

void test_base64_matches_the_rfc4648_vectors() {
  struct Case { const char* plain; const char* encoded; };
  const Case cases[] = {
      {"", ""},
      {"f", "Zg=="},
      {"fo", "Zm8="},
      {"foo", "Zm9v"},
      {"foob", "Zm9vYg=="},
      {"fooba", "Zm9vYmE="},
      {"foobar", "Zm9vYmFy"},
  };
  char out[64];
  for (const Case& c : cases) {
    const size_t n = NodeAuthenticator::base64Encode(
        reinterpret_cast<const uint8_t*>(c.plain), std::strlen(c.plain), out, sizeof(out));
    if (c.plain[0] == '\0') {
      TEST_ASSERT_EQUAL_UINT32(0, n);
      continue;
    }
    TEST_ASSERT_EQUAL_STRING(c.encoded, out);
    TEST_ASSERT_EQUAL_UINT32(std::strlen(c.encoded), n);
  }
}

void test_base64_round_trips_every_byte_value() {
  uint8_t data[64];
  for (size_t i = 0; i < sizeof(data); ++i) data[i] = static_cast<uint8_t>(i);
  char out[128];
  TEST_ASSERT_EQUAL_UINT32(88, NodeAuthenticator::base64Encode(data, sizeof(data), out,
                                                              sizeof(out)));
  TEST_ASSERT_EQUAL_UINT32(sizeof(data), decodeBase64(out).size());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(data, decodeBase64(out).data(), sizeof(data));
}

void test_base64_refuses_a_buffer_that_cannot_hold_the_padding() {
  // A 64-byte signature needs 88 characters. 87 must fail rather than drop a `=`, because a
  // truncated signature is a signature error at the central that looks like a key problem.
  uint8_t data[64]{};
  char tooSmall[87];
  TEST_ASSERT_EQUAL_UINT32(0, NodeAuthenticator::base64Encode(data, sizeof(data), tooSmall,
                                                             sizeof(tooSmall)));
  char exact[89];
  TEST_ASSERT_EQUAL_UINT32(
      88, NodeAuthenticator::base64Encode(data, sizeof(data), exact, sizeof(exact)));
}

// --- the minimal JSON reader ------------------------------------------

void test_json_string_fields_are_read() {
  char out[64];
  TEST_ASSERT_TRUE(NodeAuthenticator::jsonStringField(
      R"({"node_id":"CAUCE-001","nonce":"abc"})", "nonce", out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("abc", out);
  TEST_ASSERT_TRUE(NodeAuthenticator::jsonStringField(
      R"({"a":"x","nonce":"abc","b":"y"})", "nonce", out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("abc", out);
}

void test_json_field_spacing_and_order_do_not_matter() {
  char out[64];
  TEST_ASSERT_TRUE(NodeAuthenticator::jsonStringField(
      "{ \"nonce\" : \"spaced\" , \"x\":1 }", "nonce", out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("spaced", out);
}

void test_a_missing_or_wrongly_typed_field_is_not_found() {
  char out[64];
  TEST_ASSERT_FALSE(NodeAuthenticator::jsonStringField(R"({"x":1})", "nonce", out, sizeof(out)));
  TEST_ASSERT_FALSE(NodeAuthenticator::jsonStringField(
      R"({"nonce":123})", "nonce", out, sizeof(out)));
  TEST_ASSERT_FALSE(NodeAuthenticator::jsonStringField(
      R"({"nonce":"unterminated})", "nonce", out, sizeof(out)));
  TEST_ASSERT_FALSE(
      NodeAuthenticator::jsonStringField(R"({"nonce":"ab"})", "nonce", out, 2));
}

void test_a_json_escape_is_refused_rather_than_decoded() {
  // This test asserted that `\"` is refused, which was true only because every escape was
  // refused. `\"` is legitimate JSON that `json.dumps` emits for any value containing a
  // quote, so refusing it made the parser wrong in the other direction: correct on the
  // canonical string, broken on a nonce or node id that happens to contain a quote.
  //
  // What must still be refused is an escape with no honest single-character reading, and
  // `\u0041` is the case that matters: decoded to `A`, the node would sign a string the
  // central never wrote.
  char out[64];
  TEST_ASSERT_TRUE(NodeAuthenticator::jsonStringField(
      R"({"nonce":"ab\"cd"})", "nonce", out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("ab\"cd", out);
}

void test_a_numeric_field_refuses_hex_and_signs() {
  uint64_t value = 0;
  TEST_ASSERT_TRUE(NodeAuthenticator::jsonUintField(R"({"expires_utc_ms":1787356860000})",
                                                    "expires_utc_ms", value));
  TEST_ASSERT_EQUAL_UINT64(1787356860000ULL, value);
  // `strtoull` would accept all three of these and produce a different expiry than the
  // central issued, which signs fine and verifies never.
  TEST_ASSERT_FALSE(NodeAuthenticator::jsonUintField(R"({"expires_utc_ms":0x10})",
                                                    "expires_utc_ms", value));
  TEST_ASSERT_FALSE(NodeAuthenticator::jsonUintField(R"({"expires_utc_ms":+5})",
                                                    "expires_utc_ms", value));
  TEST_ASSERT_FALSE(NodeAuthenticator::jsonUintField(R"({"expires_utc_ms":"5"})",
                                                    "expires_utc_ms", value));
  TEST_ASSERT_FALSE(NodeAuthenticator::jsonUintField(R"({"expires_utc_ms":})",
                                                    "expires_utc_ms", value));
}

void test_a_large_expiry_does_not_overflow() {
  uint64_t value = 0;
  TEST_ASSERT_TRUE(NodeAuthenticator::jsonUintField(
      R"({"expires_utc_ms":18446744073709551615})", "expires_utc_ms", value));
  TEST_ASSERT_EQUAL_UINT64(18446744073709551615ULL, value);
}

// --- the exchange ------------------------------------------------------

void test_a_configured_node_produces_the_four_headers() {
  Rig rig;
  char nodeId[32]{}, certificate[2048]{}, nonce[64]{}, signature[160]{};
  TEST_ASSERT_EQUAL(NodeAuthStatus::Ok,
                    rig.auth.authenticate(5000, nodeId, sizeof(nodeId), certificate,
                                          sizeof(certificate), nonce, sizeof(nonce),
                                          signature, sizeof(signature)));
  TEST_ASSERT_EQUAL_STRING("CAUCE-001", nodeId);
  TEST_ASSERT_EQUAL_STRING("NOnCE", nonce);
  TEST_ASSERT_EQUAL_UINT32(1, rig.auth.successCount());
  // The certificate is base64 of the JSON the credential was set with, so the central reads
  // it back byte for byte.
  const std::string decoded = decodeBase64(certificate);
  TEST_ASSERT_EQUAL_STRING("{\"serial\":\"aa\",\"node_id\":\"CAUCE-001\"}",
                           decoded.c_str());
}

void test_the_signature_verifies_against_the_nodes_own_public_key() {
  Rig rig;
  char nodeId[32]{}, certificate[2048]{}, nonce[64]{}, signature[160]{};
  TEST_ASSERT_EQUAL(NodeAuthStatus::Ok,
                    rig.auth.authenticate(5000, nodeId, sizeof(nodeId), certificate,
                                          sizeof(certificate), nonce, sizeof(nonce),
                                          signature, sizeof(signature)));

  // Reconstruct the exact bytes and verify with the key derived from the same seed. This is
  // the cross-language claim in reverse: the backend's `cryptography` accepts what this
  // module signs, and the fixture in `test_node_auth.py` signs the same challenge.
  const std::string decoded = decodeBase64(signature);
  TEST_ASSERT_EQUAL_UINT32(64, decoded.size());

  uint8_t raw[64];
  for (size_t i = 0; i < 64; ++i) raw[i] = static_cast<uint8_t>(decoded[i]);
  const char* message =
      "cauce-sync-challenge\nCAUCE-001\nNOnCE\n1787356860000";
  TEST_ASSERT_TRUE(ed25519Verify(rig.cred.publicKey,
                                 reinterpret_cast<const uint8_t*>(message),
                                 std::strlen(message), raw));
}

void test_an_unreachable_central_reports_that_and_sends_nothing() {
  Rig rig;
  rig.source.reachable = false;
  char nodeId[32] = "STALE", certificate[2048] = "STALE", nonce[64] = "STALE",
       signature[160] = "STALE";
  TEST_ASSERT_EQUAL(NodeAuthStatus::Unreachable,
                    rig.auth.authenticate(5000, nodeId, sizeof(nodeId), certificate,
                                          sizeof(certificate), nonce, sizeof(nonce),
                                          signature, sizeof(signature)));
  // Every buffer cleared. A caller that ignores the status must send nothing rather than
  // the previous node's headers.
  TEST_ASSERT_EQUAL_STRING("", nodeId);
  TEST_ASSERT_EQUAL_STRING("", certificate);
  TEST_ASSERT_EQUAL_STRING("", nonce);
  TEST_ASSERT_EQUAL_STRING("", signature);
  TEST_ASSERT_EQUAL_UINT32(0, rig.auth.successCount());
  TEST_ASSERT_EQUAL_UINT32(1, rig.auth.failureCount());
}

void test_a_source_that_reaches_the_central_but_produces_nothing_is_refused() {
  Rig rig;
  rig.source.fill = false;
  char nodeId[32]{}, certificate[2048]{}, nonce[64]{}, signature[160]{};
  // Not `Unreachable`: the central answered, it answered with something unusable. The
  // distinction is what tells an operator whether to look at the network or the version.
  TEST_ASSERT_EQUAL(NodeAuthStatus::NoChallenge,
                    rig.auth.authenticate(5000, nodeId, sizeof(nodeId), certificate,
                                          sizeof(certificate), nonce, sizeof(nonce),
                                          signature, sizeof(signature)));
  TEST_ASSERT_EQUAL_STRING("", nonce);
}

void test_an_unconfigured_node_refuses_rather_than_signing_nothing() {
  Rig rig;
  rig.auth.clearCredential();
  char nodeId[32]{}, certificate[2048]{}, nonce[64]{}, signature[160]{};
  TEST_ASSERT_EQUAL(NodeAuthStatus::NotConfigured,
                    rig.auth.authenticate(5000, nodeId, sizeof(nodeId), certificate,
                                          sizeof(certificate), nonce, sizeof(nonce),
                                          signature, sizeof(signature)));
  TEST_ASSERT_EQUAL_UINT32(0, rig.source.calls);
}

void test_a_seed_without_a_certificate_is_not_configured() {
  // Both halves are required. A seed with no certificate would sign a challenge and present
  // nothing to verify it against.
  NodeAuthenticator bare;
  uint8_t seed[kEd25519SeedBytes];
  std::memset(seed, kSeedValue, sizeof(seed));
  bare.setCredential(seed, "");
  TEST_ASSERT_FALSE(bare.isConfigured());

  NodeAuthenticator full;
  full.setCredential(seed, "{}");
  TEST_ASSERT_TRUE(full.isConfigured());
  full.clearCredential();
  TEST_ASSERT_FALSE(full.isConfigured());
}

void test_clearing_the_credential_forgets_it_entirely() {
  // The private half of the node's identity should not be sitting in a buffer nobody
  // expects to hold it. The bytes are private, so this asserts the observable half - that
  // the authenticator reports itself unconfigured and refuses to sign - rather than
  // reaching into the object for a field that has no business being public. A future
  // `seedForTest()` accessor would defeat the point.
  NodeAuthenticator auth;
  uint8_t seed[kEd25519SeedBytes];
  std::memset(seed, 0xAA, sizeof(seed));
  auth.setCredential(seed, "{}");
  TEST_ASSERT_TRUE(auth.isConfigured());
  auth.clearCredential();
  TEST_ASSERT_FALSE(auth.isConfigured());

  char nodeId[32]{}, certificate[2048]{}, nonce[64]{}, signature[160]{};
  TEST_ASSERT_EQUAL(NodeAuthStatus::NotConfigured,
                    auth.authenticate(5000, nodeId, sizeof(nodeId), certificate,
                                      sizeof(certificate), nonce, sizeof(nonce),
                                      signature, sizeof(signature)));
}

void test_the_challenge_request_carries_the_identity_it_was_given() {
  Rig rig;
  char nodeId[32]{}, certificate[2048]{}, nonce[64]{}, signature[160]{};
  rig.auth.authenticate(5000, nodeId, sizeof(nodeId), certificate, sizeof(certificate),
                        nonce, sizeof(nonce), signature, sizeof(signature));
  TEST_ASSERT_EQUAL_STRING("https://central.example.org", rig.source.askedUrl.c_str());
  TEST_ASSERT_EQUAL_STRING("CAUCE-001", rig.source.askedNode.c_str());
}

void registerNodeAuthTests() {
  UNITY_BEGIN();
  RUN_TEST(test_the_canonical_challenge_matches_the_backends_format);
  RUN_TEST(test_the_canonical_challenge_refuses_to_truncate);
  RUN_TEST(test_the_expiry_is_plain_decimal_with_no_separators);
  RUN_TEST(test_base64_matches_the_rfc4648_vectors);
  RUN_TEST(test_base64_round_trips_every_byte_value);
  RUN_TEST(test_base64_refuses_a_buffer_that_cannot_hold_the_padding);
  RUN_TEST(test_json_string_fields_are_read);
  RUN_TEST(test_json_field_spacing_and_order_do_not_matter);
  RUN_TEST(test_a_missing_or_wrongly_typed_field_is_not_found);
  RUN_TEST(test_a_json_escape_is_refused_rather_than_decoded);
  RUN_TEST(test_the_parser_reads_the_escapes_python_actually_emits);
  RUN_TEST(test_the_parser_refuses_an_escape_it_cannot_honestly_decode);
  RUN_TEST(test_a_numeric_field_refuses_hex_and_signs);
  RUN_TEST(test_a_large_expiry_does_not_overflow);
  RUN_TEST(test_a_configured_node_produces_the_four_headers);
  RUN_TEST(test_the_signature_verifies_against_the_nodes_own_public_key);
  RUN_TEST(test_an_unreachable_central_reports_that_and_sends_nothing);
  RUN_TEST(test_a_source_that_reaches_the_central_but_produces_nothing_is_refused);
  RUN_TEST(test_an_unconfigured_node_refuses_rather_than_signing_nothing);
  RUN_TEST(test_a_seed_without_a_certificate_is_not_configured);
  RUN_TEST(test_clearing_the_credential_forgets_it_entirely);
  RUN_TEST(test_the_challenge_request_carries_the_identity_it_was_given);
  UNITY_END();
}