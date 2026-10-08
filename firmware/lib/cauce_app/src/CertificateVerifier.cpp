#include "cauce/app/CertificateVerifier.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "cauce/core/Ed25519Points.h"
#include "cauce/hal/ISyncTransport.h"

namespace cauce::app {

namespace {

// The certificate body fields the backend signs, in the order `sort_keys` produces them.
//
// This is a *sorted* list because that is what the signature covers, and reproducing an
// arbitrary field order is a second encoding to get wrong. The backend's own comment says the
// firmware "has to reproduce this byte for byte", and this is the reproduction: the same keys,
// the same separators, no whitespace, UTF-8.
//
// `site_id` may be null on the wire. `json.dumps` writes `null` for it, which is four bytes and
// not zero - so a certificate with no site serialises differently from one with an empty site.
// Getting that wrong means every signature check fails, which is at least a loud failure.
constexpr const char* kBodyKeys[] = {"algorithm", "kind",      "node_id", "not_after_utc_ms",
                                     "not_before_utc_ms", "public_key", "serial",
                                     "site_id",           "version"};
constexpr size_t kBodyKeyCount = sizeof(kBodyKeys) / sizeof(kBodyKeys[0]);

bool hexNibble(char c, int& out) {
  if (c >= '0' && c <= '9') { out = c - '0'; return true; }
  if (c >= 'a' && c <= 'f') { out = 10 + (c - 'a'); return true; }
  if (c >= 'A' && c <= 'F') { out = 10 + (c - 'A'); return true; }
  return false;
}

bool decodeHex32(const char* hex, uint8_t* out) {
  if (hex == nullptr) return false;
  size_t n = 0;
  for (; hex[2 * n] != '\0' && hex[2 * n + 1] != '\0'; ++n) {
    int hi = 0;
    int lo = 0;
    if (!hexNibble(hex[2 * n], hi) || !hexNibble(hex[2 * n + 1], lo)) return false;
    out[n] = static_cast<uint8_t>((hi << 4) | lo);
    if (n >= 32) return false;
  }
  return n == 32;
}

// Extracts a `"key":` field from flat JSON. Deliberately reusing `NodeAuthenticator`'s parser
// would be ideal, but that parser refuses escapes and the canonical body contains none here, so
// the shape is compatible - and a second implementation of JSON scanning in the same class is
// avoidable, so this lives as a small local helper with the limitation stated.
//
// The certificate's `node_id` and `site_id` may contain characters JSON escapes, so this refuses
// a value containing a backslash rather than silently decoding it. A node id with a backslash in
// it is not a thing this project issues, and guessing would mean signing bytes the CA did not.
// Appends into a fixed buffer, refusing the moment the result would not fit.
//
// Three bugs live in the obvious version of this, and CodeQL flagged two of them:
//
// 1. `used += snprintf(out + used, capacity - used, ...)`. `snprintf` returns the length it
//    *would* have written on truncation, so `used` becomes larger than the capacity and every
//    later append writes further out of bounds. The loop then compounds it.
// 2. `capacity - used` when `used >= capacity`. Both are `size_t`, so the subtraction wraps to
//    nearly 4 GB and `snprintf` is handed a length it would never have been able to fill - which
//    is the alert: "potentially overflowing call to snprintf".
// 3. `out[used++] = ','` with no check at all, which writes one past the end at exactly the
//    boundary where 1 and 2 have not yet bitten.
//
// The fix is not a bigger buffer. It is refusing instead of computing: every append is checked,
// and one that would not fit ends the verification as `Malformed` rather than corrupting
// anything. A certificate whose fields cannot fit in 512 bytes is not a certificate this build
// can verify, and saying so is the correct answer.
bool appendFormatted(char* out, size_t capacity, size_t& used, const char* format, ...)
    __attribute__((format(printf, 4, 5)));

bool appendFormatted(char* out, size_t capacity, size_t& used, const char* format, ...) {
  if (out == nullptr || used >= capacity) return false;
  va_list args;
  va_start(args, format);
  const int written = std::vsnprintf(out + used, capacity - used, format, args);
  va_end(args);
  if (written < 0) return false;
  // `>=` not `>`: a result that exactly fills the buffer has no room for the terminator.
  if (static_cast<size_t>(written) >= capacity - used) return false;
  used += static_cast<size_t>(written);
  return true;
}

bool appendChar(char* out, size_t capacity, size_t& used, char c) {
  if (out == nullptr || used + 1 >= capacity) return false;
  out[used++] = c;
  return true;
}

// Reads one JSON value for `key`, whether it is a string or a bare number.
//
// Both, deliberately. The first version of this read only strings, and `version` in a
// certificate is a bare number - so every verification returned `Malformed` against a perfectly
// valid certificate. That is the same bug class as `jsonUintField` refusing a string: a parser
// that assumes one shape and is silently useless for the other.
//
// A quoted value is returned unquoted with escapes refused; a bare token is returned as-is.
bool jsonField(const char* json, const char* key, char* out, size_t capacity) {
  if (json == nullptr || key == nullptr || out == nullptr || capacity == 0) return false;
  out[0] = '\0';
  char needle[32];
  const int n = std::snprintf(needle, sizeof(needle), "\"%s\"", key);
  if (n <= 0 || static_cast<size_t>(n) >= sizeof(needle)) return false;

  const char* at = json;
  while ((at = std::strstr(at, needle)) != nullptr) {
    const char* colon = at + n;
    while (*colon == ' ' || *colon == '\t') ++colon;
    if (*colon != ':') { at += n; continue; }
    ++colon;
    while (*colon == ' ' || *colon == '\t') ++colon;

    // `null` is a legitimate value for site_id and means "no site", not "malformed".
    if (*colon == 'n' && std::strncmp(colon, "null", 4) == 0) return true;

    size_t written = 0;
    if (*colon == '"') {
      ++colon;
      while (*colon != '\0' && *colon != '"') {
        // Escapes refused rather than decoded: a node id with a backslash in it is not a thing
        // this project issues, and guessing would mean signing bytes the CA did not.
        if (*colon == '\\') return false;
        if (written + 1 >= capacity) { out[0] = '\0'; return false; }
        out[written++] = *colon++;
      }
      if (*colon != '"') return false;
      out[written] = '\0';
      return true;
    }

    // A bare token: digits for the numeric fields, and nothing else. Anything with a quote, a
    // brace or a space in it is not the value we are looking for.
    while (*colon != '\0' && *colon != ',' && *colon != '}' && *colon != ' ' &&
           *colon != '\t' && *colon != '\n') {
      if (*colon == '"' || *colon == '{' || *colon == '[') return false;
      if (written + 1 >= capacity) { out[0] = '\0'; return false; }
      out[written++] = *colon++;
    }
    if (written == 0) return false;
    out[written] = '\0';
    return true;
  }
  return false;
}

// Numeric field, digits only. `strtoull` would take `0x10` and a leading `+`, and JSON takes
// neither - the same trap `jsonUintField` had.
bool jsonUintField(const char* json, const char* key, uint64_t& out) {
  char buf[32];
  if (!jsonField(json, key, buf, sizeof(buf))) return false;
  if (buf[0] == '\0') return false;
  char* end = nullptr;
  const unsigned long long value = std::strtoull(buf, &end, 10);
  if (end == buf) return false;
  // Trailing junk means the field was not a plain integer.
  while (*end == ' ' || *end == '\t') ++end;
  if (*end != '\0') return false;
  for (const char* p = buf; *p != '\0'; ++p) {
    if (*p < '0' || *p > '9') return false;
  }
  out = value;
  return true;
}

}  // namespace

const char* describeCertVerify(CertVerifyStatus status) {
  switch (status) {
    case CertVerifyStatus::Ok: return "ok";
    case CertVerifyStatus::NoCertificate: return "no certificate configured";
    case CertVerifyStatus::Malformed: return "the certificate is not the expected shape";
    case CertVerifyStatus::BadCaSignature: return "the CA did not sign this certificate";
    case CertVerifyStatus::WrongNode: return "the certificate names a different node";
    case CertVerifyStatus::NotYetValid: return "the certificate is not valid yet";
    case CertVerifyStatus::Expired: return "the certificate has expired";
    case CertVerifyStatus::NoCaKey: return "no CA public key is pinned";
  }
  return "unknown";
}

void CertificateVerifier::copyHex(const char* hex, char* out) const {
  out[0] = '\0';
  if (hex == nullptr) return;
  std::snprintf(out, 65, "%s", hex);
}

bool CertificateVerifier::boundPublicKey(uint8_t* out, size_t capacity) const {
  if (!verified_ || out == nullptr || capacity < sizeof(publicKey_)) return false;
  std::memcpy(out, publicKey_, sizeof(publicKey_));
  return true;
}

CertVerifyStatus CertificateVerifier::verify(const char* certificateJson, const char* nodeId,
                                              uint64_t nowUtcMs) {
  // Everything is cleared first, so a caller that ignores the result cannot go on to trust a
  // previous certificate's key. The fields are only refilled at the end, on `Ok`.
  verified_ = false;
  nodeId_[0] = '\0';
  serial_[0] = '\0';
  std::memset(publicKey_, 0, sizeof(publicKey_));
  notBeforeMs_ = 0;
  notAfterMs_ = 0;

  if (certificateJson == nullptr || certificateJson[0] == '\0') {
    return CertVerifyStatus::NoCertificate;
  }
  if (!hasCaKey()) return CertVerifyStatus::NoCaKey;
  if (nodeId == nullptr || nodeId[0] == '\0') return CertVerifyStatus::Malformed;

  // The signature and the CA key travel inside the document they describe, so both are read
  // before the canonical body is rebuilt without them.
  // 128 hex characters for a 64-byte Ed25519 signature, plus the terminator. 80 was
  // half of what it needed, which made every certificate look malformed.
  char signatureHex[132];
  if (!jsonField(certificateJson, "signature", signatureHex, sizeof(signatureHex)) ||
      signatureHex[0] == '\0') {
    return CertVerifyStatus::Malformed;
  }

  // Rebuild the canonical body: sorted keys, no whitespace, excluding signature and
  // ca_public_key. `site_id` is written as `null` when the certificate has none, because that
  // is what `json.dumps` produced when it was signed.
  // Rebuild the canonical body: sorted keys, no whitespace, excluding signature and
  // ca_public_key. `site_id` is written as `null` when the certificate has none, because that
  // is what `json.dumps` produced when it was signed.
  //
  // 512 bytes is the largest a certificate can be and still fit every field: the header is 50,
  // `public_key` is 64, and the fixed keys are about 120. So the buffer is comfortable for a
  // real certificate and *tight* for a hostile one - which is the point, because every append
  // below is checked rather than trusted.
  char body[512];
  size_t used = 0;
  if (!appendChar(body, sizeof(body), used, '{')) {
    return CertVerifyStatus::Malformed;
  }

  char site[24];
  if (!jsonField(certificateJson, "site_id", site, sizeof(site))) {
    return CertVerifyStatus::Malformed;
  }
  const bool siteIsNull = (std::strcmp(site, "null") == 0);

  for (size_t i = 0; i < kBodyKeyCount; ++i) {
    if (i > 0 && !appendChar(body, sizeof(body), used, ',')) {
      return CertVerifyStatus::Malformed;
    }
    const char* key = kBodyKeys[i];
    if (std::strcmp(key, "site_id") == 0) {
      const bool ok = siteIsNull
                          ? appendFormatted(body, sizeof(body), used, "\"site_id\":null")
                          : appendFormatted(body, sizeof(body), used, "\"site_id\":\"%s\"", site);
      if (!ok) return CertVerifyStatus::Malformed;
      continue;
    }
    // 80, not 64: public_key is 64 hex characters and needs a terminator. A 64-byte buffer
    // is one byte short, which returned false and made every certificate look malformed.
    char value[80];
    if (!jsonField(certificateJson, key, value, sizeof(value))) {
      return CertVerifyStatus::Malformed;
    }
    // Numeric keys are written bare; everything else is a JSON string. Getting this backwards
    // produces a body that differs from the signed one, which fails as a bad signature - a
    // correct-looking error for a wrong reason.
    const bool numeric =
        (std::strcmp(key, "not_after_utc_ms") == 0) ||
        (std::strcmp(key, "not_before_utc_ms") == 0) || (std::strcmp(key, "version") == 0);
    const bool ok =
        numeric ? appendFormatted(body, sizeof(body), used, "\"%s\":%s", key, value)
                : appendFormatted(body, sizeof(body), used, "\"%s\":\"%s\"", key, value);
    if (!ok) return CertVerifyStatus::Malformed;
  }
  if (!appendChar(body, sizeof(body), used, '}')) {
    return CertVerifyStatus::Malformed;
  }
  body[used] = '\0';

  uint8_t signature[64];
  size_t sigLen = 0;
  for (size_t i = 0; i < 64 && signatureHex[2 * i] != '\0'; ++i) {
    int hi = 0;
    int lo = 0;
    if (!hexNibble(signatureHex[2 * i], hi) || !hexNibble(signatureHex[2 * i + 1], lo)) {
      return CertVerifyStatus::Malformed;
    }
    signature[i] = static_cast<uint8_t>((hi << 4) | lo);
    ++sigLen;
  }
  if (sigLen != 64) return CertVerifyStatus::Malformed;

  uint8_t caKey[32];
  if (!decodeHex32(caPublicKey_, caKey)) return CertVerifyStatus::NoCaKey;

  // The signature is checked before anything else, because without it none of the other fields
  // mean anything.
  if (!ed25519Verify(caKey, reinterpret_cast<const uint8_t*>(body), used, signature)) {
    return CertVerifyStatus::BadCaSignature;
  }

  // The signature is good. Now: is this certificate about *us*?
  char certNode[16];
  if (!jsonField(certificateJson, "node_id", certNode, sizeof(certNode))) {
    return CertVerifyStatus::Malformed;
  }
  if (std::strcmp(certNode, nodeId) != 0) {
    return CertVerifyStatus::WrongNode;
  }

  uint64_t notBefore = 0;
  uint64_t notAfter = 0;
  if (!jsonUintField(certificateJson, "not_before_utc_ms", notBefore) ||
      !jsonUintField(certificateJson, "not_after_utc_ms", notAfter)) {
    return CertVerifyStatus::Malformed;
  }
  // Reported separately from the signature, because "expired" and "forged" are different
  // problems: one is a clock or a stale certificate, the other is an attack.
  if (nowUtcMs < notBefore) return CertVerifyStatus::NotYetValid;
  if (nowUtcMs > notAfter) return CertVerifyStatus::Expired;

  char publicHex[65];
  if (!jsonField(certificateJson, "public_key", publicHex, sizeof(publicHex))) {
    return CertVerifyStatus::Malformed;
  }
  if (!decodeHex32(publicHex, publicKey_)) return CertVerifyStatus::Malformed;

  (void)jsonField(certificateJson, "serial", serial_, sizeof(serial_));
  std::snprintf(nodeId_, sizeof(nodeId_), "%s", certNode);
  notBeforeMs_ = notBefore;
  notAfterMs_ = notAfter;
  verified_ = true;
  return CertVerifyStatus::Ok;
}

}  // namespace cauce::app