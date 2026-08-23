#include <cstdio>
#include <cstring>
#include <string>

#include <unity.h>

#include "cauce/app/ApiRouter.h"
#include "cauce/app/NetworkManager.h"
#include "cauce/core/LogStorageRepository.h"
#include "cauce/core/Logger.h"
#include "cauce/core/SecurityUtils.h"
#include "cauce/hal/MemoryFileSystem.h"
#include "cauce/hal/ManualClock.h"

using namespace cauce;
using namespace cauce::app;

namespace {

hal::MemoryFileSystem fs;
hal::ManualClock clock_(1787356800000ULL);

void netClockReset() { clock_ = hal::ManualClock(1787356800000ULL); }

void wipeDirApi() {
  char paths[16][64];
  int n = fs.listFiles("data_api", paths, 16);
  for (int i = 0; i < n; ++i) fs.removeFile(paths[i]);
  n = fs.listFiles("/api_test.conf", paths, 2);
  for (int i = 0; i < n; ++i) fs.removeFile(paths[i]);
}

struct SilentSink final : ILogSink {
  void writeLine(const char*) override {}
} sink;

const char* kConfigPath = "/api_test.conf";

Measurement makeM(uint32_t seq, uint64_t tsMs, Variable var, float value) {
  Measurement m{};
  copyString(m.nodeId, sizeof(m.nodeId), "CAUCE-001");
  copyString(m.sensorId, sizeof(m.sensorId), "BME280-1");
  m.sequence = seq;
  m.timestampUtcMs = tsMs;
  m.variable = var;
  m.value = value;
  m.quality = Quality::Valid;
  m.timeUncertain = false;
  return m;
}

struct Rig {
  LogStorageRepository store{fs, "data_api"};
  Logger logger{sink};
  ConfigManager config{fs, kConfigPath};
  SystemHealth health{};
  ApiRouter router{store, config, clock_, logger, health};
};

std::string bodyOf(ApiRouter::Response r, const char* buf) {
  return std::string(buf, r.bytesWritten);
}

}  // namespace

void test_unknown_route_returns_404() {
  netClockReset();
  wipeDirApi();
  Rig rig;
  char out[512];
  ApiRouter::Request req;
  req.target = "/api/v1/nope";
  const auto r = rig.router.handle(req, out, sizeof(out));
  TEST_ASSERT_EQUAL_UINT16(404, r.statusCode);
}

void test_node_endpoint_reports_identity() {
  netClockReset();
  wipeDirApi();
  Rig rig;
  NodeConfig cfg;
  copyString(cfg.nodeId, sizeof(cfg.nodeId), "CAUCE-042");
  cfg.latitude = -34.5f;
  rig.config.save(cfg);
  rig.config.load(cfg);

  char out[512];
  ApiRouter::Request req;
  req.target = "/api/v1/node";
  const auto r = rig.router.handle(req, out, sizeof(out));
  TEST_ASSERT_EQUAL_UINT16(200, r.statusCode);
  const std::string body = bodyOf(r, out);
  TEST_ASSERT_TRUE(body.find("\"node_id\":\"CAUCE-042\"") != std::string::npos);
  TEST_ASSERT_TRUE(body.find("\"protocol_version\":1") != std::string::npos);
  TEST_ASSERT_TRUE(body.find("-34.5") != std::string::npos);
}

void test_latest_returns_404_when_empty_and_record_when_present() {
  netClockReset();
  wipeDirApi();
  Rig rig;
  char out[640];

  ApiRouter::Request empty;
  empty.target = "/api/v1/measurements/latest";
  auto r = rig.router.handle(empty, out, sizeof(out));
  TEST_ASSERT_EQUAL_UINT16(404, r.statusCode);

  rig.store.open();
  rig.store.append(makeM(7, 1787356860000ULL, Variable::AirTemperature, 22.75f));

  ApiRouter::Request req;
  req.target = "/api/v1/measurements/latest";
  r = rig.router.handle(req, out, sizeof(out));
  TEST_ASSERT_EQUAL_UINT16(200, r.statusCode);
  const std::string body = bodyOf(r, out);
  TEST_ASSERT_TRUE(body.find("\"sequence\":7") != std::string::npos);
  TEST_ASSERT_TRUE(body.find("\"value\":22.75") != std::string::npos);
  TEST_ASSERT_TRUE(body.find("2026-08-22T00:01:00Z") != std::string::npos);
}

