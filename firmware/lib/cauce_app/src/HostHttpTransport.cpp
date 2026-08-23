#include "cauce/app/HostHttpTransport.h"

#ifndef ARDUINO

#include <cstdio>
#include <cstring>
#include <cstdlib>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using SocketHandle = SOCKET;
static constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
static constexpr SocketHandle kInvalidSocket = -1;
#endif

namespace cauce::app {

namespace {

bool httpDebug() {
  static const bool on = std::getenv("CAUCE_DEBUG_HTTP") != nullptr;
  return on;
}

struct UrlParts {
  std::string host;
  std::string port;
  std::string path;
};

bool parseUrl(const std::string& url, UrlParts& out) {
  const std::string scheme = "http://";
  const size_t start = url.find(scheme);
  if (start != 0) return false;
  const std::string rest = url.substr(scheme.size());
  const size_t slash = rest.find('/');
  const std::string hostPort =
      slash == std::string::npos ? rest : rest.substr(0, slash);
  out.path = slash == std::string::npos ? "/" : rest.substr(slash);
  const size_t colon = hostPort.find(':');
  if (colon == std::string::npos) {
    out.host = hostPort;
    out.port = "80";
  } else {
    out.host = hostPort.substr(0, colon);
    out.port = hostPort.substr(colon + 1);
  }
  return !out.host.empty();
}

void closeSocket(SocketHandle s) {
#ifdef _WIN32
  closesocket(s);
#else
  ::close(s);
#endif
}

}  // namespace

void HostHttpTransport::configure(const char* serverUrl, const char* bearerToken) {
  url_ = serverUrl ? serverUrl : "";
  token_ = bearerToken ? bearerToken : "";
}

hal::ISyncTransport::Result HostHttpTransport::postBatch(
    const char* jsonPayload, size_t length, const char* signatureHex,
    uint32_t timeoutMs, uint32_t& ackedSequenceOut) {
  if (httpDebug())
    std::fprintf(stderr, "[http] POST %zu bytes -> %s\n", length, url_.c_str());
#ifdef _WIN32
  static bool wsaReady = false;
  if (!wsaReady) {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return Result::NetworkError;
    wsaReady = true;
  }
#endif

  UrlParts parts;
  if (!parseUrl(url_, parts)) return Result::NetworkError;

  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* result = nullptr;
  if (getaddrinfo(parts.host.c_str(), parts.port.c_str(), &hints, &result) != 0 ||
      !result) {
    return Result::NetworkError;
  }

  SocketHandle sock = socket(result->ai_family, result->ai_socktype,
                             result->ai_protocol);
  if (sock == kInvalidSocket) {
    freeaddrinfo(result);
    return Result::NetworkError;
  }

#ifdef _WIN32
  DWORD timeout = timeoutMs;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
             sizeof(timeout));
  setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout),
             sizeof(timeout));
#else
  timeval tv{};
  tv.tv_sec = timeoutMs / 1000;
  tv.tv_usec = (timeoutMs % 1000) * 1000;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

  if (connect(sock, result->ai_addr, static_cast<int>(result->ai_addrlen)) != 0) {
    if (httpDebug()) std::fprintf(stderr, "[http] connect fallo host=%s port=%s\n", parts.host.c_str(), parts.port.c_str());
    freeaddrinfo(result);
    closeSocket(sock);
    return Result::NetworkError;
  }
  freeaddrinfo(result);

  std::string request;
  request.reserve(length + 256);
  request += "POST " + parts.path + " HTTP/1.1\r\n";
  request += "Host: " + parts.host + ":" + parts.port + "\r\n";
  request += "Content-Type: application/json\r\n";
  if (!token_.empty()) request += "Authorization: Bearer " + token_ + "\r\n";
  if (signatureHex && signatureHex[0]) {
    request += "X-CAUCE-Signature: ";
    request += signatureHex;
    request += "\r\n";
  }
  request += "Content-Length: " + std::to_string(length) + "\r\n";
  request += "Connection: close\r\n\r\n";
  request.append(jsonPayload, length);

  size_t sentTotal = 0;
  while (sentTotal < request.size()) {
    const int sent = static_cast<int>(
        send(sock, request.data() + sentTotal,
             static_cast<int>(request.size() - sentTotal), 0));
    if (sent <= 0) {
      closeSocket(sock);
      return Result::NetworkError;
    }
    sentTotal += static_cast<size_t>(sent);
  }

  std::string response;
  char buffer[2048];
  while (response.size() < 1024 * 1024) {
    const int received = static_cast<int>(
        recv(sock, buffer, sizeof(buffer), 0));
    if (received <= 0) break;
    response.append(buffer, static_cast<size_t>(received));
  }
  closeSocket(sock);

  if (httpDebug()) std::fprintf(stderr, "[http] respuesta %zu bytes, inicio=%.24s\n", response.size(), response.c_str());
  if (response.rfind("HTTP/1.", 0) != 0) return Result::NetworkError;

  int statusCode = 0;
  {
    const size_t sp1 = response.find(' ');
    statusCode = std::atoi(response.c_str() + sp1 + 1);
  }

  switch (statusCode) {
    case 200: {
      const size_t bodyStart = response.find("\r\n\r\n");
      const std::string body =
          bodyStart == std::string::npos
              ? ""
              : response.substr(bodyStart + 4);
      const std::string key = "\"acknowledged_sequence\":";
      const size_t keyPos = body.find(key);
      if (keyPos == std::string::npos) return Result::ServerError;
      ackedSequenceOut =
          static_cast<uint32_t>(std::strtoul(body.c_str() + keyPos + key.size(),
                                             nullptr, 10));
      return Result::Ok;
    }
    case 401:
    case 403:
      return Result::AuthFailed;
    case 422:
    case 409:
      return Result::Rejected;
    default:
      return Result::ServerError;
  }
}

}  // namespace cauce::app

#endif
