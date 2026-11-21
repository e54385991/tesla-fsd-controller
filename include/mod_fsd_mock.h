#pragma once
// ── FSD payload interception (feature_configuration) ─────────────────────────
// ESP32 port of TeslaDG's fsd_mock.py + dns_sink.py hotspot chain:
//   DNS: api-prd.vn.cloud.tesla.cn -> ESP32 AP
//   443: any path ending in /feature_configuration -> payload.json + X-Checksum
//
// Only compiled into the ESP32-S3 Wi-Fi bridge build (needs esp_https_server).

#include <sdkconfig.h>

#if defined(WIFI_BRIDGE_ENABLED) && defined(CONFIG_IDF_TARGET_ESP32S3) && defined(CONFIG_ESP_HTTPS_SERVER_ENABLE)

#include <Arduino.h>
#include <esp_err.h>
#include <stddef.h>
#include <stdint.h>

// Forward declaration only: esp_http_server.h also defines HTTP_GET/HTTP_POST,
// which collide with ESPAsyncWebServer's enum in main.cpp's single TU.
struct httpd_req;
struct esp_tls;

namespace fsd_mock {

// Windows dashboard caps payload edits at 8192 bytes; keep the same limit.
constexpr size_t kPayloadCapacity = 8193;
constexpr size_t kHostCapacity = 96;
constexpr size_t kRecentHitCapacity = 16;

enum class Mode : uint8_t {
    Reply = 0,
    Observe = 1,
};

struct Hit {
    uint32_t uptimeMs = 0;
    char kind[8] = {};       // HIT / OBSERVE / FLOW
    char client[16] = {};
    char method[8] = {};
    char url[192] = {};
    char vin[18] = {};
    char reqMd5[33] = {};
    char reqHead[97] = {};
    char md5[33] = {};       // response body md5 (or payload checksum)
    char mode[8] = {};
    char respCtype[48] = {};
    char respHead[129] = {};  // first 128 response bytes, printable
    uint32_t reqBytes = 0;
    uint32_t bytes = 0;
    uint16_t status = 0;
};

// Install the ESP-IDF log hook that counts failed TLS handshakes, then load
// persisted settings and register the DNS redirect. Call before httpd_ssl_start.
void installLogHook();
void begin();

// Called from esp_https_server's post-handshake user callback.
void noteHandshake(const struct esp_tls* tls);
void noteHandshakeFailure(const char* error);

// Windows regex: /feature_configuration/? ($ | ? | #) anywhere in the path.
bool isFeatureConfigurationUri(const char* uri);

// Complete an intercepted request. Handles body draining, capture, reply or
// observe-mode upstream forwarding. Returns the httpd result.
esp_err_t handleRequest(struct httpd_req* request);

// Windows reverse mode forwards every non-mock path upstream. Keep that
// behavior so DNS-hijacked api-prd calls unrelated to /feature_configuration
// still reach Tesla instead of getting a local 404.
esp_err_t forwardPassthrough(struct httpd_req* request);

uint32_t dnsRedirectCount();

bool enabled();
Mode mode();
const char* modeName();
const char* host();
const char* payload();
const char* payloadMd5();
size_t payloadBytes();
size_t payloadFields();
uint32_t hitCount();
uint32_t observeCount();

bool setEnabled(bool value);
bool setMode(const char* value);
bool setHost(const char* value);
bool setPayload(const char* json, size_t length, char* error, size_t errorSize);
bool loadPreset(const char* name, char* error, size_t errorSize);
void clearHits();
void clearHandshakes();

// Append `"fsdMock":{...}` (without a leading comma) for /api/wifi-bridge/status.
void appendStatusJson(String& out);

// Copy up to `capacity` recent hits, newest first. Returns the copied count.
size_t recentHits(Hit* out, size_t capacity);

}  // namespace fsd_mock

#endif  // WIFI_BRIDGE_ENABLED && ESP32S3 && ESP_HTTPS_SERVER