void test_measurements_streams_json_array() {
  netClockReset();
  wipeDirApi();
  Rig rig;
  rig.store.open();
  for (uint32_t i = 1; i <= 20; ++i) {
    rig.store.append(makeM(i, 1787356800000ULL + i * 60000ULL,
                           Variable::AirTemperature, 20.0f + i));
  }

  char out[512];
  ApiRouter::Request req;
  req.target = "/api/v1/measurements";
  std::string all;
  auto r = rig.router.handle(req, out, sizeof(out));
  TEST_ASSERT_EQUAL_UINT16(200, r.statusCode);
  TEST_ASSERT_TRUE(r.streamActive);
  all.append(out, r.bytesWritten);
  int guard = 0;
  while (!r.streamDone && guard++ < 50) {
    r = rig.router.continueStream(out, sizeof(out));
    all.append(out, r.bytesWritten);
  }
  TEST_ASSERT_TRUE(r.streamDone);
  TEST_ASSERT_EQUAL_INT(0, static_cast<int>(all.find('[')));
  TEST_ASSERT_TRUE(all.back() == ']');
  TEST_ASSERT_TRUE(all.find("\"sequence\":20\"") == std::string::npos);
  TEST_ASSERT_TRUE(all.find("\"sequence\":20,") != std::string::npos);
}

