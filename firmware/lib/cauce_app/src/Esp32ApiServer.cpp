#include "cauce/app/Esp32ApiServer.h"

#ifdef ARDUINO
#ifdef ARDUINO_ARCH_ESP32

#include <Arduino.h>

namespace cauce::app {

namespace {
constexpr const char* kCORS = "Access-Control-Allow-Origin: *";
}  // namespace

Esp32ApiServer::Esp32ApiServer(WebServer& server, ApiRouter& router)
    : server_(server), router_(router) {}

void Esp32ApiServer::begin() {
  auto handler = [this]() {
    const String uri = server_.uri();
    const String methodStr =
        server_.method() == HTTP_GET
            ? String("GET")
            : (server_.method() == HTTP_POST ? String("POST") : String("?"));

    ApiRouter::Request req;
    req.method = methodStr.c_str();
    req.target = uri.c_str();
    if (server_.hasArg("plain")) {
      req.body = server_.arg("plain").c_str();
      req.bodyLen = strlen(req.body);
    }
    if (server_.hasHeader("Authorization")) {
      lastAuth_ = server_.header("Authorization");
      req.authorization = lastAuth_.c_str();
    }

    ApiRouter::Response response =
        router_.handle(req, buffer_, sizeof(buffer_));

    server_.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server_.send(response.statusCode, response.contentType, "");
    server_.sendHeader("Access-Control-Allow-Origin", "*");
    server_.sendContent(buffer_, response.bytesWritten);

    int guard = 0;
    while (!response.streamDone && guard++ < 10000) {
      response = router_.continueStream(buffer_, sizeof(buffer_));
      if (response.bytesWritten == 0 && response.streamDone) break;
      server_.sendContent(buffer_, response.bytesWritten);
    }
    server_.sendContent("", 0);
  };

  server_.on("/", HTTP_GET, handler);
  server_.on("/index.html", HTTP_GET, handler);
  server_.on("/favicon.ico", HTTP_GET, handler);
  server_.on(Uri(std::string("/api/v1/node").c_str()), HTTP_GET, handler);
  server_.on(Uri(std::string("/api/v1/status").c_str()), HTTP_GET, handler);
  server_.on(Uri(std::string("/api/v1/measurements/latest").c_str()), HTTP_GET,
             handler);
  server_.on(Uri(std::string("/api/v1/health").c_str()), HTTP_GET, handler);
  server_.on(Uri(std::string("/api/v1/config").c_str()),
             HTTP_GET, handler);
  server_.on(Uri(std::string("/api/v1/config").c_str()), HTTP_POST, handler);
  server_.on(Uri(std::string("/api/v1/measurements").c_str()), HTTP_GET,
             handler);
  server_.on(Uri(std::string("/api/v1/export").c_str()), HTTP_GET, handler);
  server_.onNotFound(handler);
  server_.begin();
}

void Esp32ApiServer::handleClient() { server_.handleClient(); }

}  // namespace cauce::app

#endif
#endif
