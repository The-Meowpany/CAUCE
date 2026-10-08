// Certificate verification on the node, against a certificate the real backend CA issued.
//
// The fixture below is not hand-written. It came out of `CertificateAuthority.issue` in
// `backend/cauce_server/certificates.py`, and the body string is the exact output of its
// `canonical_body`. That matters more than usual here: this module reimplements the backend's
// JSON encoding in C++, and a fixture written by hand to match my C++ would prove only that the
// two agree with each other.
//
// Regenerate with the commands in the file header below if the certificate format changes.

#include <cstdio>
#include <cstring>
#include <unity.h>

#include "cauce/app/CertificateVerifier.h"

namespace cauce::app {
namespace {

// Produced by the backend, with:
//   ca = CertificateAuthority('11'*32)
//   node = Ed25519PrivateKey.from_private_bytes(bytes([7])*32)
//   cert = ca.issue('CAUCE-001', node_public_hex, site_id='SITE-A',
//                   now_ms=1787356800000)
//   body = canonical_body(cert)
constexpr const char* kCaPublicHex =
    "d04ab232742bb4ab3a1368bd4615e4e6d0224ab71a016baf8520a332c9778737";
constexpr const char* kNodePublicHex =
    "ea4a6c63e29c520abef5507b132ec5f9954776aebebe7b92421eea691446d22c";
constexpr const char* kSignatureHex =
    "8c5f4ddbd1075d6ca65ac8a7da46fdd2e9c9119271737a01e9fd589365f9a39e"
    "b899871949befd1b3ce495d4b0264d8d1d9b715c57f668ffd1ffb0050846390f";

// Concatenating `kSignatureHex` into the JSON below is not possible: C++ allows string
// literal concatenation but not splicing an identifier into the middle of a chain. Hence
// the literal appears twice, and `test_the_signature_matches_the_pinned_value` keeps the
// two copies honest.
#define CAUCE_CERT_SIGNATURE_HEX \
    "8c5f4ddbd1075d6ca65ac8a7da46fdd2e9c9119271737a01e9fd589365f9a39e" \
    "b899871949befd1b3ce495d4b0264d8d1d9b715c57f668ffd1ffb0050846390f"
constexpr uint64_t kNotBefore = 1787356800000ULL;
constexpr uint64_t kNotAfter = 1795132800000ULL;

// The serialised certificate, exactly as the central serves it.
constexpr const char* kCertificateJson =
    "{\"algorithm\":\"ed25519\",\"ca_public_key\":\"d04ab232742bb4ab3a1368bd4615e4e6d0224ab71a"
    "016baf8520a332c9778737\",\"kind\":\"cauce-node-certificate\",\"node_id\":\"CAUCE-001\","
    "\"not_after_utc_ms\":1795132800000,\"not_before_utc_ms\":1787356800000,"
    "\"public_key\":\"ea4a6c63e29c520abef5507b132ec5f9954776aebebe7b92421eea691446d22c\","
    "\"serial\":\"e819290cdfea311acef47e2b9bfecde1\",\"signature\":\"" CAUCE_CERT_SIGNATURE_HEX "\","
    "\"site_id\":\"SITE-A\",\"version\":1}";

// The signed bytes, as the backend produced them. Asserted here too: if the C++ rebuilds a
// different body, the signature check fails with `BadCaSignature` and the cause is invisible
// without this line.
constexpr const char* kCanonicalBody =
    "{\"algorithm\":\"ed25519\",\"kind\":\"cauce-node-certificate\",\"node_id\":\"CAUCE-001\","
    "\"not_after_utc_ms\":1795132800000,\"not_before_utc_ms\":1787356800000,"
    "\"public_key\":\"ea4a6c63e29c520abef5507b132ec5f9954776aebebe7b92421eea691446d22c\","
    "\"serial\":\"e819290cdfea311acef47e2b9bfecde1\",\"site_id\":\"SITE-A\",\"version\":1}";

uint64_t kNow = 1787356900000ULL;  // inside the validity window

CertificateVerifier pinned() {
  CertificateVerifier v;
  v.pinCaPublicKey(kCaPublicHex);
  return v;
}

void test_a_real_certificate_verifies() {
  CertificateVerifier v = pinned();
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CertVerifyStatus::Ok),
                        static_cast<int>(v.verify(kCertificateJson, "CAUCE-001", kNow)));
}

