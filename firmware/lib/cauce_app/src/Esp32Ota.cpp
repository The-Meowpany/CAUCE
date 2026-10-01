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

bool Esp32FirmwareReader::open(const char* url) {
  close();
  if (!url || url[0] == '\0') return false;
  http_.setTimeout(15000);
  if (!http_.begin(url)) return false;
  const int code = http_.GET();
  if (code != 200) {
    http_.end();
    return false;
  }
  stream_ = http_.getStreamPtr();
  return stream_ != nullptr;
}

size_t Esp32FirmwareReader::read(uint8_t* buffer, size_t capacity) {
  if (!stream_ || !buffer || capacity == 0) return 0;
  uint32_t waited = 0;
  while (!stream_->available() && waited < 10000) {
    delay(50);
    waited += 50;
  }
  if (!stream_->available()) return 0;
  return stream_->readBytes(buffer, capacity);
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
