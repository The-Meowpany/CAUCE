#include "cauce/app/Esp32Ota.h"

#ifdef ARDUINO
#ifdef ARDUINO_ARCH_ESP32

#include <Arduino.h>
#include <Update.h>

#include <cstring>

#include "cauce/app/OtaJson.h"

namespace cauce::app {

Esp32ManifestSource::Esp32ManifestSource() {
  url_[0] = '\0';
  nodeId_[0] = '\0';
}

void Esp32ManifestSource::configure(const char* baseUrl, const char* nodeId) {
  if (baseUrl) {
    std::strncpy(url_, baseUrl, sizeof(url_) - 1);
    url_[sizeof(url_) - 1] = '\0';
  } else {
    url_[0] = '\0';
  }
  if (nodeId) {
    std::strncpy(nodeId_, nodeId, sizeof(nodeId_) - 1);
    nodeId_[sizeof(nodeId_) - 1] = '\0';
  } else {
    nodeId_[0] = '\0';
  }
}

bool Esp32ManifestSource::fetchLatest(const char*, OtaRelease& out) {
  if (url_[0] == '\0') return false;
  HTTPClient http;
  http.setTimeout(15000);
  String target(url_);
  if (nodeId_[0] != '\0') {
    target += "?node_id=";
    target += nodeId_;
  }
  if (!http.begin(target)) return false;
  const int code = http.GET();
  if (code != 200) {
    http.end();
    return false;
  }
  const String body = http.getString();
  http.end();
  if (body.length() == 0 || body.length() > 1024) return false;
  char buf[1025];
  body.toCharArray(buf, sizeof(buf));
  return parseOtaManifestJson(buf, out);
}

Esp32FirmwareReader::Esp32FirmwareReader() = default;

// WHY THIS IS NOT `http_.GET()`
//
// `GET()` is not "open the connection". It sends the request *and reads the entire response
// body into a String*, and the body here is a 1 MB firmware image. So the old `open()` blocked
// for the whole download before returning, and `read()` then re-read the same bytes from a
// buffer that was already in memory.
//
// Two costs, and the second is the one that bites. The caller saw an open that took as long as
// the download, so there was no way to time-box it or to report progress during it; and the
// whole image was resident on a device with a few hundred kilobytes of usable heap, which is a
// second way for the same download to reset the board.
//
// `sendRequest` sends the request and reads only the status line and headers, then returns. The
// body stays in the socket and `getStreamPtr()` hands it over incrementally, which is what
// `read()` was written to consume. The transfer is therefore bounded by the socket timeout and
// the OTA manager's watchdog feeds rather than by the size of the image.
//
// `begin()` is also worth noting: it is pure string parsing - `beginInternal` splits the URL and
// assigns `_host`, `_port` and `_uri` - with no DNS and no socket. The name suggests otherwise,
// and treating `begin()` as the blocking step is how the original design went looking for the
// cost in the wrong place.
bool Esp32FirmwareReader::open(const char* url) {
  close();
  if (!url || url[0] == '\0') return false;
  http_.setTimeout(15000);
  if (!http_.begin(url)) return false;
  // The transfer is chunked rather than reused across updates: a keep-alive socket held open
  // across a flash erase is a resource the erase cannot reclaim, and an OTA that reuses a
  // stale connection is a failure mode that only appears on the second update.
  http_.setReuse(false);
  const int code = http_.sendRequest("GET");
  if (code != 200) {
    http_.end();
    return false;
  }
  stream_ = http_.getStreamPtr();
  if (stream_ == nullptr) {
    http_.end();
    return false;
  }
  return true;
}

ReadStatus Esp32FirmwareReader::read(uint8_t* buffer, size_t capacity,
                                    size_t* bytesRead) {
  if (bytesRead != nullptr) *bytesRead = 0;
  if (!stream_ || !buffer || capacity == 0) return ReadStatus::Error;
  if (!stream_->available()) return ReadStatus::NoDataYet;
  const size_t n = stream_->readBytes(buffer, capacity);
  if (n == 0) return ReadStatus::Eof;
  if (bytesRead != nullptr) *bytesRead = n;
  return ReadStatus::Data;
}

void Esp32FirmwareReader::close() {
  http_.end();
  stream_ = nullptr;
}

bool Esp32FirmwareInstaller::beginInstall(uint32_t totalSize) {
  if (totalSize == 0) return false;
  return Update.begin(totalSize);
}

bool Esp32FirmwareInstaller::writeChunk(const uint8_t* data, size_t length) {
  if (!data || length == 0) return false;
  return Update.write(const_cast<uint8_t*>(data), length) == length;
}

InstallDecision Esp32FirmwareInstaller::finishInstall() {
  if (!Update.end(true)) return InstallDecision::Abort;
  return Update.isFinished() ? InstallDecision::Proceed
                             : InstallDecision::Abort;
}

void Esp32FirmwareInstaller::abortInstall() { Update.abort(); }

}  // namespace cauce::app

#endif
#endif
