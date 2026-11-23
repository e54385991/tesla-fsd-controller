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

// PC TeslaDG chain: 443 is the TLS FSD listener; 4567 is the plaintext
// mothership reverse listener (svc/stack.py reverse:http://<ip>:4567).
constexpr uint16_t kMothershipHttpPort = 4567;

constexpr size_t kPayloadCapacity = 2049;      // compact payload limit (<= 2048 B)
constexpr size_t kPayloadInputCapacity = 4096; // raw (pretty-printed) JSON upload limit before compacting
constexpr size_t kHostCapacity = 96;
constexpr size_t kApiPrdRuleCapacity = 256;
constexpr char kDefaultApiPrdRules[] = "uuid=music";
// Local dashboard probe marker (PC web/server.py /api/selftest parity).
// Requests carrying this User-Agent are logged as SELFTEST and never counted
// as vehicle HITs or allowed to trigger the post-delivery auto-off timer.
constexpr char kSelftestUserAgent[] = "fsd-selftest";
constexpr size_t kRecentHitCapacity = 8;    // RAM ring, not persisted
constexpr size_t kHandshakeCapacity = 6;    // RAM ring, not persisted
constexpr size_t kConfigHistoryCapacity = 8; // RAM ring, not persisted
constexpr size_t kLastResponseCapacity = 769;   // full 654B default payload + headroom

enum class Mode : uint8_t {
    Reply = 0,
    Observe = 1,
};

// Mirrors the fields written by fsd_mock.py to hits.jsonl. ts is ISO local
// time when the clock is valid, otherwise "up:<seconds>".
struct Hit {
    char kind[10] = {};         // HIT / OBSERVE / FLOW / BLOCK / SELFTEST
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
struct ConfigChange {
    char ts[32] = {};
    char source[16] = {};
    char md5[33] = {};
    char prevMd5[33] = {};
    uint32_t bytes = 0;
    uint16_t fields = 0;
};

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
void setListenersReady(bool ready);
void resyncDnsRedirect();

void noteHandshake(const struct esp_tls* tls);
void noteHandshakeFailure(const char* error);
void noteAutoDisabled();

bool isFeatureConfigurationUri(const char* uri);
bool apiPrdUriAllowed(const char* uri, const char* rules);
esp_err_t handleRequest(struct httpd_req* request);
esp_err_t forwardPassthrough(struct httpd_req* request);

bool enabled();
bool autoOffOnHit();
bool apiPrdFilterEnabled();
size_t copyApiPrdRules(char* output, size_t capacity);
Mode mode();
const char* modeName();
const char* host();
size_t payloadBytes();
size_t payloadFields();
const char* payloadMd5();
size_t copyPayload(char* output, size_t capacity,
                   char* md5Out = nullptr, size_t* fieldsOut = nullptr);

bool setEnabled(bool value);
bool setAutoOffOnHit(bool value);
bool setApiPrdFilterEnabled(bool value);
bool setApiPrdFilterRules(const char* value);
bool setMode(const char* value);
bool setHost(const char* value);
bool setPayload(const char* json, size_t length, char* error, size_t errorSize,
                const char* source = "api");
bool loadPreset(const char* name, char* error, size_t errorSize);

void clearHits();
void clearHandshakes();
uint32_t hitCount();
uint32_t observeCount();
uint32_t flowCount();
uint32_t blockedCount();
uint32_t selftestCount();
uint32_t dnsRedirectCount();
uint32_t autoDisabledCount();

// Appends `"fsdMock":{...}` (without leading comma) for /api/wifi-bridge/status.
void appendStatusJson(String& out);

// RAM log export (JSONL) for /api/fsd-mock/hits and /handshakes.
void appendHitsJsonl(Print& out);
void appendHandshakesJsonl(Print& out);
void appendConfigHistoryJsonl(Print& out);

size_t recentHits(Hit* out, size_t capacity);
size_t recentHandshakes(Handshake* out, size_t capacity);

}  // namespace fsd_mock

#endif  // WIFI_BRIDGE_ENABLED && ESP32S3 && ESP_HTTPS_SERVER
