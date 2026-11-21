#pragma once
// ── FSD payload interception (feature_configuration) ─────────────────────────
// ESP32 port of TeslaDG/app/fsd_mock.py (hotspot chain):
//   DNS: api-prd.vn.cloud.tesla.cn -> ESP32 AP
//   443: any path ending in /feature_configuration -> payload.json + X-Checksum
// Logs are kept in RAM only (no SPIFFS writes); payload/config persist.
//
// Only compiled into the ESP32-S3 Wi-Fi bridge build (needs esp_https_server).

#include <sdkconfig.h>

#if defined(WIFI_BRIDGE_ENABLED) && defined(CONFIG_IDF_TARGET_ESP32S3) && defined(CONFIG_ESP_HTTPS_SERVER_ENABLE)

#include <Arduino.h>
#include <esp_err.h>
#include <stddef.h>
#include <stdint.h>

struct httpd_req;
struct esp_tls;

namespace fsd_mock {

constexpr size_t kPayloadCapacity = 2049;   // compact payload limit (Windows UI sends 602/476/282 B)
constexpr size_t kHostCapacity = 96;
constexpr size_t kRecentHitCapacity = 8;    // RAM ring, not persisted
constexpr size_t kHandshakeCapacity = 6;    // RAM ring, not persisted
constexpr size_t kLastResponseCapacity = 641;   // full 602B payload + headroom

enum class Mode : uint8_t {
    Reply = 0,
    Observe = 1,
};

// Mirrors the fields written by fsd_mock.py to hits.jsonl. ts is ISO local
// time when the clock is valid, otherwise "up:<seconds>".
struct Hit {
    char kind[8] = {};          // HIT / OBSERVE / FLOW
    char ts[32] = {};
    char client[46] = {};
    char method[8] = {};
    char url[160] = {};
    char host[64] = {};
    char userAgent[32] = {};
    char httpVersion[16] = {};
    char vin[18] = {};
    char reqMd5[33] = {};
    char reqCtype[32] = {};
    char reqHead[65] = {};
    char md5[33] = {};
    char respCtype[32] = {};
    char respHead[33] = {};
    char mode[8] = {};
    uint32_t reqBytes = 0;
    uint32_t bytes = 0;
    uint32_t atMs = 0;
    uint16_t status = 0;
};

// Mirrors handshake.log entries: handshake_ok / handshake_failed.
struct Handshake {
    char event[16] = {};
    char ts[32] = {};
    char client[46] = {};
    char sni[64] = {};
    char alpn[16] = {};
    char tls[16] = {};
    char cipher[48] = {};
    char error[64] = {};
    char firstRequest[80] = {};
};

void installLogHook();
void begin();

void noteHandshake(const struct esp_tls* tls);
void noteHandshakeFailure(const char* error);

bool isFeatureConfigurationUri(const char* uri);
esp_err_t handleRequest(struct httpd_req* request);
esp_err_t forwardPassthrough(struct httpd_req* request);

bool enabled();
Mode mode();
const char* modeName();
const char* host();
size_t payloadBytes();
size_t payloadFields();
const char* payloadMd5();
size_t copyPayload(char* output, size_t capacity);

bool setEnabled(bool value);
bool setMode(const char* value);
bool setHost(const char* value);
bool setPayload(const char* json, size_t length, char* error, size_t errorSize);
bool loadPreset(const char* name, char* error, size_t errorSize);

void clearHits();
void clearHandshakes();
uint32_t hitCount();
uint32_t observeCount();
uint32_t flowCount();
uint32_t dnsRedirectCount();

// Appends `"fsdMock":{...}` (without leading comma) for /api/wifi-bridge/status.
void appendStatusJson(String& out);

// RAM log export (JSONL) for /api/fsd-mock/hits and /handshakes.
void appendHitsJsonl(Print& out);
void appendHandshakesJsonl(Print& out);

size_t recentHits(Hit* out, size_t capacity);
size_t recentHandshakes(Handshake* out, size_t capacity);

}  // namespace fsd_mock

#endif  // WIFI_BRIDGE_ENABLED && ESP32S3 && ESP_HTTPS_SERVER