void test_export_csv_content_type_and_header() {
  netClockReset();
  wipeDirApi();
  Rig rig;
  rig.store.open();
  rig.store.append(makeM(1, 1787356860000ULL, Variable::Pressure, 1013.2f));

  char out[512];
  ApiRouter::Request req;
  req.target = "/api/v1/export?format=csv";
  const auto r = rig.router.handle(req, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("text/csv", r.contentType);
  const std::string body = bodyOf(r, out);
  TEST_ASSERT_TRUE(body.find("node_id,sensor_id,sequence") == 0);
}

void test_export_rejects_bad_format() {
  netClockReset();
  wipeDirApi();
  Rig rig;
  char out[512];
  ApiRouter::Request req;
  req.target = "/api/v1/export?format=xml";
  const auto r = rig.router.handle(req, out, sizeof(out));
  TEST_ASSERT_EQUAL_UINT16(400, r.statusCode);
}

void test_health_endpoint_counts() {
  netClockReset();
  wipeDirApi();
  Rig rig;
  rig.health.measurementCount = 123;
  rig.health.invalidCount = 4;
  rig.health.nodeState = NodeState::Ready;
  rig.health.netState = NetState::Connected;

  char out[768];
  ApiRouter::Request req;
  req.target = "/api/v1/health";
  const auto r = rig.router.handle(req, out, sizeof(out));
  const std::string body = bodyOf(r, out);
  TEST_ASSERT_TRUE(body.find("\"measurements\":123") != std::string::npos);
  TEST_ASSERT_TRUE(body.find("\"invalid\":4") != std::string::npos);
  TEST_ASSERT_TRUE(body.find("\"net_state\":\"CONNECTED\"") != std::string::npos);
}

void test_post_config_requires_token_fail_closed_when_unset() {
  netClockReset();
  wipeDirApi();
  Rig rig;
  char out[512];

  ApiRouter::Request req;
  req.method = "POST";
  req.target = "/api/v1/config";
  req.body = "sampling_interval_s=120\n";
  req.bodyLen = strlen(req.body);

  auto r = rig.router.handle(req, out, sizeof(out));
  TEST_ASSERT_EQUAL_UINT16(503, r.statusCode);

  req.authorization = "Bearer deadbeef";
  r = rig.router.handle(req, out, sizeof(out));
  TEST_ASSERT_EQUAL_UINT16(503, r.statusCode);
}

void test_post_config_with_valid_token_applies() {
  netClockReset();
  wipeDirApi();
  Rig rig;

  NodeConfig initial{};
  char tokenHash[65];
  sha256Hex("secreto-cauce", tokenHash);
  copyString(initial.adminTokenSha256, sizeof(initial.adminTokenSha256),
             tokenHash);
  rig.config.save(initial);

  char auth[96];
  std::snprintf(auth, sizeof(auth), "Bearer %s", "secreto-cauce");

  char out[512];
  ApiRouter::Request req;
  req.method = "POST";
  req.target = "/api/v1/config";
  req.body = "sampling_interval_s=180\nnode_id=CAUCE-777\n";
  req.bodyLen = strlen(req.body);
  req.authorization = auth;

  const auto r = rig.router.handle(req, out, sizeof(out));
  TEST_ASSERT_EQUAL_UINT16(200, r.statusCode);

  NodeConfig reloaded{};
  rig.config.load(reloaded);
  TEST_ASSERT_EQUAL_UINT32(180, reloaded.samplingIntervalS);
  TEST_ASSERT_EQUAL_STRING("CAUCE-777", reloaded.nodeId);
}

void test_post_config_wrong_token_401() {
  netClockReset();
  wipeDirApi();
  Rig rig;
  NodeConfig initial{};
  char tokenHash[65];
  sha256Hex("correcto", tokenHash);
  copyString(initial.adminTokenSha256, sizeof(initial.adminTokenSha256),
             tokenHash);
  rig.config.save(initial);

  char out[512];
  ApiRouter::Request req;
  req.method = "POST";
  req.target = "/api/v1/config";
  req.body = "sampling_interval_s=999\n";
  req.bodyLen = strlen(req.body);
  req.authorization = "Bearer incorrecto";
  const auto r = rig.router.handle(req, out, sizeof(out));
  TEST_ASSERT_EQUAL_UINT16(401, r.statusCode);

  NodeConfig unchanged{};
  rig.config.load(unchanged);
  TEST_ASSERT_EQUAL_UINT32(60, unchanged.samplingIntervalS);
}

void test_post_config_invalid_values_422_with_errors() {
  netClockReset();
  wipeDirApi();
  Rig rig;
  NodeConfig initial{};
  char tokenHash[65];
  sha256Hex("tok", tokenHash);
  copyString(initial.adminTokenSha256, sizeof(initial.adminTokenSha256),
             tokenHash);
  rig.config.save(initial);

  char out[1024];
  ApiRouter::Request req;
  req.method = "POST";
  req.target = "/api/v1/config";
  req.body = "sampling_interval_s=2\n";
  req.bodyLen = strlen(req.body);
  req.authorization = "Bearer tok";

  const auto r = rig.router.handle(req, out, sizeof(out));
  TEST_ASSERT_EQUAL_UINT16(422, r.statusCode);
  const std::string body = bodyOf(r, out);
  TEST_ASSERT_TRUE(body.find("\"errors\":[") != std::string::npos);
  TEST_ASSERT_TRUE(body.find("sampling_interval_s") != std::string::npos);
}

void test_measurements_from_filter_iso8601() {
  netClockReset();
  wipeDirApi();
  Rig rig;
  rig.store.open();
  const uint64_t t0 = 1787356800000ULL;
  for (uint32_t i : {1u, 2u, 3u}) {
    rig.store.append(makeM(i, t0 + i * 3600000ULL, Variable::Light, 100.0f * i));
  }

  char out[512];
  ApiRouter::Request req;
  req.target =
      "/api/v1/measurements?from=2026-08-22T01%3A30%3A00Z";
  auto r = rig.router.handle(req, out, sizeof(out));
  std::string all;
  all.append(out, r.bytesWritten);
  int guard = 0;
  while (!r.streamDone && guard++ < 50) {
    r = rig.router.continueStream(out, sizeof(out));
    all.append(out, r.bytesWritten);
  }
  TEST_ASSERT_TRUE(all.find("\"value\":100.00") == std::string::npos);
  TEST_ASSERT_TRUE(all.find("\"value\":200.00") != std::string::npos);
  TEST_ASSERT_TRUE(all.find("\"value\":300.00") != std::string::npos);
}


void test_root_serves_dashboard_html_in_chunks() {
  netClockReset();
  wipeDirApi();
  Rig rig;
  char out[512];
  ApiRouter::Request req;
  req.target = "/";
  std::string all;
  auto r = rig.router.handle(req, out, sizeof(out));
  TEST_ASSERT_EQUAL_UINT16(200, r.statusCode);
  TEST_ASSERT_EQUAL_STRING("text/html; charset=utf-8", r.contentType);
  TEST_ASSERT_TRUE(r.streamActive);
  all.append(out, r.bytesWritten);
  int guard = 0;
  while (!r.streamDone && guard++ < 200) {
    r = rig.router.continueStream(out, sizeof(out));
    all.append(out, r.bytesWritten);
  }
  TEST_ASSERT_TRUE(r.streamDone);
  TEST_ASSERT_TRUE(all.size() > 4000);
  TEST_ASSERT_TRUE(all.rfind("<!DOCTYPE", 0) == 0);
  TEST_ASSERT_TRUE(all.find("</html>") != std::string::npos);
  TEST_ASSERT_TRUE(all.find("api/v1/measurements") != std::string::npos);
}

void test_favicon_returns_204() {
  netClockReset();
  wipeDirApi();
  Rig rig;
  char out[512];
  ApiRouter::Request req;
  req.target = "/favicon.ico";
  const auto r = rig.router.handle(req, out, sizeof(out));
  TEST_ASSERT_EQUAL_UINT16(204, r.statusCode);
  TEST_ASSERT_EQUAL_UINT(0, r.bytesWritten);
}
void registerApiTests() {
  UNITY_BEGIN();
  RUN_TEST(test_unknown_route_returns_404);
  RUN_TEST(test_node_endpoint_reports_identity);
  RUN_TEST(test_latest_returns_404_when_empty_and_record_when_present);
  RUN_TEST(test_measurements_streams_json_array);
  RUN_TEST(test_export_csv_content_type_and_header);
  RUN_TEST(test_export_rejects_bad_format);
  RUN_TEST(test_health_endpoint_counts);
  RUN_TEST(test_post_config_requires_token_fail_closed_when_unset);
  RUN_TEST(test_post_config_with_valid_token_applies);
  RUN_TEST(test_post_config_wrong_token_401);
  RUN_TEST(test_post_config_invalid_values_422_with_errors);
  RUN_TEST(test_measurements_from_filter_iso8601);
  RUN_TEST(test_root_serves_dashboard_html_in_chunks);
  RUN_TEST(test_favicon_returns_204);
  UNITY_END();
}