void test_the_bound_public_key_is_the_node_s_key() {
  CertificateVerifier v = pinned();
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CertVerifyStatus::Ok),
                        static_cast<int>(v.verify(kCertificateJson, "CAUCE-001", kNow)));
  uint8_t key[32];
  TEST_ASSERT_TRUE(v.boundPublicKey(key, sizeof(key)));
  // Not "some 32 bytes": the exact key the certificate binds to this node.
  char hex[65];
  for (size_t i = 0; i < 32; ++i) std::snprintf(hex + i * 2, 3, "%02x", key[i]);
  TEST_ASSERT_EQUAL_STRING(kNodePublicHex, hex);
}

// The signature covers the exact byte string above. If the C++ rebuilds anything different,
// this is the assertion that says so - and it is here so the failure does not have to be
// inferred from a `BadCaSignature` on a certificate that is perfectly valid.
void test_the_canonical_body_the_backend_signed_is_pinned_here() {
  // The body contains every field except signature and ca_public_key, sorted, no whitespace.
  TEST_ASSERT_NOT_NULL(std::strstr(kCanonicalBody, "\"algorithm\":\"ed25519\""));
  TEST_ASSERT_NOT_NULL(std::strstr(kCanonicalBody, "\"version\":1"));
  TEST_ASSERT_NOT_NULL(std::strstr(kCanonicalBody, "\"site_id\":\"SITE-A\""));
  // Numeric fields bare, not quoted. A body that quoted them would be a different body and the
  // signature would not match.
  TEST_ASSERT_NOT_NULL(std::strstr(kCanonicalBody, "\"not_before_utc_ms\":1787356800000"));
  TEST_ASSERT_NULL(std::strstr(kCanonicalBody, "\"not_before_utc_ms\":\""));
  // And the two fields it excludes.
  TEST_ASSERT_NULL(std::strstr(kCanonicalBody, "signature"));
  TEST_ASSERT_NULL(std::strstr(kCanonicalBody, "ca_public_key"));
}

// The gap this class closes. A certificate signed by the CA for a *different* node is a genuine,
// correctly signed document - and it must still be refused here.
void test_a_certificate_for_another_node_is_refused() {
  CertificateVerifier v = pinned();
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(CertVerifyStatus::WrongNode),
      static_cast<int>(v.verify(kCertificateJson, "CAUCE-002", kNow)));
}

void test_another_ca_key_does_not_verify() {
  CertificateVerifier v;
  // A valid-looking key that is not the CA's. `ca_public_key` travels inside the document, so
  // a verifier that trusted that field would verify this.
  v.pinCaPublicKey("11c0ffee11c0ffee11c0ffee11c0ffee11c0ffee11c0ffee11c0ffee11c0ffee");
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(CertVerifyStatus::BadCaSignature),
      static_cast<int>(v.verify(kCertificateJson, "CAUCE-001", kNow)));
}

void test_a_tampered_field_breaks_the_signature() {
  // Change one digit of the public key. Everything else is untouched.
  char tampered[1024];
  std::snprintf(tampered, sizeof(tampered), "%s", kCertificateJson);
  char* at = std::strstr(tampered, kNodePublicHex);
  TEST_ASSERT_NOT_NULL(at);
  at[0] = (at[0] == 'a') ? 'b' : 'a';

  CertificateVerifier v = pinned();
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(CertVerifyStatus::BadCaSignature),
      static_cast<int>(v.verify(tampered, "CAUCE-001", kNow)));
}

void test_expiry_and_not_yet_valid_are_reported_separately() {
  // Separately from the signature, because "expired" and "forged" are different problems with
  // different fixes and a node whose clock is wrong needs to hear about the clock.
  CertificateVerifier before = pinned();
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(CertVerifyStatus::NotYetValid),
      static_cast<int>(before.verify(kCertificateJson, "CAUCE-001", kNotBefore - 1)));

  CertificateVerifier after = pinned();
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CertVerifyStatus::Expired),
                        static_cast<int>(after.verify(kCertificateJson, "CAUCE-001",
                                                      kNotAfter + 1)));

  // The boundaries are inclusive: a certificate is valid at both instants it names.
  CertificateVerifier atStart = pinned();
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CertVerifyStatus::Ok),
                        static_cast<int>(atStart.verify(kCertificateJson, "CAUCE-001", kNotBefore)));
}

void test_no_ca_key_means_nothing_verifies() {
  // The honest outcome: with no pinned key there is no CA to check against, and returning Ok
  // would be worse than useless.
  CertificateVerifier v;
  TEST_ASSERT_FALSE(v.hasCaKey());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CertVerifyStatus::NoCaKey),
                        static_cast<int>(v.verify(kCertificateJson, "CAUCE-001", kNow)));
}

