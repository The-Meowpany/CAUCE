#pragma once

#ifdef ARDUINO
#ifdef ARDUINO_ARCH_ESP32

#include <WebServer.h>

#include "cauce/app/ApiRouter.h"
#include "cauce/app/SystemHealth.h"

namespace cauce::app {

class Esp32ApiServer {
 public:
  static constexpr size_t kResponseCapacity = 4096;

  Esp32ApiServer(WebServer& server, ApiRouter& router);

  void begin();
  void handleClient();

 private:
  void sendCurrentChunk(bool finalChunk);

  WebServer& server_;
  ApiRouter& router_;
  char buffer_[kResponseCapacity];
  String lastAuth_;
};

}  // namespace cauce::app

#endif
#endif
