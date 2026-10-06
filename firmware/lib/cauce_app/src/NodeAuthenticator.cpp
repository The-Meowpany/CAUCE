#include "cauce/app/NodeAuthenticator.h"

#include <cstdio>
#include <cstring>

#include "cauce/core/Types.h"

namespace cauce::app {
namespace {

constexpr char kBase64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

}  // namespace

void NodeAuthenticator::setCredential(const uint8_t seed[kEd25519SeedBytes],
                                      const char* certificateJson) {
  if (!seed || !certificateJson) {
    clearCredential();
    return;
  }
  std::memcpy(seed_, seed, kEd25519SeedBytes);
  copyString(certificate_, sizeof(certificate_), certificateJson);
  // The certificate may be empty while the seed is valid. Both are required, and checking
  // it here means `isConfigured` is a real answer rather than a hopeful one.
  hasSeed_ = true;
}

void NodeAuthenticator::setIdentity(const char* baseUrl, const char* nodeId) {
  copyString(baseUrl_, sizeof(baseUrl_), baseUrl ? baseUrl : "");
  copyString(nodeId_, sizeof(nodeId_), nodeId ? nodeId : "");
}

void NodeAuthenticator::clearCredential() {
  // Zeroed rather than left in place: this is the private half of the node's identity, and
  // the only reason to clear it is that it is no longer valid.
  std::memset(seed_, 0, sizeof(seed_));
  certificate_[0] = '\0';
  hasSeed_ = false;
}

size_t NodeAuthenticator::canonicalChallengeBytes(const char* nodeId, const char* nonce,
                                                  uint64_t expiresUtcMs, char* out,
                                                  size_t capacity) {
  if (!out || capacity == 0 || !nodeId || !nonce) return 0;
  // `%llu` for a uint64_t. The backend formats the same field with Python's `str(int)`,
  // which is decimal with no separators - so the two agree only if neither pads and
  // neither uses a thousands separator.
  const int n = std::snprintf(out, capacity, "cauce-sync-challenge\n%s\n%s\n%llu", nodeId,
                              nonce, static_cast<unsigned long long>(expiresUtcMs));
  if (n <= 0 || static_cast<size_t>(n) >= capacity) {
    if (capacity > 0) out[0] = '\0';
    return 0;
  }
  return static_cast<size_t>(n);
}

size_t NodeAuthenticator::base64Encode(const uint8_t* data, size_t length, char* out,
                                       size_t capacity) {
  if (!out || capacity == 0) return 0;
  out[0] = '\0';
  if (!data && length > 0) return 0;
  if (length == 0) return 0;

  // 4 output characters per 3 input bytes, plus a terminator.
  const size_t needed = ((length + 2) / 3) * 4 + 1;
  if (capacity < needed) return 0;

  size_t written = 0;
  size_t i = 0;
  while (i + 2 < length) {
    const uint32_t triple =
        (static_cast<uint32_t>(data[i]) << 16) |
        (static_cast<uint32_t>(data[i + 1]) << 8) | static_cast<uint32_t>(data[i + 2]);
    out[written++] = kBase64Alphabet[(triple >> 18) & 0x3F];
    out[written++] = kBase64Alphabet[(triple >> 12) & 0x3F];
    out[written++] = kBase64Alphabet[(triple >> 6) & 0x3F];
    out[written++] = kBase64Alphabet[triple & 0x3F];
    i += 3;
  }

  const size_t remaining = length - i;
  if (remaining == 1) {
    const uint32_t triple = static_cast<uint32_t>(data[i]) << 16;
    out[written++] = kBase64Alphabet[(triple >> 18) & 0x3F];
    out[written++] = kBase64Alphabet[(triple >> 12) & 0x3F];
    out[written++] = '=';
    out[written++] = '=';
  } else if (remaining == 2) {
    const uint32_t triple =
        (static_cast<uint32_t>(data[i]) << 16) | (static_cast<uint32_t>(data[i + 1]) << 8);
    out[written++] = kBase64Alphabet[(triple >> 18) & 0x3F];
    out[written++] = kBase64Alphabet[(triple >> 12) & 0x3F];
    out[written++] = kBase64Alphabet[(triple >> 6) & 0x3F];
    out[written++] = '=';
  }

  out[written] = '\0';
  return written;
}

bool NodeAuthenticator::jsonStringField(const char* json, const char* key, char* out,
                                        size_t capacity) {
  if (!json || !key || !out || capacity == 0) return false;
  out[0] = '\0';
  char needle[32];
  const int n = std::snprintf(needle, sizeof(needle), "\"%s\"", key);
  if (n <= 0 || static_cast<size_t>(n) >= sizeof(needle)) return false;

  const char* at = json;
  // Only the first occurrence, and only when it is a value rather than a key. A later
  // occurrence of the same name would be inside some other object.
  while ((at = std::strstr(at, needle)) != nullptr) {
    const char* colon = at + n;
    while (*colon == ' ' || *colon == '\t') ++colon;
    if (*colon != ':') {
      at += n;
      continue;
    }
    ++colon;
    while (*colon == ' ' || *colon == '\t') ++colon;
    if (*colon != '"') {
      at += n;
      continue;
    }
    ++colon;
    size_t written = 0;
    // Decoded escapes. An earlier version refused every `\`, justified by a comment saying
    // the central emits none. It does: `json.dumps` writes the canonical string's three
    // newlines as three backslash-n pairs, and a strict refusal meant a node could never
    // read a real challenge. Only the escapes Python's `json.dumps` produces by default are
    // accepted, and `\u` is not one of them - see below.
    bool truncated = false;
    char decoded = '\0';
    while (*colon && *colon != '"') {
      if (*colon == '\\') {
        decoded = '\0';
        switch (colon[1]) {
          case 'n': decoded = '\n'; break;
          case 't': decoded = '\t'; break;
          case 'r': decoded = '\r'; break;
          case '"': decoded = '"'; break;
          case '\\': decoded = '\\'; break;
          // `\b`, `\f`, `\/`, and every `\uXXXX` are refused rather than guessed at. A
          // surrogate pair or a `\u0041` decoded to `A` would mean this module signs a
          // string the central never wrote, and it fails at the central as an invalid
          // signature, which points at the key instead of at the decoding. Refusing here
          // fails as NoChallenge, which points at the right place.
          //
          // `out` is cleared on this path as on every other failure. A caller that ignores the
          // false and reads the buffer would otherwise sign whatever prefix was decoded before
          // the escape appeared - here `cauce` out of `cauce\Xchallenge`.
          default:
            out[0] = '\0';
            return false;
        }
        colon += 2;
      } else {
        decoded = *colon++;
      }
      if (written + 1 >= capacity) {
        truncated = true;
        break;
      }
      out[written++] = decoded;
    }
    if (truncated) {
      out[0] = '\0';
      return false;
    }
    if (*colon != '"') return false;
    out[written] = '\0';
    return true;
  }
  return false;
}

bool NodeAuthenticator::jsonUintField(const char* json, const char* key, uint64_t& out) {
  if (!json || !key) return false;
  char needle[32];
  const int n = std::snprintf(needle, sizeof(needle), "\"%s\"", key);
  if (n <= 0 || static_cast<size_t>(n) >= sizeof(needle)) return false;
  const char* at = std::strstr(json, needle);
  if (!at) return false;
  const char* colon = at + n;
  while (*colon == ' ' || *colon == '\t') ++colon;
  if (*colon != ':') return false;
  ++colon;
  while (*colon == ' ' || *colon == '\t') ++colon;

  uint64_t value = 0;
  bool any = false;
  // Digits only. `strtoull` would accept a leading sign, a `0x` prefix and trailing text,
  // so `0x10` would become sixteen and a challenge would be signed over an expiry the
  // central never issued.
  while (*colon >= '0' && *colon <= '9') {
    value = value * 10 + static_cast<uint64_t>(*colon - '0');
    ++colon;
    any = true;
  }
  if (!any) return false;
  // Trailing non-space is a malformed document rather than something to skip past.
  while (*colon == ' ' || *colon == '\t' || *colon == '\r' || *colon == '\n') ++colon;
  if (*colon != '\0' && *colon != ',' && *colon != '}') return false;
  out = value;
  return true;
}

NodeAuthStatus NodeAuthenticator::authenticate(
    uint32_t timeoutMs, char* nodeIdOut, size_t nodeIdCap, char* certificateOut,
    size_t certificateCap, char* nonceOut, size_t nonceCap, char* signatureOut,
    size_t signatureCap) {
  // Every output is cleared first, unconditionally. A caller that ignores the returned
  // status then sends nothing rather than something that was half written.
  const auto clear = [](char* buffer, size_t capacity) {
    if (buffer && capacity > 0) buffer[0] = '\0';
  };
  clear(nodeIdOut, nodeIdCap);
  clear(certificateOut, certificateCap);
  clear(nonceOut, nonceCap);
  clear(signatureOut, signatureCap);

  if (!isConfigured()) {
    ++failures_;
    return NodeAuthStatus::NotConfigured;
  }
  if (!source_) {
    ++failures_;
    return NodeAuthStatus::Unreachable;
  }

  NodeChallenge challenge{};
  const hal::ISyncTransport::Result fetched =
      source_->fetchChallenge(baseUrl_, nodeId_, challenge);
  if (fetched != hal::ISyncTransport::Result::Ok) {
    // A source that filled nothing is the same failure as one that could not reach the
    // central: signing a zeroed nonce would produce a valid signature over the wrong
    // bytes, and the central would report an invalid signature instead of a network fault.
    if (challenge.signThis[0] == '\0') {
      ++failures_;
      return NodeAuthStatus::Unreachable;
    }
  }
  if (challenge.signThis[0] == '\0' || challenge.nonce[0] == '\0') {
    ++failures_;
    return NodeAuthStatus::NoChallenge;
  }

  uint8_t signature[kEd25519SignatureBytes];
  if (!ed25519Sign(signature, seed_,
                    reinterpret_cast<const uint8_t*>(challenge.signThis),
                    std::strlen(challenge.signThis))) {
    ++failures_;
    return NodeAuthStatus::SignFailed;
  }

  char encodedSignature[128];
  char encodedCertificate[1536];
  const size_t sigLen =
      base64Encode(signature, sizeof(signature), encodedSignature, sizeof(encodedSignature));
  const size_t certLen = base64Encode(
      reinterpret_cast<const uint8_t*>(certificate_), std::strlen(certificate_),
      encodedCertificate, sizeof(encodedCertificate));
  if (sigLen == 0 || certLen == 0) {
    ++failures_;
    return NodeAuthStatus::EncodeFailed;
  }

  // The buffers are the caller's, so a mismatch between what was produced and what was
  // offered is an error rather than a truncation.
  const auto fits = [](const char* text, char* out, size_t capacity) {
    return out && capacity > 0 && std::strlen(text) < capacity;
  };
  if (!fits(encodedSignature, signatureOut, signatureCap) ||
      !fits(encodedCertificate, certificateOut, certificateCap) ||
      !fits(challenge.nonce, nonceOut, nonceCap) ||
      !fits(challenge.nodeId, nodeIdOut, nodeIdCap)) {
    ++failures_;
    return NodeAuthStatus::EncodeFailed;
  }

  copyString(nodeIdOut, nodeIdCap, challenge.nodeId);
  copyString(certificateOut, certificateCap, encodedCertificate);
  copyString(nonceOut, nonceCap, challenge.nonce);
  copyString(signatureOut, signatureCap, encodedSignature);

  ++successes_;
  return NodeAuthStatus::Ok;
}

}  // namespace cauce::app