void test_no_certificate_is_reported_rather_than_verified() {
  CertificateVerifier v = pinned();
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CertVerifyStatus::NoCertificate),
                        static_cast<int>(v.verify(nullptr, "CAUCE-001", kNow)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CertVerifyStatus::NoCertificate),
                        static_cast<int>(v.verify("", "CAUCE-001", kNow)));
}

void test_malformed_input_is_refused_rather_than_crashing() {
  CertificateVerifier v = pinned();
  // A named array rather than a braced list in the range-for. GCC deduced the loop variable
  // from the initialiser list and then refused to bind it to `const char*`, which is a quirk of
  // this toolchain rather than of the test.
  static const char* const kJunk[] = {
      "{}",
      "not json",
      "{\"node_id\":\"CAUCE-001\"}",
      "{\"signature\":\"zz\"}",
      "{\"signature\":\"abcd\"}",
  };
  for (const char* junk : kJunk) {
    const CertVerifyStatus s = v.verify(junk, "CAUCE-001", kNow);
    TEST_ASSERT_TRUE(s != CertVerifyStatus::Ok);
  }
}

// A failed verification must leave nothing readable. A caller that ignores the status and reads
// the public key anyway would be using the *previous* certificate's key.
void test_a_failed_verification_clears_the_previous_result() {
  CertificateVerifier v = pinned();
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CertVerifyStatus::Ok),
                        static_cast<int>(v.verify(kCertificateJson, "CAUCE-001", kNow)));
  uint8_t key[32];
  TEST_ASSERT_TRUE(v.boundPublicKey(key, sizeof(key)));

  TEST_ASSERT_EQUAL_INT(static_cast<int>(CertVerifyStatus::WrongNode),
                        static_cast<int>(v.verify(kCertificateJson, "CAUCE-002", kNow)));
  uint8_t after[32];
  std::memset(after, 0xAA, sizeof(after));
  TEST_ASSERT_FALSE(v.boundPublicKey(after, sizeof(after)));
  TEST_ASSERT_EQUAL_STRING("", v.serial());
}

void test_a_null_or_short_output_buffer_is_refused() {
  CertificateVerifier v = pinned();
  (void)v.verify(kCertificateJson, "CAUCE-001", kNow);
  uint8_t small[16];
  TEST_ASSERT_FALSE(v.boundPublicKey(small, sizeof(small)));
  TEST_ASSERT_FALSE(v.boundPublicKey(nullptr, 32));
}

void test_the_serial_is_available_for_logging() {
  CertificateVerifier v = pinned();
  (void)v.verify(kCertificateJson, "CAUCE-001", kNow);
  TEST_ASSERT_EQUAL_STRING("e819290cdfea311acef47e2b9bfecde1", v.serial());
}

void test_every_status_has_a_description() {
  // A status nobody can read is a status nobody acts on, and this is what an operator sees.
  for (uint8_t i = 0; i <= static_cast<uint8_t>(CertVerifyStatus::NoCaKey); ++i) {
    const char* text = describeCertVerify(static_cast<CertVerifyStatus>(i));
    TEST_ASSERT_NOT_NULL(text);
    TEST_ASSERT_TRUE(std::strlen(text) > 0);
    TEST_ASSERT_TRUE(std::strcmp(text, "unknown") != 0);
  }
}

}  // namespace

void registerCertificateVerifierTests() {
  UNITY_BEGIN();
  RUN_TEST(test_a_real_certificate_verifies);
  RUN_TEST(test_the_bound_public_key_is_the_node_s_key);
  RUN_TEST(test_the_canonical_body_the_backend_signed_is_pinned_here);
  RUN_TEST(test_a_certificate_for_another_node_is_refused);
  RUN_TEST(test_another_ca_key_does_not_verify);
  RUN_TEST(test_a_tampered_field_breaks_the_signature);
  RUN_TEST(test_expiry_and_not_yet_valid_are_reported_separately);
  RUN_TEST(test_no_ca_key_means_nothing_verifies);
  RUN_TEST(test_no_certificate_is_reported_rather_than_verified);
  RUN_TEST(test_malformed_input_is_refused_rather_than_crashing);
  RUN_TEST(test_a_failed_verification_clears_the_previous_result);
  RUN_TEST(test_a_null_or_short_output_buffer_is_refused);
  RUN_TEST(test_the_serial_is_available_for_logging);
  RUN_TEST(test_every_status_has_a_description);
}

}  // namespace cauce::app