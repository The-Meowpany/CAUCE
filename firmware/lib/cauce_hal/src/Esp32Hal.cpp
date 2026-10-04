#include "cauce/hal/Esp32Hal.h"

#ifdef ARDUINO
#ifdef ARDUINO_ARCH_ESP32

#include <Arduino.h>
#include <LittleFS.h>
#include <time.h>

namespace cauce::hal {

Esp32Clock::Esp32Clock()
    : lastUtcMs_(0), anchorMonotonicMs_(0) {}

uint32_t Esp32Clock::monotonicMs() const { return millis(); }

uint64_t Esp32Clock::utcMs() const {
  struct timeval tv {};
  gettimeofday(&tv, nullptr);
  return static_cast<uint64_t>(tv.tv_sec) * 1000ULL +
         static_cast<uint64_t>(tv.tv_usec) / 1000ULL;
}

bool Esp32Clock::utcTimeValid() const {
  struct timeval tv {};
  gettimeofday(&tv, nullptr);
  if (tv.tv_sec < 1700000000) return false;
  return true;
}

void Esp32Clock::setUtcMs(uint64_t epochMs) {
  struct timeval tv {};
  tv.tv_sec = static_cast<time_t>(epochMs / 1000ULL);
  tv.tv_usec = static_cast<suseconds_t>((epochMs % 1000ULL) * 1000ULL);
settimeofday(&tv, nullptr);
      }

      void Esp32Clock::sleepMs(uint32_t durationMs) { delay(durationMs); }

Esp32WireBus::Esp32WireBus(int sdaPin, int sclPin, uint32_t frequencyHz)
    : sdaPin_(sdaPin), sclPin_(sclPin), frequencyHz_(frequencyHz) {}

bool Esp32WireBus::begin() {
  Wire.begin(static_cast<int>(sdaPin_), static_cast<int>(sclPin_),
             static_cast<uint32_t>(frequencyHz_));
  return true;
}

bool Esp32WireBus::writeRegisters(uint8_t deviceAddress, uint8_t startRegister,
                                  const uint8_t* data, size_t length) {
  Wire.beginTransmission(deviceAddress);
  Wire.write(startRegister);
  for (size_t i = 0; i < length; ++i) Wire.write(data[i]);
  return Wire.endTransmission() == 0;
}

bool Esp32WireBus::readRegisters(uint8_t deviceAddress, uint8_t startRegister,
                                 uint8_t* buffer, size_t length) {
  Wire.beginTransmission(deviceAddress);
  Wire.write(startRegister);
  if (Wire.endTransmission(false) != 0) return false;
  const size_t received = Wire.requestFrom(static_cast<int>(deviceAddress),
                                           static_cast<int>(length));
  if (received != length) return false;
  for (size_t i = 0; i < length; ++i) buffer[i] = Wire.read();
  return true;
}

bool Esp32WireBus::isPresent(uint8_t deviceAddress) {
  Wire.beginTransmission(deviceAddress);
  return Wire.endTransmission() == 0;
}

bool Esp32LittleFs::mount() { return LittleFS.begin(true); }

bool Esp32LittleFs::exists(const char* path) { return LittleFS.exists(path); }

bool Esp32LittleFs::appendBytes(const char* path, const uint8_t* data,
                                size_t length) {
  File file = LittleFS.open(path, FILE_APPEND);
  if (!file) return false;
  const size_t written = file.write(data, length);
  file.close();
  return written == length;
}

bool Esp32LittleFs::readRange(const char* path, size_t offset, uint8_t* buffer,
                              size_t length) {
  File file = LittleFS.open(path, FILE_READ);
  if (!file) return false;
  const bool ok = file.seek(offset) && file.read(buffer, length) == length;
  file.close();
  return ok;
}

bool Esp32LittleFs::writeWholeFile(const char* path, const uint8_t* data,
                                   size_t length) {
  File file = LittleFS.open(path, FILE_WRITE);
  if (!file) return false;
  const size_t written = file.write(data, length);
  file.close();
  return written == length;
}

size_t Esp32LittleFs::fileSize(const char* path) {
  File file = LittleFS.open(path, FILE_READ);
  if (!file) return 0;
  const size_t size = file.size();
  file.close();
  return size;
}

bool Esp32LittleFs::removeFile(const char* path) { return LittleFS.remove(path); }

int Esp32LittleFs::listFiles(const char* directory, char (*outPaths)[64],
                             int maxItems) {
  int count = 0;
  File dir = LittleFS.open(directory, FILE_READ);
  if (!dir || !dir.isDirectory()) return 0;

  // ITERATIONS ARE BOUNDED SEPARATELY FROM EMITTED FILES.
  //
  // Two ways this loop used to hang forever on real hardware, both invisible to the
  // host suite because this file is behind an #ifdef and every host test uses a
  // fake filesystem:
  //
  // 1. `count` only advances for a real file, so a directory containing only
  //    subdirectories or dot entries never reached maxItems and iterated forever.
  //    Bounding the emitted count does not bound the iterations.
  // 2. Calling entry.close() before dir.openNextFile() makes arduino-esp32 return
  //    the SAME entry again, which turns any non-empty directory into an infinite
  //    loop. The symptom on hardware was a task-watchdog reset with no panic
  //    backtrace, every nine seconds, with 274 host tests green - because the loop
  //    never yielded and the idle task is what the watchdog was watching.
  //
  // entry is reassigned, not closed: the File destructor and the assignment handle
  // cleanup, and calling close() here is what broke the iterator.
  const int kMaxIterations = 512;
  int iterations = 0;
  File entry = dir.openNextFile();
  while (entry && count < maxItems && iterations < kMaxIterations) {
    ++iterations;
    // File::name() returns const char* on arduino-esp32 2.x and String on 3.x.
    // Wrapping in String compiles against both without a version check.
    const String name = String(entry.name());
    const char* raw = name.c_str();
    const bool isDot = raw[0] == '.' &&
                       (raw[1] == '\0' || (raw[1] == '.' && raw[2] == '\0'));
    if (!isDot && !entry.isDirectory() && raw[0] != '\0') {
      snprintf(outPaths[count], 64, "%s/%s", directory, raw);
      ++count;
    }
    entry = dir.openNextFile();
  }
  dir.close();
  return count;
}

}  // namespace cauce::hal

#endif
#endif
