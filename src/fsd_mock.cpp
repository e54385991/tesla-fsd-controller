#include "mod_fsd_mock.h"

#include <sdkconfig.h>

#if defined(WIFI_BRIDGE_ENABLED) && defined(CONFIG_IDF_TARGET_ESP32S3) && defined(CONFIG_ESP_HTTPS_SERVER_ENABLE)

#include <Arduino.h>
#include <Preferences.h>
#include <SPIFFS.h>
#include <WiFi.h>
#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_rom_md5.h>
#include <esp_tls.h>
#include <esp_idf_version.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <lwip/sockets.h>
#include <mbedtls/ssl.h>

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <strings.h>
#include <time.h>

#include "fsd_payload.h"
#include "mod_client_config.h"
#include "mod_dns.h"

namespace fsd_mock {
namespace {

constexpr uint32_t kSettingsMagic = 0x46444D32;  // "FDM2"
constexpr uint8_t kSettingFlagAutoOffSet = 0x01;
constexpr char kPayloadPath[] = "/fsd_payload.json";
constexpr char kPayloadTempPath[] = "/fsd_payload.tmp";
constexpr char kPayloadBackupPath[] = "/fsd_payload.bak";
constexpr size_t kRequestCaptureBytes = 64;
constexpr size_t kMaxFsdRequestBodyBytes = 65536;     // body cap on /feature_configuration
constexpr size_t kMaxForwardBodyBytes = 65536;        // streaming cap for observe/passthrough request bodies
constexpr size_t kMaxForwardResponseBytes = 262144;   // streaming cap so huge FLOW responses cannot wedge 443
constexpr uint32_t kMaxForwardDurationMs = 15000;     // wall-clock cap per upstream passthrough stream
constexpr size_t kUpstreamBufferRxBytes = 2048;       // upstream HTTP response read buffer
constexpr size_t kUpstreamBufferTxBytes = 4096;       // upstream HTTP request TX buffer (large JWT/Cookie safe)
constexpr size_t kMaxForwardedRespHeaders = 22;       // fits inside httpd max_resp_headers = 28
constexpr size_t kMaxRespHeaderTotalBytes = 4096;     // heap guard for captured upstream response headers
// TeslaDG hijack parity: api-prd (443 chain) plus the mothership hosts used
// by the PC reverse/ARP chains. Any other mothership.* also matches through
// dnsFsdRedirectMatchesHost(), so this list is mainly for display/persistence.
constexpr char kDefaultHosts[] =
    "api-prd.vn.cloud.tesla.cn mothership.vn.teslamotors.com mothership.vn.cloud.tesla.cn";
// PC reverse targets: start_hotspot/start_all use
// reverse:https://api-prd.vn.cloud.tesla.cn@443. The 4567 listener in
// TeslaDG/app/svc/stack.py is reverse:http://192.0.2.1:4567 (TEST-NET,
// unreachable): non-FSD mothership paths therefore end in mitmproxy's 502
// page instead of reaching Tesla. Match that contract so this listener can
// never let the cloud rewrite the vehicle's FSD config.
constexpr char kApiPrdHost[] = "api-prd.vn.cloud.tesla.cn";
constexpr char kMothershipBlockedUpstream[] = "192.0.2.1:4567";
// Before mothership support the persisted default was this single host. Only
// that exact value is auto-migrated; user-edited host lists are left alone.
constexpr char kLegacyDefaultHosts[] = "api-prd.vn.cloud.tesla.cn";

struct Settings {
    uint32_t magic = kSettingsMagic;
    uint8_t enabled = 0;  // Default OFF to preserve RAM/network headroom; user toggles ON in UI
    uint8_t mode = static_cast<uint8_t>(Mode::Reply);
    uint8_t flags = kSettingFlagAutoOffSet;
    uint8_t autoOffOnHit = 1;  // Default ON: auto-disable FSD mock once delivery succeeds
    char host[kHostCapacity] =
        "api-prd.vn.cloud.tesla.cn mothership.vn.teslamotors.com mothership.vn.cloud.tesla.cn";
};
static_assert(sizeof(Settings) == 4 + 4 + kHostCapacity, "Settings NVS layout must stay stable");

struct State {
    bool enabled = false;
    bool autoOffOnHit = true;
    Mode mode = Mode::Reply;
    char host[kHostCapacity] =
        "api-prd.vn.cloud.tesla.cn mothership.vn.teslamotors.com mothership.vn.cloud.tesla.cn";
    char payload[kPayloadCapacity] = {};
    char md5[33] = {};
    size_t bytes = 0;
    size_t fields = 0;
};

struct Capture {
    uint32_t requestBytes = 0;
    char requestMd5[33] = {};
    char ctype[32] = {};
    char vin[18] = {};
    char head[kRequestCaptureBytes + 1] = {};
};

struct LastResponse {
    char kind[8] = {};
    uint16_t status = 0;
    char ctype[32] = {};
    char md5[33] = {};
    uint32_t bytes = 0;
    char body[kLastResponseCapacity] = {};
};

State gState;
SemaphoreHandle_t gStateMutex = nullptr;
bool gActive = false;

Hit gHits[kRecentHitCapacity];
size_t gHitCount = 0;
size_t gHitWrite = 0;
portMUX_TYPE gHitMux = portMUX_INITIALIZER_UNLOCKED;
uint32_t gHitTotal = 0;
uint32_t gObserveTotal = 0;
uint32_t gFlowTotal = 0;
uint32_t gAutoDisabledTotal = 0;

ConfigChange gConfigHistory[kConfigHistoryCapacity];
size_t gConfigCount = 0;
size_t gConfigWrite = 0;
portMUX_TYPE gConfigMux = portMUX_INITIALIZER_UNLOCKED;

// Persistent ring of the last 5 下发记录 (NVS namespace "fsdmock", key "cfglog").
constexpr uint32_t kConfigLogMagic = 0x43464731;  // "CFG1"
struct ConfigLogBlob {
    uint32_t magic;
    uint16_t count;
    uint16_t write;
    ConfigChange items[kConfigHistoryCapacity];
};

Handshake gHandshakes[kHandshakeCapacity];
size_t gHandshakeCount = 0;
size_t gHandshakeWrite = 0;
portMUX_TYPE gHandshakeMux = portMUX_INITIALIZER_UNLOCKED;
uint32_t gHandshakeOk = 0;
uint32_t gHandshakeFail = 0;
uint32_t gLastHandshakeOkMs = 0;
uint32_t gLastHandshakeFailMs = 0;
char gLastHandshakeClient[46] = {};
char gLastHandshakeTls[16] = {};
char gLastHandshakeCipher[48] = {};
char gLastHandshakeAlpn[16] = {};
char gLastHandshakeError[64] = {};

LastResponse gLastResponse;
portMUX_TYPE gLastResponseMux = portMUX_INITIALIZER_UNLOCKED;

vprintf_like_t gPreviousLog = nullptr;
volatile TaskHandle_t gUpstreamTask = nullptr;
volatile uint32_t gLastMbedtlsFailureMs = 0;

struct ProxyJob {
    httpd_req_t* req = nullptr;
    Hit hit;
    Capture capture;
    bool observe = false;
};

portMUX_TYPE gProxyMux = portMUX_INITIALIZER_UNLOCKED;
QueueHandle_t gProxyQueue = nullptr;
TaskHandle_t gProxyWorkerTask = nullptr;
volatile bool gProxyShuttingDown = false;
uint32_t gProxyInFlight = 0;

// ── basic helpers ───────────────────────────────────────────────────────────

void lockState() {
    if (gStateMutex) xSemaphoreTake(gStateMutex, portMAX_DELAY);
}

void unlockState() {
    if (gStateMutex) xSemaphoreGive(gStateMutex);
}

void md5Hex(const void* data, size_t length, char out[33]) {
    uint8_t digest[16];
    md5_context_t context;
    esp_rom_md5_init(&context);
    esp_rom_md5_update(&context, data, static_cast<uint32_t>(length));
    esp_rom_md5_final(digest, &context);
    for (size_t i = 0; i < sizeof(digest); ++i) {
        snprintf(out + i * 2, 3, "%02x", static_cast<unsigned>(digest[i]));
    }
    out[32] = '\0';
}

void jsonEscape(Print& out, const char* value) {
    if (!value) return;
    for (const char* p = value; *p; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        switch (c) {
            case '\\': out.print("\\\\"); break;
            case '"': out.print("\\\""); break;
            case '\n': out.print("\\n"); break;
            case '\r': out.print("\\r"); break;
            case '\t': out.print("\\t"); break;
            default:
                if (c < 0x20 || c >= 0x7f) out.print('.');
                else out.write(c);
                break;
        }
    }
}

void formatTimestamp(char out[32]) {
    const time_t now = time(nullptr);
    if (now > 1600000000) {
        struct tm local = {};
        localtime_r(&now, &local);
        strftime(out, 32, "%Y-%m-%dT%H:%M:%S%z", &local);
    } else {
        snprintf(out, 32, "up:%lu", static_cast<unsigned long>(millis() / 1000));
    }
}

const char* trimCopy(const char* value, char* out, size_t capacity) {
    if (capacity == 0) return out;
    out[0] = '\0';
    if (!value) return out;
    while (*value && isspace(static_cast<unsigned char>(*value))) ++value;
    const char* end = value + strlen(value);
    while (end > value && isspace(static_cast<unsigned char>(end[-1]))) --end;
    size_t length = static_cast<size_t>(end - value);
    if (length >= capacity) length = capacity - 1;
    memcpy(out, value, length);
    out[length] = '\0';
    return out;
}

bool validHostList(const char* host) {
    if (!host || !*host) return false;
    if (strlen(host) >= kHostCapacity) return false;
    // One or more DNS names separated by spaces (setHost normalizes , and ;
    // to spaces). Reject underscores, empty labels, leading/trailing dots or
    // hyphens, and over-long labels so a typo cannot silently disable FSD.
    const char* p = host;
    while (*p) {
        while (*p == ' ') ++p;
        if (!*p) break;
        const char* start = p;
        while (*p && *p != ' ') ++p;
        const size_t len = static_cast<size_t>(p - start);
        if (len == 0 || len > 253) return false;
        if (start[0] == '.' || start[len - 1] == '.') return false;
        if (start[0] == '-' || start[len - 1] == '-') return false;
        bool hasDot = false;
        size_t label = 0;
        for (size_t i = 0; i < len; ++i) {
            const unsigned char c = static_cast<unsigned char>(start[i]);
            if (c == '.') {
                if (label == 0 || label > 63) return false;
                label = 0;
                hasDot = true;
                continue;
            }
            if (!(isalnum(c) || c == '-')) return false;
            if (label == 0 && c == '-') return false;
            if (c == '-' && i + 1 < len && start[i + 1] == '.') return false;
            ++label;
        }
        if (label == 0 || label > 63) return false;
        if (!hasDot) return false;
    }
    return true;
}

size_t countFields(cJSON* root) {
    return root ? static_cast<size_t>(cJSON_GetArraySize(root)) : 0;
}

// Match Python json.loads() / JS JSON.parse(): when a raw JSON object has
// duplicate keys, keep the first key's position and replace its value with the
// last occurrence so compacted bytes and MD5 X-Checksum match PC TeslaDG.
void dedupeJsonObjects(cJSON* item) {
    if (!item) return;
    if (cJSON_IsObject(item)) {
        for (cJSON* cur = item->child; cur; cur = cur->next) {
            if (!cur->string) continue;
            cJSON* dup = cur->next;
            while (dup) {
                cJSON* nextDup = dup->next;
                if (dup->string && strcmp(cur->string, dup->string) == 0) {
                    cJSON* detached = cJSON_DetachItemViaPointer(item, dup);
                    if (detached) {
                        cJSON_ReplaceItemViaPointer(item, cur, detached);
                        cur = detached;
                    }
                }
                dup = nextDup;
            }
        }
    }
    for (cJSON* child = item->child; child; child = child->next) {
        dedupeJsonObjects(child);
    }
}

bool compactJson(const char* json, size_t length, char* out, size_t outCapacity,
                 size_t& outLength, size_t& fields, char* error, size_t errorSize) {
    if (!json || length == 0) {
        snprintf(error, errorSize, "empty payload");
        return false;
    }
    if (length > kPayloadInputCapacity) {
        snprintf(error, errorSize, "payload_too_large");
        return false;
    }
    if (memchr(json, '\0', length) != nullptr) {
        snprintf(error, errorSize, "invalid_json");
        return false;
    }
    const char* parseEnd = nullptr;
    cJSON* root = cJSON_ParseWithLengthOpts(json, length, &parseEnd, false);
    if (!root || !parseEnd) {
        cJSON_Delete(root);
        snprintf(error, errorSize, "invalid_json");
        return false;
    }
    for (const char* tail = parseEnd; tail < json + length; ++tail) {
        if (!isspace(static_cast<unsigned char>(*tail))) {
            cJSON_Delete(root);
            snprintf(error, errorSize, "invalid_json");
            return false;
        }
    }
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        snprintf(error, errorSize, "expected_object");
        return false;
    }
    dedupeJsonObjects(root);
    char* text = cJSON_PrintUnformatted(root);
    fields = countFields(root);
    cJSON_Delete(root);
    if (!text) {
        snprintf(error, errorSize, "print_failed");
        return false;
    }
    const size_t textLength = strlen(text);
    if (textLength == 0 || textLength >= outCapacity) {
        cJSON_free(text);
        snprintf(error, errorSize, "payload_too_large");
        return false;
    }
    memcpy(out, text, textLength + 1);
    cJSON_free(text);
    outLength = textLength;
    return true;
}

void copyPrintable(const uint8_t* source, size_t sourceLength, char* out, size_t capacity) {
    if (capacity == 0) return;
    size_t written = 0;
    for (size_t i = 0; i < sourceLength && written + 1 < capacity; ++i) {
        const unsigned char c = source[i];
        out[written++] = (c < 0x20 || c > 0x7e) ? '.' : static_cast<char>(c);
    }
    out[written] = '\0';
}

void copyText(const char* source, size_t sourceLength, char* out, size_t capacity) {
    if (capacity == 0) return;
    size_t written = 0;
    for (size_t i = 0; i < sourceLength && written + 1 < capacity; ++i) {
        const unsigned char c = static_cast<unsigned char>(source[i]);
        out[written++] = (c < 0x20 || c > 0x7e) ? '.' : static_cast<char>(c);
    }
    out[written] = '\0';
}

void socketAddress(int fd, char* out, size_t capacity) {
    out[0] = '\0';
    if (fd < 0 || capacity == 0) return;
    // ESP-IDF's httpd listens on a dual-stack IPv6 socket when IPv6 is
    // enabled; a plain sockaddr_in would read only zeros for v4 clients.
    struct sockaddr_storage storage = {};
    socklen_t length = sizeof(storage);
    if (getpeername(fd, reinterpret_cast<struct sockaddr*>(&storage), &length) != 0) return;
    if (storage.ss_family == AF_INET) {
        auto* address = reinterpret_cast<struct sockaddr_in*>(&storage);
        if (inet_ntop(AF_INET, &address->sin_addr, out, capacity)) return;
    }
#if CONFIG_LWIP_IPV6
    else if (storage.ss_family == AF_INET6) {
        auto* address = reinterpret_cast<struct sockaddr_in6*>(&storage);
        const uint8_t* bytes = address->sin6_addr.s6_addr;
        static const uint8_t v4mapped[12] = {0,0,0,0,0,0,0,0,0,0,0xff,0xff};
        if (memcmp(bytes, v4mapped, sizeof(v4mapped)) == 0) {
            snprintf(out, capacity, "%u.%u.%u.%u", bytes[12], bytes[13], bytes[14], bytes[15]);
            return;
        }
        if (inet_ntop(AF_INET6, &address->sin6_addr, out, capacity)) return;
    }
#endif
    out[0] = '\0';
}

void clientAddress(httpd_req_t* request, char* out, size_t capacity) {
    socketAddress(httpd_req_to_sockfd(request), out, capacity);
}

// Local listener port for this request: 443 uses TLS + HTTPS upstream, 4567
// is the PC-style plaintext mothership reverse listener. ESP-IDF's httpd keeps
// one server per port, so getsockname() tells the callback which chain it is.
uint16_t localPort(httpd_req_t* request) {
    const int fd = httpd_req_to_sockfd(request);
    if (fd < 0) return 0;
    struct sockaddr_storage storage = {};
    socklen_t length = sizeof(storage);
    if (getsockname(fd, reinterpret_cast<struct sockaddr*>(&storage), &length) != 0) return 0;
    if (storage.ss_family == AF_INET) {
        return ntohs(reinterpret_cast<struct sockaddr_in*>(&storage)->sin_port);
    }
#if CONFIG_LWIP_IPV6
    if (storage.ss_family == AF_INET6) {
        return ntohs(reinterpret_cast<struct sockaddr_in6*>(&storage)->sin6_port);
    }
#endif
    return 0;
}

void extractVin(const char* uri, char out[18]) {
    out[0] = '\0';
    const char* marker = strstr(uri, "/vehicles/");
    if (!marker) return;
    marker += strlen("/vehicles/");
    size_t length = 0;
    while (length < 17 && marker[length] &&
           (isalnum(static_cast<unsigned char>(marker[length])) || marker[length] == '-')) {
        ++length;
    }
    if (length == 0) return;
    memcpy(out, marker, length);
    out[length] = '\0';
}

const char* httpMethodName(int method) {
    switch (method) {
        case HTTP_GET: return "GET";
        case HTTP_POST: return "POST";
        case HTTP_PUT: return "PUT";
        case HTTP_DELETE: return "DELETE";
        case HTTP_HEAD: return "HEAD";
        case HTTP_OPTIONS: return "OPTIONS";
        case HTTP_PATCH: return "PATCH";
        default: return "GET";
    }
}

esp_http_client_method_t upstreamMethod(int method) {
    switch (method) {
        case HTTP_POST: return HTTP_METHOD_POST;
        case HTTP_PUT: return HTTP_METHOD_PUT;
        case HTTP_DELETE: return HTTP_METHOD_DELETE;
        case HTTP_HEAD: return HTTP_METHOD_HEAD;
        case HTTP_OPTIONS: return HTTP_METHOD_OPTIONS;
        case HTTP_PATCH: return HTTP_METHOD_PATCH;
        case HTTP_GET:
        default: return HTTP_METHOD_GET;
    }
}

void copyHeader(httpd_req_t* request, const char* name, char* out, size_t capacity) {
    if (!out || capacity == 0) return;
    out[0] = '\0';
    // Truncate long headers (e.g. browser/Tesla User-Agent, Content-Type with
    // charset) instead of discarding them to an empty string.
    const esp_err_t err = httpd_req_get_hdr_value_str(request, name, out, capacity);
    if (err != ESP_OK && err != ESP_ERR_HTTPD_RESULT_TRUNC) out[0] = '\0';
}

bool isChunkedRequest(httpd_req_t* request) {
    char te[48] = {};
    copyHeader(request, "Transfer-Encoding", te, sizeof(te));
    for (char* p = te; *p; ++p) *p = static_cast<char>(tolower(static_cast<unsigned char>(*p)));
    return strstr(te, "chunked") != nullptr;
}

void parseRequestMeta(httpd_req_t* request, Hit& hit, Capture& capture) {
    char hostHeader[96] = {};
    copyHeader(request, "Host", hostHeader, sizeof(hostHeader));
    char userAgent[48] = {};
    copyHeader(request, "User-Agent", userAgent, sizeof(userAgent));
    char ctype[32] = {};
    copyHeader(request, "Content-Type", ctype, sizeof(ctype));

    const char* uri = request->uri;
    strlcpy(hit.method, httpMethodName(request->method), sizeof(hit.method));
    // fsd_mock.py logs flow.request.pretty_url (scheme://host/path?query).
    // Port 4567 is the plaintext mothership listener, 443 is TLS.
    strlcpy(hit.url, localPort(request) == kMothershipHttpPort ? "http://" : "https://", sizeof(hit.url));
    if (hostHeader[0]) strlcat(hit.url, hostHeader, sizeof(hit.url));
    strlcat(hit.url, uri, sizeof(hit.url));
    strlcpy(hit.host, hostHeader, sizeof(hit.host));
    strlcpy(hit.userAgent, userAgent, sizeof(hit.userAgent));
    strlcpy(hit.httpVersion, "HTTP/1.1", sizeof(hit.httpVersion));
    strlcpy(capture.ctype, ctype, sizeof(capture.ctype));
    extractVin(uri, capture.vin);
    clientAddress(request, hit.client, sizeof(hit.client));
    formatTimestamp(hit.ts);
    hit.atMs = millis();
    hit.reqBytes = static_cast<uint32_t>(request->content_len);
}

void updateLatestHandshake(const Hit& hit) {
    const char* host = hit.host[0] ? hit.host : "";
    // fsd_mock.py logs "<METHOD> <path>" for first_request; keep the same
    // shape but use the path portion of the pretty URL.
    const char* path = strstr(hit.url, "://");
    path = path ? strchr(path + 3, '/') : hit.url;
    if (!path) path = "/";
    char first[80] = {};
    snprintf(first, sizeof(first), "%.7s %.70s", hit.method, path);
    portENTER_CRITICAL(&gHandshakeMux);
    for (size_t age = 0; age < gHandshakeCount; ++age) {
        const size_t index = (gHandshakeWrite + kHandshakeCapacity - 1 - age) % kHandshakeCapacity;
        Handshake& handshake = gHandshakes[index];
        if (strcmp(handshake.event, "handshake_ok") != 0) continue;
        if (host[0] && !handshake.sni[0]) strlcpy(handshake.sni, host, sizeof(handshake.sni));
        if (!handshake.firstRequest[0]) strlcpy(handshake.firstRequest, first, sizeof(handshake.firstRequest));
        break;
    }
    portEXIT_CRITICAL(&gHandshakeMux);
}

void recordHit(const Hit& hit, const char* responseBody) {
    portENTER_CRITICAL(&gHitMux);
    gHits[gHitWrite] = hit;
    gHitWrite = (gHitWrite + 1) % kRecentHitCapacity;
    if (gHitCount < kRecentHitCapacity) ++gHitCount;
    if (strcmp(hit.kind, "OBSERVE") == 0) ++gObserveTotal;
    else if (strcmp(hit.kind, "FLOW") == 0) ++gFlowTotal;
    else ++gHitTotal;
    portEXIT_CRITICAL(&gHitMux);

    // The panel's "最新回包" is an FSD view. Non-FSD FLOW responses (for
    // example Tesla's mTLS 400 for a command probe) stay in the log/recent
    // list but must not overwrite the last HIT/OBSERVE body.
    if (strcmp(hit.kind, "FLOW") != 0) {
        portENTER_CRITICAL(&gLastResponseMux);
        strlcpy(gLastResponse.kind, hit.kind, sizeof(gLastResponse.kind));
        gLastResponse.status = hit.status;
        strlcpy(gLastResponse.ctype, hit.respCtype, sizeof(gLastResponse.ctype));
        strlcpy(gLastResponse.md5, hit.md5, sizeof(gLastResponse.md5));
        gLastResponse.bytes = hit.bytes;
        const size_t length = responseBody ? strlen(responseBody) : 0;
        const size_t copy = length < sizeof(gLastResponse.body) - 1
            ? length : sizeof(gLastResponse.body) - 1;
        if (copy) memcpy(gLastResponse.body, responseBody, copy);
        gLastResponse.body[copy] = '\0';
        portEXIT_CRITICAL(&gLastResponseMux);
    }

    updateLatestHandshake(hit);
}

// ── payload storage ─────────────────────────────────────────────────────────

void freeLogSpace() {
    // FSD logs are RAM-only. If the payload file cannot be written, reclaim
    // the existing diagnostic log; payload config is the critical file.
    SPIFFS.remove("/diag.log");
}

bool writePayloadFile(const char* payload, size_t length) {
    SPIFFS.remove(kPayloadTempPath);
    File file = SPIFFS.open(kPayloadTempPath, "w");
    if (!file) return false;
    const size_t written = file.write(reinterpret_cast<const uint8_t*>(payload), length);
    file.close();
    if (written != length) {
        SPIFFS.remove(kPayloadTempPath);
        return false;
    }
    // Keep one generation until the rename succeeds. SPIFFS rename does not
    // reliably replace an existing destination, hence the backup dance.
    SPIFFS.remove(kPayloadBackupPath);
    const bool hadOld = SPIFFS.exists(kPayloadPath);
    if (hadOld && !SPIFFS.rename(kPayloadPath, kPayloadBackupPath)) {
        SPIFFS.remove(kPayloadTempPath);
        return false;
    }
    if (!SPIFFS.rename(kPayloadTempPath, kPayloadPath)) {
        if (hadOld) SPIFFS.rename(kPayloadBackupPath, kPayloadPath);
        SPIFFS.remove(kPayloadTempPath);
        return false;
    }
    SPIFFS.remove(kPayloadBackupPath);
    return true;
}

bool savePayloadFile(const char* payload, size_t length) {
    const size_t used = SPIFFS.usedBytes();
    const size_t total = SPIFFS.totalBytes();
    const size_t margin = length + 4096;
    if (total > used && total - used < margin) freeLogSpace();
    if (writePayloadFile(payload, length)) return true;
    freeLogSpace();
    return writePayloadFile(payload, length);
}

bool loadPayloadFile(char* out, size_t capacity, size_t& length) {
    File file = SPIFFS.open(kPayloadPath, "r");
    if (!file) return false;
    length = file.size();
    if (length == 0 || length >= capacity) {
        file.close();
        return false;
    }
    const size_t read = file.read(reinterpret_cast<uint8_t*>(out), length);
    file.close();
    out[length] = '\0';
    return read == length;
}

void syncDnsRedirect() {
    bool enabledNow;
    char hostNow[kHostCapacity];
    lockState();
    enabledNow = gState.enabled;
    strlcpy(hostNow, gState.host, sizeof(hostNow));
    unlockState();
    const IPAddress answer = WiFi.softAPIP();
    dnsSetFsdRedirect(gActive && enabledNow, hostNow, answer);
}

bool persistSettings() {
    Settings settings;
    lockState();
    settings.enabled = gState.enabled ? 1 : 0;
    settings.mode = static_cast<uint8_t>(gState.mode);
    settings.flags = kSettingFlagAutoOffSet;
    settings.autoOffOnHit = gState.autoOffOnHit ? 1 : 0;
    strlcpy(settings.host, gState.host, sizeof(settings.host));
    unlockState();

    Preferences prefs;
    if (!prefs.begin("fsdmock", false)) return false;
    const size_t written = prefs.putBytes("cfg", &settings, sizeof(settings));
    prefs.end();
    return written == sizeof(settings);
}

// Snapshot the ring under the mutex, then write outside it (NVS may block).
void persistConfigLog() {
    ConfigLogBlob blob = {};
    blob.magic = kConfigLogMagic;
    portENTER_CRITICAL(&gConfigMux);
    blob.count = static_cast<uint16_t>(gConfigCount);
    blob.write = static_cast<uint16_t>(gConfigWrite);
    memcpy(blob.items, gConfigHistory, sizeof(gConfigHistory));
    portEXIT_CRITICAL(&gConfigMux);

    Preferences prefs;
    if (!prefs.begin("fsdmock", false)) return;
    prefs.putBytes("cfglog", &blob, sizeof(blob));
    prefs.end();
}

// ── request body ────────────────────────────────────────────────────────────

void captureBodyHead(const char* chunk, size_t receivedSize, Capture& capture, size_t& captured) {
    if (captured >= kRequestCaptureBytes || receivedSize == 0) return;
    const size_t headCopy = (kRequestCaptureBytes - captured) < receivedSize
        ? (kRequestCaptureBytes - captured) : receivedSize;
    if (capture.head[0] == '\0' && headCopy > 0) {
        const bool text = strstr(capture.ctype, "json") || strstr(capture.ctype, "text") ||
            chunk[0] == '{' || chunk[0] == '[';
        if (text) {
            copyText(chunk, headCopy, capture.head, sizeof(capture.head));
        } else {
            size_t written = 0;
            for (size_t i = 0; i < headCopy && written + 2 < sizeof(capture.head); ++i) {
                snprintf(capture.head + written, sizeof(capture.head) - written,
                         "%02x", static_cast<unsigned>(static_cast<uint8_t>(chunk[i])));
                written += 2;
            }
        }
    }
    captured += headCopy;
}

bool drainRequestBody(httpd_req_t* request, Capture& capture) {
    capture.requestBytes = static_cast<uint32_t>(request->content_len);
    capture.head[0] = '\0';
    capture.requestMd5[0] = '\0';
    if (request->content_len == 0) return true;

    md5_context_t md5;
    esp_rom_md5_init(&md5);
    char chunk[256];
    size_t remaining = request->content_len;
    size_t captured = 0;
    int timeouts = 0;
    while (remaining > 0) {
        const size_t want = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
        const int received = httpd_req_recv(request, chunk, want);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts > 3) return false;
            continue;
        }
        if (received <= 0) return false;
        const size_t receivedSize = static_cast<size_t>(received);
        esp_rom_md5_update(&md5, chunk, static_cast<uint32_t>(received));
        captureBodyHead(chunk, receivedSize, capture, captured);
        remaining -= receivedSize;
    }
    uint8_t digest[16];
    esp_rom_md5_final(digest, &md5);
    for (size_t i = 0; i < sizeof(digest); ++i) {
        snprintf(capture.requestMd5 + i * 2, 3, "%02x", static_cast<unsigned>(digest[i]));
    }
    return true;
}

// Drain an unread request body before an early error response. Closing a
// socket with unread data sends RST, which can discard the 502/413 response
// before the client reads it (visible as "empty reply" from curl/the car).
bool discardRequestBody(httpd_req_t* request, size_t limit) {
    size_t remaining = request->content_len;
    char buffer[256];
    int timeouts = 0;
    while (remaining > 0 && limit > 0) {
        const size_t want = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        const size_t capped = want < limit ? want : limit;
        const int received = httpd_req_recv(request, buffer, capped);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts > 2) return false;
            continue;
        }
        if (received <= 0) return false;
        remaining -= static_cast<size_t>(received);
        limit -= static_cast<size_t>(received);
    }
    return remaining == 0;
}

// ── responses / forwarding ──────────────────────────────────────────────────

const char* formatStatusLine(int code, char buffer[32]) {
    switch (code) {
        case 200: return "200 OK";
        case 201: return "201 Created";
        case 202: return "202 Accepted";
        case 204: return "204 No Content";
        case 206: return "206 Partial Content";
        case 301: return "301 Moved Permanently";
        case 302: return "302 Found";
        case 304: return "304 Not Modified";
        case 307: return "307 Temporary Redirect";
        case 308: return "308 Permanent Redirect";
        case 400: return "400 Bad Request";
        case 401: return "401 Unauthorized";
        case 403: return "403 Forbidden";
        case 404: return "404 Not Found";
        case 405: return "405 Method Not Allowed";
        case 409: return "409 Conflict";
        case 413: return "413 Payload Too Large";
        case 429: return "429 Too Many Requests";
        case 500: return "500 Internal Server Error";
        case 502: return "502 Bad Gateway";
        case 503: return "503 Service Unavailable";
        case 504: return "504 Gateway Timeout";
        default:
            snprintf(buffer, 32, "%d", code);
            return buffer;
    }
}

void finishJsonError(httpd_req_t* request, Hit& hit, char* responseBody,
                     int status, const char* json) {
    char statusLine[32];
    httpd_resp_set_status(request, formatStatusLine(status, statusLine));
    httpd_resp_set_type(request, "application/json");
    // Requests that fail before their body is drained must not reuse the
    // socket; the car would otherwise keep a half-read request open.
    httpd_resp_set_hdr(request, "Connection", "close");
    const size_t length = strlen(json);
    httpd_resp_send(request, json, length);
    hit.status = static_cast<uint16_t>(status);
    hit.bytes = static_cast<uint32_t>(length);
    md5Hex(json, length, hit.md5);
    strlcpy(hit.respCtype, "application/json", sizeof(hit.respCtype));
    copyPrintable(reinterpret_cast<const uint8_t*>(json), length, hit.respHead, sizeof(hit.respHead));
    if (responseBody) strlcpy(responseBody, json, kLastResponseCapacity);
}

// TeslaDG mitmproxy error page (mitmproxy/proxy/layers/http/_base.py
// format_error + _http1.make_error_response): status, Server: mitmproxy,
// Content-Type: text/html, Connection: close, HTML body.
void appendHtmlEscaped(char* out, size_t capacity, size_t& used, const char* text) {
    if (!out || capacity == 0 || !text) return;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p; ++p) {
        const char* replacement = nullptr;
        char entity[8] = {};
        switch (*p) {
            case '&': replacement = "&amp;"; break;
            case '<': replacement = "&lt;"; break;
            case '>': replacement = "&gt;"; break;
            case '"': replacement = "&quot;"; break;
            case '\'': replacement = "&#x27;"; break;
            default: break;
        }
        if (replacement) {
            const size_t length = strlen(replacement);
            if (used + length + 1 > capacity) return;
            memcpy(out + used, replacement, length);
            used += length;
        } else if (*p < 0x20) {
            snprintf(entity, sizeof(entity), "&#x%x;", static_cast<unsigned>(*p));
            const size_t length = strlen(entity);
            if (used + length + 1 > capacity) return;
            memcpy(out + used, entity, length);
            used += length;
        } else {
            if (used + 2 > capacity) return;
            out[used++] = static_cast<char>(*p);
        }
    }
    out[used] = '\0';
}

size_t formatProxyErrorBody(char* out, size_t capacity, int status, const char* message) {
    if (!out || capacity == 0) return 0;
    const char* reason = "Bad Gateway";
    if (status == 400) reason = "Bad Request";
    else if (status == 403) reason = "Forbidden";
    else if (status == 413) reason = "Payload Too Large";
    else if (status == 500) reason = "Internal Server Error";
    else if (status == 503) reason = "Service Unavailable";
    else if (status == 504) reason = "Gateway Timeout";

    size_t used = 0;
    const int head = snprintf(out, capacity,
        "<html>\n<head>\n    <title>%d %s</title>\n</head>\n<body>\n    <h1>%d %s</h1>\n    <p>",
        status, reason, status, reason);
    if (head > 0) used = static_cast<size_t>(head) < capacity ? static_cast<size_t>(head) : capacity - 1;
    out[used] = '\0';
    appendHtmlEscaped(out, capacity, used, message ? message : "");
    const char tail[] = "</p>\n</body>\n</html>";
    if (used + sizeof(tail) <= capacity) {
        memcpy(out + used, tail, sizeof(tail));
        used += sizeof(tail) - 1;
    }
    out[used] = '\0';
    return used;
}

void sendProxyError(httpd_req_t* request, Hit& hit, char* responseBody,
                    int status, const char* message) {
    char body[640];
    const size_t used = formatProxyErrorBody(body, sizeof(body), status, message);

    char statusLine[32];
    httpd_resp_set_status(request, formatStatusLine(status, statusLine));
    httpd_resp_set_type(request, "text/html");
    httpd_resp_set_hdr(request, "Server", "mitmproxy 12.3.1");
    httpd_resp_set_hdr(request, "Connection", "close");
    httpd_resp_send(request, body, static_cast<ssize_t>(used));

    hit.status = static_cast<uint16_t>(status);
    hit.bytes = static_cast<uint32_t>(used);
    md5Hex(body, used, hit.md5);
    strlcpy(hit.respCtype, "text/html", sizeof(hit.respCtype));
    copyPrintable(reinterpret_cast<const uint8_t*>(body), used, hit.respHead, sizeof(hit.respHead));
    if (responseBody) strlcpy(responseBody, body, kLastResponseCapacity);
}

bool upstreamReady() {
    return WiFi.status() == WL_CONNECTED &&
        static_cast<uint32_t>(WiFi.localIP()) != 0;
}

// Fast-fail repeated upstream failures (mTLS/blocked/Slow api-prd) instead of
// allocating a new TLS context and stalling the single 443 task on every
// vehicle request. Backoff: 15s -> 30s -> 60s.
portMUX_TYPE gUpstreamGuardMux = portMUX_INITIALIZER_UNLOCKED;
uint8_t gUpstreamFailures = 0;
uint32_t gUpstreamCooldownUntilMs = 0;
uint32_t gUpstreamLastStaIp = 0;

bool upstreamCoolingDown(uint32_t staIp, uint32_t now) {
    bool cooling = false;
    portENTER_CRITICAL(&gUpstreamGuardMux);
    if (gUpstreamLastStaIp != staIp) {
        gUpstreamLastStaIp = staIp;
        gUpstreamFailures = 0;
        gUpstreamCooldownUntilMs = 0;
    } else if (gUpstreamFailures >= 2 &&
               static_cast<int32_t>(now - gUpstreamCooldownUntilMs) < 0) {
        cooling = true;
    }
    portEXIT_CRITICAL(&gUpstreamGuardMux);
    return cooling;
}

void upstreamRecordSuccess() {
    portENTER_CRITICAL(&gUpstreamGuardMux);
    gUpstreamFailures = 0;
    gUpstreamCooldownUntilMs = 0;
    portEXIT_CRITICAL(&gUpstreamGuardMux);
}

void upstreamRecordFailure(uint32_t now) {
    static constexpr uint32_t kCooldownMs[] = {0, 0, 15000, 30000, 60000};
    portENTER_CRITICAL(&gUpstreamGuardMux);
    if (gUpstreamFailures < 4) ++gUpstreamFailures;
    const uint32_t delayMs = kCooldownMs[gUpstreamFailures];
    gUpstreamCooldownUntilMs = delayMs ? (now + delayMs) : 0;
    portEXIT_CRITICAL(&gUpstreamGuardMux);
}

// Captures upstream HTTP response headers via HTTP_EVENT_ON_HEADER.
// Note: esp_http_client_get_header() only inspects outgoing *request* headers,
// not upstream response headers; and httpd_resp_set_hdr()/set_type() retain
// raw pointers until httpd_resp_send* completes, so this object must outlive
// the response transmission.
struct UpstreamResponseHeaders {
    struct Entry {
        char* key = nullptr;
        char* value = nullptr;
    };
    char* contentType = nullptr;
    Entry entries[kMaxForwardedRespHeaders];
    size_t count = 0;
    size_t totalBytes = 0;

    ~UpstreamResponseHeaders() {
        free(contentType);
        for (size_t i = 0; i < count; ++i) {
            free(entries[i].key);
            free(entries[i].value);
        }
    }

    static bool isHopByHop(const char* key) {
        return strcasecmp(key, "Transfer-Encoding") == 0 ||
               strcasecmp(key, "Content-Length") == 0 ||
               strcasecmp(key, "Connection") == 0 ||
               strcasecmp(key, "Keep-Alive") == 0 ||
               strcasecmp(key, "Proxy-Connection") == 0 ||
               strcasecmp(key, "Upgrade") == 0 ||
               strcasecmp(key, "Trailer") == 0 ||
               strcasecmp(key, "TE") == 0;
    }

    static bool hasCrlf(const char* text) {
        return text && strpbrk(text, "\r\n") != nullptr;
    }

    void add(const char* key, const char* value) {
        if (!key || !*key || !value) return;
        if (hasCrlf(key) || hasCrlf(value)) return;
        const size_t keyLen = strlen(key);
        const size_t valLen = strlen(value);
        if (strcasecmp(key, "Content-Type") == 0) {
            if (valLen > 128) return;
            char* dup = static_cast<char*>(malloc(valLen + 1));
            if (!dup) return;
            memcpy(dup, value, valLen + 1);
            free(contentType);
            contentType = dup;
            return;
        }
        if (isHopByHop(key)) return;
        if (count >= kMaxForwardedRespHeaders) return;
        if (totalBytes + keyLen + valLen + 2 > kMaxRespHeaderTotalBytes) return;
        char* keyDup = static_cast<char*>(malloc(keyLen + 1));
        char* valDup = static_cast<char*>(malloc(valLen + 1));
        if (!keyDup || !valDup) {
            free(keyDup);
            free(valDup);
            return;
        }
        memcpy(keyDup, key, keyLen + 1);
        memcpy(valDup, value, valLen + 1);
        entries[count].key = keyDup;
        entries[count].value = valDup;
        ++count;
        totalBytes += keyLen + valLen + 2;
    }
};

esp_err_t upstreamEventHandler(esp_http_client_event_t* event) {
    if (event && event->event_id == HTTP_EVENT_ON_HEADER && event->user_data) {
        auto* headers = static_cast<UpstreamResponseHeaders*>(event->user_data);
        headers->add(event->header_key, event->header_value);
    }
    return ESP_OK;
}

bool fastRejectForward(httpd_req_t* request, Capture& capture,
                       Hit& hit, char* responseBody, size_t responseCapacity,
                       bool& closeConnection) {
    closeConnection = false;
    hit.status = 0;
    hit.bytes = 0;
    hit.md5[0] = '\0';
    hit.respHead[0] = '\0';
    hit.respCtype[0] = '\0';
    if (responseBody && responseCapacity) responseBody[0] = '\0';

    strlcpy(hit.reqCtype, capture.ctype, sizeof(hit.reqCtype));
    strlcpy(hit.vin, capture.vin, sizeof(hit.vin));
    hit.reqBytes = static_cast<uint32_t>(request->content_len);

    // TeslaDG/app/svc/stack.py starts the 4567 reverse with
    // reverse:http://192.0.2.1:4567 (TEST-NET, unreachable). Non-FSD
    // mothership requests therefore get mitmproxy's 502 instead of being
    // forwarded to Tesla.
    if (localPort(request) == kMothershipHttpPort) {
        closeConnection = true;
        discardRequestBody(request, 8192);
        char message[96];
        snprintf(message, sizeof(message),
                 "Cannot connect to %s: [Errno 113] No route to host",
                 kMothershipBlockedUpstream);
        sendProxyError(request, hit, responseBody, 502, message);
        return true;
    }

    if (dnsApInternetBlockReasons() != 0) {
        closeConnection = true;
        discardRequestBody(request, 8192);
        sendProxyError(request, hit, responseBody, 503, "ap_upstream_blocked");
        return true;
    }

    if (gProxyShuttingDown) {
        closeConnection = true;
        discardRequestBody(request, 8192);
        sendProxyError(request, hit, responseBody, 503, "proxy_stopping");
        return true;
    }

    if (!upstreamReady()) {
        closeConnection = true;
        discardRequestBody(request, 8192);
        sendProxyError(request, hit, responseBody, 502,
                       "Cannot connect to api-prd.vn.cloud.tesla.cn: upstream_unavailable");
        return true;
    }

    const size_t bodyLength = request->content_len;
    if (bodyLength > kMaxForwardBodyBytes) {
        closeConnection = true;
        discardRequestBody(request, 8192);
        finishJsonError(request, hit, responseBody, 413, "{\"error\":\"request_too_large\"}");
        return true;
    }

    // A second TLS context (upstream api-prd) needs ~20-24 KB contiguous
    // internal RAM. Under concurrent 443 handshakes, trying anyway causes
    // mbedtls_ssl_handshake -0x0084 / -0x7F00 (ALLOC_FAILED), esp-aes OOM spam
    // and panic/reboot. Fail this passthrough fast; FSD /feature_configuration
    // stays local and unaffected. (Same internal guard as aa0a69f8b; PSRAM, when
    // present, must additionally have room for the mbedTLS structs.)
    const bool lowHeap = ESP.getFreeHeap() < 45000 ||
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 20000 ||
        (ESP.getPsramSize() > 0 && ESP.getFreePsram() < 65536);
    if (lowHeap) {
        closeConnection = true;
        discardRequestBody(request, 8192);
        sendProxyError(request, hit, responseBody, 503, "low_heap");
        return true;
    }

    const uint32_t staIp = static_cast<uint32_t>(WiFi.localIP());
    if (upstreamCoolingDown(staIp, millis())) {
        closeConnection = true;
        discardRequestBody(request, 8192);
        sendProxyError(request, hit, responseBody, 502,
                       "Cannot connect to api-prd.vn.cloud.tesla.cn: upstream_unreachable");
        return true;
    }

    return false;
}

int forwardRequest(httpd_req_t* request, Capture& capture,
                   Hit& hit, char* responseBody, size_t responseCapacity,
                   bool& closeConnection) {
    if (fastRejectForward(request, capture, hit, responseBody, responseCapacity, closeConnection)) {
        return hit.status;
    }

    const size_t bodyLength = request->content_len;

    // TeslaDG reverse target for 443 is
    // reverse:https://api-prd.vn.cloud.tesla.cn@443 (keep_host_header=false).
    // The FSD hit path never reaches this function; it is only used for
    // observe mode and non-FSD passthrough.
    const bool chunkedReq = isChunkedRequest(request);
    if (chunkedReq) closeConnection = true;
    const char* upstreamHost = kApiPrdHost;

    const size_t urlLength = strlen(upstreamHost) + strlen(request->uri) + 16;
    char* url = static_cast<char*>(malloc(urlLength));
    if (!url) {
        closeConnection = true;
        discardRequestBody(request, 8192);
        finishJsonError(request, hit, responseBody, 500, "{\"error\":\"no_memory\"}");
        return 500;
    }
    snprintf(url, urlLength, "https://%s%s", upstreamHost, request->uri);

    UpstreamResponseHeaders respHeaders;
    esp_http_client_config_t config = {};
    config.url = url;
    config.method = upstreamMethod(request->method);
    config.timeout_ms = 2500;
#if ESP_IDF_VERSION_MAJOR >= 5
    config.addr_type = HTTP_ADDR_TYPE_INET;
#endif
    config.disable_auto_redirect = true;
    config.transport_type = HTTP_TRANSPORT_OVER_SSL;
    config.skip_cert_common_name_check = true;
    config.buffer_size = kUpstreamBufferRxBytes;
    config.buffer_size_tx = kUpstreamBufferTxBytes;
    config.event_handler = upstreamEventHandler;
    config.user_data = &respHeaders;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        closeConnection = true;
        free(url);
        discardRequestBody(request, 8192);
        sendProxyError(request, hit, responseBody, 502,
                       "Cannot connect to api-prd.vn.cloud.tesla.cn: init failed");
        return 502;
    }
    // Remove ESP-IDF's default "ESP32 HTTP Client/1.0" User-Agent so upstream
    // only sees what the vehicle sent (mitmproxy parity).
    esp_http_client_delete_header(client, "User-Agent");

    static const char* const kForwardHeaders[] = {
        "Host", "Authorization", "Content-Type", "Accept", "Accept-Encoding", "Accept-Language",
        "User-Agent", "Cookie", "Origin", "Referer",
        "Cache-Control", "Pragma", "Range", "If-Match", "If-None-Match", "If-Modified-Since", "If-Range",
        "X-Tesla-User-Agent", "Tesla-User-Agent", "X-Tesla-Client",
        "X-Tesla-Protocol", "X-Tesla-Protocol-Version", "X-Tesla-App-Version",
        "X-Tesla-Vehicle-Id", "X-Tesla-Signature", "X-Tesla-Timestamp",
        "X-Tesla-Token", "X-Tesla-Device-Id", "X-Tesla-Session-Id",
        "X-Tesla-Client-Id", "X-Tesla-Client-Version", "X-Tesla-Fleet-Key",
        "X-Tesla-User-Id", "X-Tesla-Vehicle-Name", "X-Tesla-Locale", "X-Tesla-Timezone",
        "X-Checksum", "X-Request-Id", "X-Correlation-Id", "X-Requested-With",
    };
    for (const char* name : kForwardHeaders) {
        // 443 rewrites Host to api-prd, matching the default mitmproxy reverse
        // mode (keep_host_header=false). 4567 never reaches here.
        if (strcasecmp(name, "Host") == 0) continue;
        const size_t length = httpd_req_get_hdr_value_len(request, name);
        if (length == 0) continue;
        char stackValue[128];
        char* value = (length < sizeof(stackValue)) ? stackValue : static_cast<char*>(malloc(length + 1));
        if (!value) continue;
        if (httpd_req_get_hdr_value_str(request, name, value, length + 1) == ESP_OK) {
            esp_http_client_set_header(client, name, value);
        }
        if (value != stackValue) free(value);
    }

    // Do NOT use esp_http_client_perform() here: it reads and discards the
    // response body internally. Open/write/fetch/read streams both request and
    // response bodies in bounded chunks without a 4 KB heap buffer (mitmproxy
    // reverse-mode parity).
    capture.requestBytes = static_cast<uint32_t>(bodyLength);
    capture.head[0] = '\0';
    capture.requestMd5[0] = '\0';
    bool clientReadFailed = false;
    bool policyBlocked = false;
    const auto stopForward = [&]() {
        policyBlocked = policyBlocked || dnsApInternetBlockReasons() != 0;
        return policyBlocked || gProxyShuttingDown;
    };
    gUpstreamTask = xTaskGetCurrentTaskHandle();
    esp_err_t error = stopForward() ? ESP_FAIL : esp_http_client_open(client, static_cast<int>(bodyLength));
    if (error == ESP_OK && bodyLength > 0) {
        md5_context_t reqMd5;
        esp_rom_md5_init(&reqMd5);
        char reqChunk[512];
        size_t remaining = bodyLength;
        size_t captured = 0;
        int timeouts = 0;
        while (remaining > 0 && !stopForward()) {
            const size_t want = remaining < sizeof(reqChunk) ? remaining : sizeof(reqChunk);
            const int received = httpd_req_recv(request, reqChunk, want);
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                if (++timeouts > 3) {
                    clientReadFailed = true;
                    error = ESP_FAIL;
                    break;
                }
                continue;
            }
            if (received <= 0) {
                clientReadFailed = true;
                error = ESP_FAIL;
                break;
            }
            const size_t receivedSize = static_cast<size_t>(received);
            esp_rom_md5_update(&reqMd5, reqChunk, static_cast<uint32_t>(received));
            captureBodyHead(reqChunk, receivedSize, capture, captured);
            if (stopForward()) break;
            const int written = esp_http_client_write(client, reqChunk, received);
            if (written != received) {
                error = ESP_FAIL;
                break;
            }
            remaining -= receivedSize;
        }
        if (remaining > 0 && error == ESP_OK) error = ESP_FAIL;
        if (error == ESP_OK) {
            uint8_t digest[16];
            esp_rom_md5_final(digest, &reqMd5);
            for (size_t i = 0; i < sizeof(digest); ++i) {
                snprintf(capture.requestMd5 + i * 2, 3, "%02x", static_cast<unsigned>(digest[i]));
            }
        }
    }
    if (error == ESP_OK && !stopForward() && esp_http_client_fetch_headers(client) < 0) {
        error = ESP_FAIL;
    }
    if (stopForward()) error = ESP_FAIL;
    gUpstreamTask = nullptr;

    strlcpy(hit.reqMd5, capture.requestMd5, sizeof(hit.reqMd5));
    strlcpy(hit.reqHead, capture.head, sizeof(hit.reqHead));

    if (error != ESP_OK) {
        closeConnection = true;
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        free(url);
        if (policyBlocked) {
            sendProxyError(request, hit, responseBody, 503, "ap_upstream_blocked");
            return 503;
        }
        if (bodyLength > 0 && !gProxyShuttingDown) discardRequestBody(request, 8192);
        if (clientReadFailed) {
            finishJsonError(request, hit, responseBody, 400, "{\"error\":\"request_read_failed\"}");
            return 400;
        }
        if (!gProxyShuttingDown) upstreamRecordFailure(millis());
        char message[128];
        snprintf(message, sizeof(message), "Cannot connect to %s: %s",
                 upstreamHost, esp_err_to_name(error));
        sendProxyError(request, hit, responseBody, 502, message);
        return 502;
    }
    upstreamRecordSuccess();

    int status = esp_http_client_get_status_code(client);
    if (status <= 0) status = 502;
    hit.status = static_cast<uint16_t>(status);
    char statusLine[32];
    httpd_resp_set_status(request, formatStatusLine(status, statusLine));

    if (respHeaders.contentType) {
        httpd_resp_set_type(request, respHeaders.contentType);
        strlcpy(hit.respCtype, respHeaders.contentType, sizeof(hit.respCtype));
    }
    for (size_t i = 0; i < respHeaders.count; ++i) {
        httpd_resp_set_hdr(request, respHeaders.entries[i].key, respHeaders.entries[i].value);
    }
    if (chunkedReq) {
        // esp_http_server does not de-chunk incoming Transfer-Encoding: chunked
        // bodies, so close after replying to keep framing aligned.
        httpd_resp_set_hdr(request, "Connection", "close");
    }

    const bool noBodyResponse = (request->method == HTTP_HEAD || status == 204 || status == 304);
    md5_context_t md5;
    esp_rom_md5_init(&md5);
    char buffer[512];
    size_t total = 0;
    size_t preview = 0;
    size_t bodyStored = 0;
    bool sendOk = true;
    bool streamCapped = false;
    const uint32_t streamStartMs = millis();
    if (!noBodyResponse) {
        int read = 0;
        while (!stopForward() &&
               (read = esp_http_client_read(client, buffer, sizeof(buffer))) > 0) {
            esp_rom_md5_update(&md5, buffer, static_cast<uint32_t>(read));
            if (preview < sizeof(hit.respHead) - 1) {
                const size_t copy = (sizeof(hit.respHead) - 1 - preview) < static_cast<size_t>(read)
                    ? (sizeof(hit.respHead) - 1 - preview) : static_cast<size_t>(read);
                copyPrintable(reinterpret_cast<const uint8_t*>(buffer), copy,
                              hit.respHead + preview, sizeof(hit.respHead) - preview);
                preview += copy;
            }
            if (responseBody && responseCapacity && bodyStored + 1 < responseCapacity) {
                const size_t copy = (responseCapacity - 1 - bodyStored) < static_cast<size_t>(read)
                    ? (responseCapacity - 1 - bodyStored) : static_cast<size_t>(read);
                memcpy(responseBody + bodyStored, buffer, copy);
                bodyStored += copy;
                responseBody[bodyStored] = '\0';
            }
            if (stopForward()) break;
            const esp_err_t sent = httpd_resp_send_chunk(request, buffer, read);
            total += static_cast<size_t>(read);
            if (sent != ESP_OK) {
                sendOk = false;
                closeConnection = true;
                break;
            }
            // Hard wall-clock & byte bounds keep a slow/huge upstream FLOW from
            // monopolizing the upstream worker.
            if (millis() - streamStartMs >= kMaxForwardDurationMs ||
                total >= kMaxForwardResponseBytes) {
                streamCapped = true;
                closeConnection = true;
                break;
            }
        }
    }
    if (stopForward()) {
        streamCapped = true;
        closeConnection = true;
    }
    if (sendOk && !streamCapped) {
        if (total > 0) {
            if (httpd_resp_send_chunk(request, nullptr, 0) != ESP_OK) closeConnection = true;
        } else {
            if (httpd_resp_send(request, nullptr, 0) != ESP_OK) closeConnection = true;
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(url);
    hit.bytes = static_cast<uint32_t>(total);
    if (total > 0) {
        uint8_t digest[16];
        esp_rom_md5_final(digest, &md5);
        for (size_t i = 0; i < sizeof(digest); ++i) {
            snprintf(hit.md5 + i * 2, 3, "%02x", static_cast<unsigned>(digest[i]));
        }
    }
    return status;
}

void proxyWorkerTask(void*) {
    while (true) {
        ProxyJob* job = nullptr;
        if (xQueueReceive(gProxyQueue, &job, portMAX_DELAY) != pdTRUE) continue;
        if (job == nullptr) break;  // shutdown sentinel

        char* respBody = job->observe
            ? static_cast<char*>(malloc(kLastResponseCapacity))
            : nullptr;
        const int fd = httpd_req_to_sockfd(job->req);
        bool closeConn = false;
        forwardRequest(job->req, job->capture, job->hit,
                       respBody, respBody ? kLastResponseCapacity : 0, closeConn);
        recordHit(job->hit, respBody);
        free(respBody);

        if (closeConn && fd >= 0) {
            // Half-close RX before returning the socket to httpd's select() so
            // httpd immediately detects EOF and closes the session cleanly.
            shutdown(fd, SHUT_RD);
        }
        httpd_req_async_handler_complete(job->req);
        delete job;

        portENTER_CRITICAL(&gProxyMux);
        if (gProxyInFlight > 0) --gProxyInFlight;
        portEXIT_CRITICAL(&gProxyMux);
    }

    portENTER_CRITICAL(&gProxyMux);
    gProxyWorkerTask = nullptr;
    portEXIT_CRITICAL(&gProxyMux);
    vTaskDelete(nullptr);
}

// Offload upstream reverse-proxy I/O off the 443 httpd task so /feature_configuration
// and new TLS handshakes on other sockets are never blocked by upstream latency.
esp_err_t dispatchForward(httpd_req_t* request, Hit& hit, Capture& capture, bool observe) {
    char* localResp = observe ? static_cast<char*>(malloc(kLastResponseCapacity)) : nullptr;
    bool closeConnection = false;
    if (fastRejectForward(request, capture, hit,
                          localResp, localResp ? kLastResponseCapacity : 0, closeConnection)) {
        recordHit(hit, localResp);
        free(localResp);
        return closeConnection ? ESP_FAIL : ESP_OK;
    }

    QueueHandle_t q = nullptr;
    bool stopping = false;
    portENTER_CRITICAL(&gProxyMux);
    q = gProxyQueue;
    stopping = gProxyShuttingDown;
    portEXIT_CRITICAL(&gProxyMux);

    if (q != nullptr && !stopping) {
        if (uxQueueSpacesAvailable(q) == 0) {
            discardRequestBody(request, 8192);
            sendProxyError(request, hit, localResp, 503, "proxy_busy");
            recordHit(hit, localResp);
            free(localResp);
            return ESP_FAIL;
        }
        auto* job = new (std::nothrow) ProxyJob;
        if (!job) {
            discardRequestBody(request, 8192);
            finishJsonError(request, hit, localResp, 500, "{\"error\":\"no_memory\"}");
            recordHit(hit, localResp);
            free(localResp);
            return ESP_FAIL;
        }
        httpd_req_t* asyncReq = nullptr;
        if (httpd_req_async_handler_begin(request, &asyncReq) != ESP_OK) {
            delete job;
            discardRequestBody(request, 8192);
            sendProxyError(request, hit, localResp, 503, "async_unavailable");
            recordHit(hit, localResp);
            free(localResp);
            return ESP_FAIL;
        }
        job->req = asyncReq;
        job->hit = hit;
        job->capture = capture;
        job->observe = observe;

        portENTER_CRITICAL(&gProxyMux);
        ++gProxyInFlight;
        portEXIT_CRITICAL(&gProxyMux);

        if (xQueueSend(q, &job, 0) != pdTRUE) {
            portENTER_CRITICAL(&gProxyMux);
            if (gProxyInFlight > 0) --gProxyInFlight;
            portEXIT_CRITICAL(&gProxyMux);
            const int fd = httpd_req_to_sockfd(asyncReq);
            discardRequestBody(asyncReq, 8192);
            sendProxyError(asyncReq, hit, localResp, 503, "proxy_busy");
            recordHit(hit, localResp);
            if (fd >= 0) shutdown(fd, SHUT_RD);
            httpd_req_async_handler_complete(asyncReq);
            delete job;
            free(localResp);
            return ESP_OK;
        }
        free(localResp);
        return ESP_OK;
    }

    // Fallback synchronous path if worker was not started.
    forwardRequest(request, capture, hit,
                   localResp, localResp ? kLastResponseCapacity : 0, closeConnection);
    recordHit(hit, localResp);
    free(localResp);
    return closeConnection ? ESP_FAIL : ESP_OK;
}

const char* tlsAlertName(int alert) {
    switch (alert) {
        case 0:   return "close_notify";
        case 10:  return "unexpected_message";
        case 20:  return "bad_record_mac";
        case 21:  return "decryption_failed";
        case 22:  return "record_overflow";
        case 40:  return "handshake_failure";
        case 42:  return "bad_certificate";
        case 43:  return "unsupported_certificate";
        case 44:  return "certificate_revoked";
        case 45:  return "certificate_expired";
        case 46:  return "certificate_unknown";
        case 47:  return "illegal_parameter";
        case 48:  return "unknown_ca";
        case 49:  return "access_denied";
        case 50:  return "decode_error";
        case 51:  return "decrypt_error";
        case 70:  return "protocol_version";
        case 71:  return "insufficient_security";
        case 80:  return "internal_error";
        case 86:  return "inappropriate_fallback";
        case 90:  return "user_canceled";
        case 109: return "missing_extension";
        case 110: return "unsupported_extension";
        case 112: return "unrecognized_name";
        case 116: return "certificate_required";
        case 120: return "no_application_protocol";
        default:  return "other";
    }
}

}  // namespace

// ── public API ──────────────────────────────────────────────────────────────

static void storeHandshakeLogError(const char* format, va_list args) {
    char raw[160];
    va_list copy;
    va_copy(copy, args);
    vsnprintf(raw, sizeof(raw), format, copy);
    va_end(copy);
    // The ESP_LOG line includes ANSI colour/tag prefixes. Strip escapes and
    // control bytes so the dashboard does not show terminal garbage.
    char clean[96];
    size_t out = 0;
    for (const char* p = raw; *p && out + 1 < sizeof(clean); ++p) {
        if (*p == '\x1b') {
            ++p;
            while (*p && !(*p >= '@' && *p <= '~')) ++p;
            continue;
        }
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c < 0x20 || c >= 0x7f) continue;
        clean[out++] = static_cast<char>(c);
    }
    clean[out] = '\0';
    const char* detail = strstr(clean, "mbedtls_ssl_handshake returned");
    if (!detail) detail = strstr(clean, "esp_tls_create_server_session failed");
    noteHandshakeFailure(detail ? detail : clean);
}

static int fsdLogHook(const char* format, va_list args) {
    const TaskHandle_t curTask = xTaskGetCurrentTaskHandle();
    const bool isUpstream = (gUpstreamTask != nullptr && curTask == gUpstreamTask);
    if (format && !isUpstream) {
        if (strstr(format, "mbedtls_ssl_handshake returned")) {
            storeHandshakeLogError(format, args);
            gLastMbedtlsFailureMs = millis();
        } else if (strstr(format, "esp_tls_create_server_session failed")) {
            // esp_https_server logs this right after the mbedtls line; count
            // the attempt once but still catch failures without an mbedtls log.
            if (gLastMbedtlsFailureMs == 0 || millis() - gLastMbedtlsFailureMs > 1000) {
                noteHandshakeFailure("esp_tls_create_server_session failed");
            }
        }
    }
    return gPreviousLog ? gPreviousLog(format, args) : vprintf(format, args);
}

void installLogHook() {
    if (!gPreviousLog) gPreviousLog = esp_log_set_vprintf(fsdLogHook);
}

void noteHandshakeFailure(const char* error) {
    const uint32_t now = millis();
    const char* message = error ? error : "handshake_failed";
    char ts[32] = {};
    formatTimestamp(ts);
    portENTER_CRITICAL(&gHandshakeMux);
    const bool newFailure = gHandshakeFail == 0 || now - gLastHandshakeFailMs > 1000;
    const bool sameAlertBurst = !newFailure &&
        strstr(message, "-0x7780") != nullptr &&
        strstr(gLastHandshakeError, "-0x7780") != nullptr;
    const bool newMessage = !sameAlertBurst && strcmp(message, gLastHandshakeError) != 0;
    ++gHandshakeFail;
    if (newFailure || newMessage ||
        (!sameAlertBurst && strlen(message) > strlen(gLastHandshakeError))) {
        strlcpy(gLastHandshakeError, message, sizeof(gLastHandshakeError));
    }
    gLastHandshakeFailMs = now;

    // Count every attempt, but add at most one ring entry per second unless
    // the error text changes, so a retrying car cannot evict handshake_ok.
    if (newFailure || newMessage) {
        Handshake& handshake = gHandshakes[gHandshakeWrite];
        handshake = Handshake{};
        strlcpy(handshake.event, "handshake_failed", sizeof(handshake.event));
        strlcpy(handshake.ts, ts, sizeof(handshake.ts));
        strlcpy(handshake.error, message, sizeof(handshake.error));
        gHandshakeWrite = (gHandshakeWrite + 1) % kHandshakeCapacity;
        if (gHandshakeCount < kHandshakeCapacity) ++gHandshakeCount;
    }
    portEXIT_CRITICAL(&gHandshakeMux);
}

void noteHandshakeError(const esp_tls_t* tls) {
    char client[46] = {};
    char detail[64] = {};
    int alert = -1;
    if (tls) {
#if ESP_IDF_VERSION_MAJOR >= 5
        int sock = -1;
        if (esp_tls_get_conn_sockfd(const_cast<esp_tls_t*>(tls), &sock) == ESP_OK) {
            socketAddress(sock, client, sizeof(client));
        }
        auto* ssl = static_cast<mbedtls_ssl_context*>(
            esp_tls_get_ssl_context(const_cast<esp_tls_t*>(tls)));
#else
        socketAddress(tls->sockfd, client, sizeof(client));
        auto* ssl = &const_cast<esp_tls_t*>(tls)->ssl;
#endif
#if defined(CONFIG_ESP_TLS_USING_MBEDTLS) && ESP_IDF_VERSION_MAJOR >= 6
        if (ssl) {
            alert = mbedtls_ssl_get_fatal_alert(ssl);
        }
#else
        (void)ssl;
#endif
    }

    // Counting/ring logic stays exactly aa0a69f8b: the log hook already recorded
    // "mbedtls_ssl_handshake returned -0x...." (or esp_tls_create_server_session
    // failed) just before this callback. Never add/count a failure here, and
    // never rewrite non-alert errors such as -0x0084 (esp-aes/SSL alloc) or
    // -0x7280 (peer EOF): in_fatal_alert_type is 0 for those, which would
    // otherwise be misreported as "-0x7780 alert=0(close_notify)".
    if (alert >= 0) {
        snprintf(detail, sizeof(detail), "handshake -0x7780 alert=%d(%s)",
                 alert, tlsAlertName(alert));
    }
    const uint32_t now = millis();
    portENTER_CRITICAL(&gHandshakeMux);
    const bool recent = gHandshakeFail > 0 && (now - gLastHandshakeFailMs <= 1000);
    if (recent && gHandshakeCount > 0) {
        const size_t latest = (gHandshakeWrite + kHandshakeCapacity - 1) % kHandshakeCapacity;
        Handshake& entry = gHandshakes[latest];
        if (strcmp(entry.event, "handshake_failed") == 0) {
            if (client[0] && !entry.client[0]) {
                strlcpy(entry.client, client, sizeof(entry.client));
            }
            if (detail[0] && strstr(entry.error, "-0x7780") != nullptr &&
                strcmp(entry.error, detail) != 0) {
                strlcpy(entry.error, detail, sizeof(entry.error));
                if (strstr(gLastHandshakeError, "-0x7780") != nullptr) {
                    strlcpy(gLastHandshakeError, detail, sizeof(gLastHandshakeError));
                }
            }
        }
    }
    portEXIT_CRITICAL(&gHandshakeMux);
}

void getHandshakeSummary(uint32_t* ok, uint32_t* fail,
                         char* tls, size_t tlsCapacity,
                         char* error, size_t errorCapacity) {
    portENTER_CRITICAL(&gHandshakeMux);
    if (ok) *ok = gHandshakeOk;
    if (fail) *fail = gHandshakeFail;
    if (tls && tlsCapacity > 0) strlcpy(tls, gLastHandshakeTls, tlsCapacity);
    if (error && errorCapacity > 0) strlcpy(error, gLastHandshakeError, errorCapacity);
    portEXIT_CRITICAL(&gHandshakeMux);
}

void startProxyWorker() {
    portENTER_CRITICAL(&gProxyMux);
    gProxyShuttingDown = false;
    const bool alreadyUp = (gProxyQueue != nullptr && gProxyWorkerTask != nullptr);
    portEXIT_CRITICAL(&gProxyMux);
    if (alreadyUp) return;

    QueueHandle_t q = gProxyQueue;
    if (!q) {
        q = xQueueCreate(2, sizeof(ProxyJob*));
        if (!q) return;
        portENTER_CRITICAL(&gProxyMux);
        gProxyQueue = q;
        portEXIT_CRITICAL(&gProxyMux);
    }

    TaskHandle_t task = nullptr;
    if (xTaskCreatePinnedToCore(proxyWorkerTask, "FsdProxy", 8192, nullptr, 1, &task, 1) == pdPASS) {
        portENTER_CRITICAL(&gProxyMux);
        gProxyWorkerTask = task;
        portEXIT_CRITICAL(&gProxyMux);
    }
}

void stopProxyWorker() {
    portENTER_CRITICAL(&gProxyMux);
    gProxyShuttingDown = true;
    QueueHandle_t q = gProxyQueue;
    portEXIT_CRITICAL(&gProxyMux);

    // Drain all queued/active async proxy jobs BEFORE httpd_ssl_stop() frees sock_db.
    // Timeout exceeds worst-case upstream connect (2.5 s) + stream cap (15 s) so
    // FsdProxy can never call httpd_req_async_handler_complete() after free.
    const uint32_t deadline = millis() + 22000;
    while (static_cast<int32_t>(deadline - millis()) > 0) {
        portENTER_CRITICAL(&gProxyMux);
        const uint32_t inflight = gProxyInFlight;
        portEXIT_CRITICAL(&gProxyMux);
        if (inflight == 0) break;
        if (esp_task_wdt_status(nullptr) == ESP_OK) esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (q) {
        ProxyJob* sentinel = nullptr;
        xQueueSend(q, &sentinel, pdMS_TO_TICKS(200));
        const uint32_t exitDeadline = millis() + 500;
        while (static_cast<int32_t>(exitDeadline - millis()) > 0) {
            portENTER_CRITICAL(&gProxyMux);
            const bool done = (gProxyWorkerTask == nullptr);
            portEXIT_CRITICAL(&gProxyMux);
            if (done) break;
            if (esp_task_wdt_status(nullptr) == ESP_OK) esp_task_wdt_reset();
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        portENTER_CRITICAL(&gProxyMux);
        const bool canDeleteQ = (gProxyWorkerTask == nullptr);
        if (canDeleteQ) gProxyQueue = nullptr;
        portEXIT_CRITICAL(&gProxyMux);
        if (canDeleteQ) vQueueDelete(q);
    }
}

void noteHandshake(const esp_tls_t* tls) {
    char client[46] = {};
    char version[16] = {};
    char cipher[48] = {};
    char alpn[16] = {};
    char ts[32] = {};
    formatTimestamp(ts);
    if (tls) {
#if ESP_IDF_VERSION_MAJOR >= 5
        // esp_tls_t is opaque in IDF 5.x; use the public accessors.
        int socket = -1;
        if (esp_tls_get_conn_sockfd(const_cast<esp_tls_t*>(tls), &socket) == ESP_OK) {
            socketAddress(socket, client, sizeof(client));
        }
        auto* ssl = static_cast<mbedtls_ssl_context*>(
            esp_tls_get_ssl_context(const_cast<esp_tls_t*>(tls)));
#else
        socketAddress(tls->sockfd, client, sizeof(client));
        auto* ssl = &const_cast<esp_tls_t*>(tls)->ssl;
#endif
#if defined(CONFIG_ESP_TLS_USING_MBEDTLS)
        if (ssl) {
            const char* value = mbedtls_ssl_get_version(ssl);
            if (value) strlcpy(version, value, sizeof(version));
            value = mbedtls_ssl_get_ciphersuite(ssl);
            if (value) strlcpy(cipher, value, sizeof(cipher));
            value = mbedtls_ssl_get_alpn_protocol(ssl);
            if (value) strlcpy(alpn, value, sizeof(alpn));
        }
#endif
    }

    portENTER_CRITICAL(&gHandshakeMux);
    ++gHandshakeOk;
    gLastHandshakeOkMs = millis();
    strlcpy(gLastHandshakeClient, client, sizeof(gLastHandshakeClient));
    strlcpy(gLastHandshakeTls, version, sizeof(gLastHandshakeTls));
    strlcpy(gLastHandshakeCipher, cipher, sizeof(gLastHandshakeCipher));
    strlcpy(gLastHandshakeAlpn, alpn, sizeof(gLastHandshakeAlpn));

    Handshake& handshake = gHandshakes[gHandshakeWrite];
    handshake = Handshake{};
    strlcpy(handshake.event, "handshake_ok", sizeof(handshake.event));
    strlcpy(handshake.ts, ts, sizeof(handshake.ts));
    strlcpy(handshake.client, client, sizeof(handshake.client));
    strlcpy(handshake.tls, version, sizeof(handshake.tls));
    strlcpy(handshake.cipher, cipher, sizeof(handshake.cipher));
    strlcpy(handshake.alpn, alpn, sizeof(handshake.alpn));
    gHandshakeWrite = (gHandshakeWrite + 1) % kHandshakeCapacity;
    if (gHandshakeCount < kHandshakeCapacity) ++gHandshakeCount;
    portEXIT_CRITICAL(&gHandshakeMux);
}

void begin() {
    gStateMutex = xSemaphoreCreateMutex();
    if (!gStateMutex) return;

    // Older firmware persisted hit/handshake JSONL here. Logs are RAM-only
    // now, so reclaim that space for payload storage.
    SPIFFS.remove("/fsd_hits.jsonl");
    SPIFFS.remove("/fsd_handshake.log");
    if (!SPIFFS.exists(kPayloadPath) && SPIFFS.exists(kPayloadBackupPath)) {
        SPIFFS.rename(kPayloadBackupPath, kPayloadPath);
    }

    cJSON* defaultJson = cJSON_Parse(kFsdPayloadDefault);
    const size_t defaultFields = countFields(defaultJson);
    cJSON_Delete(defaultJson);

    lockState();
    strlcpy(gState.payload, kFsdPayloadDefault, sizeof(gState.payload));
    gState.bytes = strlen(kFsdPayloadDefault);
    gState.fields = defaultFields;
    md5Hex(gState.payload, gState.bytes, gState.md5);
    unlockState();

    Preferences prefs;
    bool migrateHost = false;
    if (prefs.begin("fsdmock", true)) {
        Settings settings;
        const size_t length = prefs.getBytesLength("cfg");
        if (length == sizeof(settings) &&
            prefs.getBytes("cfg", &settings, sizeof(settings)) == sizeof(settings) &&
            settings.magic == kSettingsMagic) {
            lockState();
            gState.enabled = settings.enabled != 0;
            gState.autoOffOnHit = (settings.flags & kSettingFlagAutoOffSet) != 0
                ? (settings.autoOffOnHit != 0)
                : true;
            gState.mode = settings.mode == static_cast<uint8_t>(Mode::Observe) ? Mode::Observe : Mode::Reply;
            if (validHostList(settings.host)) {
                if (strcasecmp(settings.host, kLegacyDefaultHosts) == 0) {
                    strlcpy(gState.host, kDefaultHosts, sizeof(gState.host));
                    migrateHost = true;
                } else {
                    strlcpy(gState.host, settings.host, sizeof(gState.host));
                }
            }
            unlockState();
        }
        // Restore the persisted 下发记录 ring (survives reboot).
        ConfigLogBlob log = {};
        if (prefs.getBytesLength("cfglog") == sizeof(log) &&
            prefs.getBytes("cfglog", &log, sizeof(log)) == sizeof(log) &&
            log.magic == kConfigLogMagic &&
            log.count <= kConfigHistoryCapacity &&
            log.write < kConfigHistoryCapacity) {
            portENTER_CRITICAL(&gConfigMux);
            memcpy(gConfigHistory, log.items, sizeof(gConfigHistory));
            gConfigCount = log.count;
            gConfigWrite = log.write;
            portEXIT_CRITICAL(&gConfigMux);
        }
        prefs.end();
    }

    char* stored = static_cast<char*>(malloc(kPayloadCapacity));
    char* compacted = static_cast<char*>(malloc(kPayloadCapacity));
    if (stored && compacted) {
        size_t storedLength = 0;
        if (loadPayloadFile(stored, kPayloadCapacity, storedLength)) {
            char error[48];
            size_t length = 0;
            size_t fields = 0;
            if (compactJson(stored, storedLength, compacted, kPayloadCapacity, length, fields, error, sizeof(error))) {
                lockState();
                memcpy(gState.payload, compacted, length + 1);
                gState.bytes = length;
                gState.fields = fields;
                md5Hex(gState.payload, gState.bytes, gState.md5);
                unlockState();
            }
        }
    }
    free(stored);
    free(compacted);
    if (migrateHost) persistSettings();
    // Listeners (443/4567) are started lazily by client_config only when
    // enabled() is true; gActive becomes true via setListenersReady(true).
    gActive = false;
    syncDnsRedirect();
}

void setListenersReady(bool ready) {
    gActive = ready;
    syncDnsRedirect();
}

void resyncDnsRedirect() {
    syncDnsRedirect();
}

void noteAutoDisabled() {
    portENTER_CRITICAL(&gHitMux);
    ++gAutoDisabledTotal;
    portEXIT_CRITICAL(&gHitMux);
}

bool isFeatureConfigurationUri(const char* uri) {
    if (!uri) return false;
    const char* found = strstr(uri, "/feature_configuration");
    if (!found) return false;
    const char* end = found + strlen("/feature_configuration");
    if (*end == '\0' || *end == '?' || *end == '#') return true;
    if (*end == '/') {
        ++end;
        return *end == '\0' || *end == '?' || *end == '#';
    }
    return false;
}

esp_err_t handleRequest(httpd_req_t* request) {
    if (!enabled()) return forwardPassthrough(request);

    Hit hit;
    Capture capture;
    parseRequestMeta(request, hit, capture);
    const bool observe = mode() == Mode::Observe;
    strlcpy(hit.kind, observe ? "OBSERVE" : "HIT", sizeof(hit.kind));
    strlcpy(hit.mode, observe ? "observe" : "reply", sizeof(hit.mode));

    if (observe) {
        return dispatchForward(request, hit, capture, true);
    }

    if (request->content_len > kMaxFsdRequestBodyBytes) {
        const char* errorBody = "{\"error\":\"request_too_large\"}";
        finishJsonError(request, hit, nullptr, 413, errorBody);
        recordHit(hit, errorBody);
        // Return ESP_FAIL so esp_http_server closes the socket immediately
        // instead of purging megabytes 32 B at a time in httpd_req_delete().
        return ESP_FAIL;
    }

    const bool bodyOk = drainRequestBody(request, capture);
    strlcpy(hit.reqMd5, capture.requestMd5, sizeof(hit.reqMd5));
    strlcpy(hit.reqCtype, capture.ctype, sizeof(hit.reqCtype));
    strlcpy(hit.reqHead, capture.head, sizeof(hit.reqHead));
    hit.reqBytes = capture.requestBytes;
    strlcpy(hit.vin, capture.vin, sizeof(hit.vin));
    if (!bodyOk) {
        const char* errorBody = "{\"error\":\"request_read_failed\"}";
        finishJsonError(request, hit, nullptr, 400, errorBody);
        recordHit(hit, errorBody);
        return ESP_FAIL;
    }

    char* body = static_cast<char*>(malloc(kPayloadCapacity));
    if (!body) {
        const char* errorBody = "{\"error\":\"no_memory\"}";
        finishJsonError(request, hit, nullptr, 500, errorBody);
        recordHit(hit, errorBody);
        return ESP_FAIL;
    }
    char checksum[33];
    size_t bytes = 0;
    bool autoOff = false;
    lockState();
    bytes = gState.bytes;
    memcpy(body, gState.payload, bytes + 1);
    strlcpy(checksum, gState.md5, sizeof(checksum));
    autoOff = gState.autoOffOnHit;
    unlockState();

    const bool chunked = isChunkedRequest(request);
    const bool triggerAutoOff = autoOff && request->method != HTTP_HEAD;
    char statusLine[32];
    httpd_resp_set_status(request, formatStatusLine(200, statusLine));
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "X-Checksum", checksum);
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    if (chunked) {
        // esp_http_server does not de-chunk incoming Transfer-Encoding: chunked
        // bodies, so close after replying to keep framing aligned.
        httpd_resp_set_hdr(request, "Connection", "close");
    }
    // RFC 9110 / mitmproxy parity: HEAD gets Content-Length: <bytes> with no body.
    const esp_err_t result = request->method == HTTP_HEAD
        ? httpd_resp_send(request, nullptr, static_cast<ssize_t>(bytes))
        : httpd_resp_send(request, body, static_cast<ssize_t>(bytes));

    hit.status = 200;
    hit.bytes = static_cast<uint32_t>(bytes);
    strlcpy(hit.md5, checksum, sizeof(hit.md5));
    strlcpy(hit.respCtype, "application/json", sizeof(hit.respCtype));
    copyPrintable(reinterpret_cast<const uint8_t*>(body), bytes, hit.respHead, sizeof(hit.respHead));
    recordHit(hit, body);
    free(body);
    if (result == ESP_OK && triggerAutoOff) {
        client_config::scheduleAutoOff();
    }
    return (result != ESP_OK || chunked) ? ESP_FAIL : ESP_OK;
}

esp_err_t forwardPassthrough(httpd_req_t* request) {
    Hit hit;
    Capture capture;
    parseRequestMeta(request, hit, capture);
    strlcpy(hit.kind, "FLOW", sizeof(hit.kind));
    strlcpy(hit.mode, "flow", sizeof(hit.mode));

    // Offload upstream I/O to FsdProxy worker so 443 never stalls /feature_configuration.
    return dispatchForward(request, hit, capture, false);
}

bool enabled() {
    lockState();
    const bool value = gState.enabled;
    unlockState();
    return value;
}

bool autoOffOnHit() {
    lockState();
    const bool value = gState.autoOffOnHit;
    unlockState();
    return value;
}

Mode mode() {
    lockState();
    const Mode value = gState.mode;
    unlockState();
    return value;
}

const char* modeName() {
    return mode() == Mode::Observe ? "observe" : "reply";
}

const char* host() {
    return gState.host;
}

size_t payloadBytes() {
    lockState();
    const size_t value = gState.bytes;
    unlockState();
    return value;
}

size_t payloadFields() {
    lockState();
    const size_t value = gState.fields;
    unlockState();
    return value;
}

const char* payloadMd5() {
    return gState.md5;
}

size_t copyPayload(char* output, size_t capacity, char* md5Out, size_t* fieldsOut) {
    lockState();
    const size_t bytes = gState.bytes;
    if (md5Out) strlcpy(md5Out, gState.md5, 33);
    if (fieldsOut) *fieldsOut = gState.fields;
    size_t copied = bytes;
    if (output && capacity > 0) {
        copied = bytes < capacity - 1 ? bytes : capacity - 1;
        memcpy(output, gState.payload, copied);
        output[copied] = '\0';
    }
    unlockState();
    return copied;
}

bool setEnabled(bool value) {
    lockState();
    gState.enabled = value;
    unlockState();
    syncDnsRedirect();
    return persistSettings();
}

bool setAutoOffOnHit(bool value) {
    lockState();
    gState.autoOffOnHit = value;
    unlockState();
    return persistSettings();
}

bool setMode(const char* value) {
    if (!value) return false;
    Mode next;
    if (strcasecmp(value, "reply") == 0) next = Mode::Reply;
    else if (strcasecmp(value, "observe") == 0) next = Mode::Observe;
    else return false;
    lockState();
    gState.mode = next;
    unlockState();
    return persistSettings();
}

bool setHost(const char* value) {
    char trimmed[kHostCapacity];
    trimCopy(value, trimmed, sizeof(trimmed));
    char normalized[kHostCapacity];
    size_t out = 0;
    bool space = false;
    for (const char* p = trimmed; *p && out + 1 < sizeof(normalized); ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (isspace(c) || c == ',' || c == ';') {
            space = out > 0;
            continue;
        }
        if (space && out + 1 < sizeof(normalized)) normalized[out++] = ' ';
        space = false;
        normalized[out++] = static_cast<char>(tolower(c));
    }
    normalized[out] = '\0';
    if (!validHostList(normalized)) return false;
    lockState();
    strlcpy(gState.host, normalized, sizeof(gState.host));
    unlockState();
    syncDnsRedirect();
    return persistSettings();
}

void recordConfigChange(const char* source, size_t bytes, size_t fields,
                        const char* md5, const char* prevMd5) {
    ConfigChange record;
    formatTimestamp(record.ts);
    strlcpy(record.source, source && source[0] ? source : "api", sizeof(record.source));
    strlcpy(record.md5, md5, sizeof(record.md5));
    strlcpy(record.prevMd5, prevMd5, sizeof(record.prevMd5));
    record.bytes = static_cast<uint32_t>(bytes);
    record.fields = static_cast<uint16_t>(fields);
    portENTER_CRITICAL(&gConfigMux);
    gConfigHistory[gConfigWrite] = record;
    gConfigWrite = (gConfigWrite + 1) % kConfigHistoryCapacity;
    if (gConfigCount < kConfigHistoryCapacity) ++gConfigCount;
    portEXIT_CRITICAL(&gConfigMux);
    persistConfigLog();
}

bool setPayload(const char* json, size_t length, char* error, size_t errorSize,
                const char* source) {
    if (error && errorSize) error[0] = '\0';
    char* compacted = static_cast<char*>(malloc(kPayloadCapacity));
    if (!compacted) {
        snprintf(error, errorSize, "no_memory");
        return false;
    }
    size_t compactLength = 0;
    size_t fields = 0;
    if (!compactJson(json, length, compacted, kPayloadCapacity, compactLength, fields, error, errorSize)) {
        free(compacted);
        return false;
    }
    if (!savePayloadFile(compacted, compactLength)) {
        free(compacted);
        snprintf(error, errorSize, "storage");
        return false;
    }
    char prevMd5[33];
    char md5[33];
    lockState();
    strlcpy(prevMd5, gState.md5, sizeof(prevMd5));
    memcpy(gState.payload, compacted, compactLength + 1);
    gState.bytes = compactLength;
    gState.fields = fields;
    md5Hex(gState.payload, gState.bytes, gState.md5);
    strlcpy(md5, gState.md5, sizeof(md5));
    unlockState();
    recordConfigChange(source, compactLength, fields, md5, prevMd5);
    free(compacted);
    return true;
}

bool loadPreset(const char* name, char* error, size_t errorSize) {
    if (!name) {
        snprintf(error, errorSize, "unknown_preset");
        return false;
    }
    if (strcasecmp(name, "default") == 0 || strcasecmp(name, "esp32") == 0 ||
        strcasecmp(name, "654") == 0 || strcasecmp(name, "autoshift") == 0) {
        return setPayload(kFsdPayloadDefault, strlen(kFsdPayloadDefault), error, errorSize, "preset");
    }
    if (strcasecmp(name, "windows") == 0 || strcasecmp(name, "pc") == 0 ||
        strcasecmp(name, "602") == 0) {
        return setPayload(kFsdPayloadPc, strlen(kFsdPayloadPc), error, errorSize, "preset");
    }
    if (strcasecmp(name, "16f") == 0 || strcasecmp(name, "payload-16f") == 0) {
        return setPayload(kFsdPayload16f, strlen(kFsdPayload16f), error, errorSize, "preset");
    }
    if (strcasecmp(name, "282b") == 0 || strcasecmp(name, "payload-282b") == 0 ||
        strcasecmp(name, "official") == 0) {
        return setPayload(kFsdPayload282b, strlen(kFsdPayload282b), error, errorSize, "preset");
    }
    if (strcasecmp(name, "2031") == 0 || strcasecmp(name, "payload-2031") == 0 ||
        strcasecmp(name, "1990") == 0 || strcasecmp(name, "2020") == 0 ||
        strcasecmp(name, "cn") == 0 || strcasecmp(name, "cn62") == 0 ||
        strcasecmp(name, "full") == 0) {
        return setPayload(kFsdPayloadCnFull, strlen(kFsdPayloadCnFull), error, errorSize, "preset");
    }
    if (strcasecmp(name, "1466") == 0 || strcasecmp(name, "payload-1466") == 0 ||
        strcasecmp(name, "cn46") == 0) {
        return setPayload(kFsdPayloadCn46, strlen(kFsdPayloadCn46), error, errorSize, "preset");
    }
    snprintf(error, errorSize, "unknown_preset");
    return false;
}

void clearHits() {
    portENTER_CRITICAL(&gHitMux);
    gHitCount = 0;
    gHitWrite = 0;
    gHitTotal = 0;
    gObserveTotal = 0;
    gFlowTotal = 0;
    memset(gHits, 0, sizeof(gHits));
    portEXIT_CRITICAL(&gHitMux);
    portENTER_CRITICAL(&gLastResponseMux);
    gLastResponse = LastResponse{};
    portEXIT_CRITICAL(&gLastResponseMux);
}

void clearHandshakes() {
    portENTER_CRITICAL(&gHandshakeMux);
    gHandshakeCount = 0;
    gHandshakeWrite = 0;
    gHandshakeOk = 0;
    gHandshakeFail = 0;
    gLastHandshakeOkMs = 0;
    gLastHandshakeFailMs = 0;
    gLastMbedtlsFailureMs = 0;
    gLastHandshakeClient[0] = '\0';
    gLastHandshakeTls[0] = '\0';
    gLastHandshakeCipher[0] = '\0';
    gLastHandshakeAlpn[0] = '\0';
    gLastHandshakeError[0] = '\0';
    memset(gHandshakes, 0, sizeof(gHandshakes));
    portEXIT_CRITICAL(&gHandshakeMux);
}

uint32_t hitCount() {
    portENTER_CRITICAL(&gHitMux);
    const uint32_t value = gHitTotal;
    portEXIT_CRITICAL(&gHitMux);
    return value;
}

uint32_t observeCount() {
    portENTER_CRITICAL(&gHitMux);
    const uint32_t value = gObserveTotal;
    portEXIT_CRITICAL(&gHitMux);
    return value;
}

uint32_t flowCount() {
    portENTER_CRITICAL(&gHitMux);
    const uint32_t value = gFlowTotal;
    portEXIT_CRITICAL(&gHitMux);
    return value;
}

uint32_t dnsRedirectCount() {
    uint32_t total = 0;
    char last[128];
    dnsGetFsdRedirectStats(&total, last, sizeof(last));
    return total;
}

uint32_t autoDisabledCount() {
    portENTER_CRITICAL(&gHitMux);
    const uint32_t value = gAutoDisabledTotal;
    portEXIT_CRITICAL(&gHitMux);
    return value;
}

static bool readRecentHit(size_t index, Hit& out);
static bool readRecentHandshake(size_t index, Handshake& out);

void appendStatusJson(Print& out) {
    bool enabledNow;
    bool autoOffNow;
    Mode modeNow;
    char hostNow[kHostCapacity];
    char md5Now[33];
    size_t bytesNow;
    size_t fieldsNow;
    lockState();
    enabledNow = gState.enabled;
    autoOffNow = gState.autoOffOnHit;
    modeNow = gState.mode;
    strlcpy(hostNow, gState.host, sizeof(hostNow));
    strlcpy(md5Now, gState.md5, sizeof(md5Now));
    bytesNow = gState.bytes;
    fieldsNow = gState.fields;
    unlockState();

    uint32_t dnsTotal = 0;
    char dnsLast[128] = {};
    dnsGetFsdRedirectStats(&dnsTotal, dnsLast, sizeof(dnsLast));

    out.print("\"fsdMock\":{");
    out.print("\"nowMs\":"); out.print(static_cast<unsigned long>(millis()));
    out.print(",\"active\":"); out.print(gActive ? "true" : "false");
    out.print(",\"enabled\":"); out.print(enabledNow ? "true" : "false");
    out.print(",\"autoOffOnHit\":"); out.print(autoOffNow ? "true" : "false");
    out.print(",\"autoOffPending\":"); out.print(client_config::autoOffPending() ? "true" : "false");
    out.print(",\"autoOffRemainingSec\":"); out.print(static_cast<unsigned long>(client_config::autoOffRemainingSec()));
    out.print(",\"autoDisabledCount\":"); out.print(static_cast<unsigned long>(autoDisabledCount()));
    out.print(",\"fsd443\":"); out.print(client_config::fsdHttpsStarted() ? "true" : "false");
    out.print(",\"fsd4567\":"); out.print(client_config::mothershipHttpStarted() ? "true" : "false");
    out.print(",\"recoveries\":"); out.print(static_cast<unsigned long>(client_config::recoveryCount()));
    out.print(",\"lastError\":\""); jsonEscape(out, client_config::lastListenerError()); out.print("\"");
    out.print(",\"mode\":\""); out.print(modeNow == Mode::Observe ? "observe" : "reply"); out.print("\"");
    out.print(",\"host\":\""); jsonEscape(out, hostNow); out.print("\"");
    out.print(",\"bytes\":"); out.print(static_cast<unsigned>(bytesNow));
    out.print(",\"fields\":"); out.print(static_cast<unsigned>(fieldsNow));
    out.print(",\"md5\":\""); out.print(md5Now); out.print("\"");
    out.print(",\"canonical\":");
    out.print((bytesNow == kFsdCanonicalBytes && strcmp(md5Now, kFsdCanonicalMd5) == 0) ? "true" : "false");
    out.print(",\"hits\":"); out.print(static_cast<unsigned>(hitCount()));
    out.print(",\"observes\":"); out.print(static_cast<unsigned>(observeCount()));
    out.print(",\"flows\":"); out.print(static_cast<unsigned>(flowCount()));
    out.print(",\"dnsRedirects\":"); out.print(static_cast<unsigned>(dnsTotal));
    out.print(",\"dnsLast\":\""); jsonEscape(out, dnsLast); out.print("\"");

    portENTER_CRITICAL(&gHandshakeMux);
    const uint32_t hsOk = gHandshakeOk;
    const uint32_t hsFail = gHandshakeFail;
    const uint32_t hsOkMs = gLastHandshakeOkMs;
    const uint32_t hsFailMs = gLastHandshakeFailMs;
    char hsClient[46]; strlcpy(hsClient, gLastHandshakeClient, sizeof(hsClient));
    char hsTls[16]; strlcpy(hsTls, gLastHandshakeTls, sizeof(hsTls));
    char hsCipher[48]; strlcpy(hsCipher, gLastHandshakeCipher, sizeof(hsCipher));
    char hsAlpn[16]; strlcpy(hsAlpn, gLastHandshakeAlpn, sizeof(hsAlpn));
    char hsError[64]; strlcpy(hsError, gLastHandshakeError, sizeof(hsError));
    portEXIT_CRITICAL(&gHandshakeMux);

    out.print(",\"handshake\":{");
    out.print("\"ok\":"); out.print(static_cast<unsigned long>(hsOk));
    out.print(",\"fail\":"); out.print(static_cast<unsigned long>(hsFail));
    out.print(",\"lastOkMs\":"); out.print(static_cast<unsigned long>(hsOkMs));
    out.print(",\"lastFailMs\":"); out.print(static_cast<unsigned long>(hsFailMs));
    out.print(",\"client\":\""); jsonEscape(out, hsClient); out.print("\"");
    out.print(",\"tls\":\""); jsonEscape(out, hsTls); out.print("\"");
    out.print(",\"cipher\":\""); jsonEscape(out, hsCipher); out.print("\"");
    out.print(",\"alpn\":\""); jsonEscape(out, hsAlpn); out.print("\"");
    out.print(",\"error\":\""); jsonEscape(out, hsError); out.print("\"");
    out.print("}");

    auto* last = new (std::nothrow) LastResponse;
    if (last) {
        portENTER_CRITICAL(&gLastResponseMux);
        *last = gLastResponse;
        portEXIT_CRITICAL(&gLastResponseMux);
        out.print(",\"lastResponse\":{");
        out.print("\"kind\":\""); jsonEscape(out, last->kind); out.print("\"");
        out.print(",\"status\":"); out.print(static_cast<unsigned>(last->status));
        out.print(",\"ctype\":\""); jsonEscape(out, last->ctype); out.print("\"");
        out.print(",\"bytes\":"); out.print(static_cast<unsigned long>(last->bytes));
        out.print(",\"md5\":\""); jsonEscape(out, last->md5); out.print("\"");
        out.print(",\"body\":\""); jsonEscape(out, last->body); out.print("\"");
        out.print("}");
    }
    delete last;

    constexpr size_t kStatusRecentLimit = 3;
    out.print(",\"recent\":[");
    size_t shown = 0;
    for (size_t i = 0; i < kRecentHitCapacity && shown < kStatusRecentLimit; ++i) {
        Hit recent;
        if (!readRecentHit(i, recent)) break;
        // Windows fsd_panel's timeline only lists feature_configuration
        // intercepts; non-FSD FLOW entries stay in the JSONL log.
        if (strcmp(recent.kind, "FLOW") == 0) continue;
        if (shown) out.print(',');
        ++shown;
        out.print("{\"kind\":\""); jsonEscape(out, recent.kind); out.print("\"");
        out.print(",\"ts\":\""); jsonEscape(out, recent.ts); out.print("\"");
        out.print(",\"client\":\""); jsonEscape(out, recent.client); out.print("\"");
        out.print(",\"method\":\""); jsonEscape(out, recent.method); out.print("\"");
        out.print(",\"url\":\""); jsonEscape(out, recent.url); out.print("\"");
        out.print(",\"vin\":\""); jsonEscape(out, recent.vin); out.print("\"");
        out.print(",\"atMs\":"); out.print(static_cast<unsigned long>(recent.atMs));
        out.print(",\"reqBytes\":"); out.print(static_cast<unsigned long>(recent.reqBytes));
        out.print(",\"bytes\":"); out.print(static_cast<unsigned long>(recent.bytes));
        out.print(",\"md5\":\""); jsonEscape(out, recent.md5); out.print("\"");
        out.print(",\"mode\":\""); jsonEscape(out, recent.mode); out.print("\"");
        out.print(",\"status\":"); out.print(static_cast<unsigned>(recent.status));
        out.print(",\"ctype\":\""); jsonEscape(out, recent.respCtype); out.print("\"");
        out.print(",\"respHead\":\""); jsonEscape(out, recent.respHead); out.print("\"");
        out.print("}");
    }
    out.print("]}");
}

// Copy one RAM ring entry without allocating an array of Hits. `index` is
// 0 = newest. Keeps peak heap low on status polls and log downloads.
static bool readRecentHit(size_t index, Hit& out) {
    portENTER_CRITICAL(&gHitMux);
    if (index >= gHitCount) {
        portEXIT_CRITICAL(&gHitMux);
        return false;
    }
    const size_t slot = (gHitWrite + kRecentHitCapacity - 1 - index) % kRecentHitCapacity;
    out = gHits[slot];
    portEXIT_CRITICAL(&gHitMux);
    return true;
}

static bool readRecentHandshake(size_t index, Handshake& out) {
    portENTER_CRITICAL(&gHandshakeMux);
    if (index >= gHandshakeCount) {
        portEXIT_CRITICAL(&gHandshakeMux);
        return false;
    }
    const size_t slot = (gHandshakeWrite + kHandshakeCapacity - 1 - index) % kHandshakeCapacity;
    out = gHandshakes[slot];
    portEXIT_CRITICAL(&gHandshakeMux);
    return true;
}

void appendHitsJsonl(Print& out) {
    for (size_t i = 0; i < kRecentHitCapacity; ++i) {
        Hit h;
        if (!readRecentHit(i, h)) break;
        out.print("{\"ts\":\""); jsonEscape(out, h.ts); out.print("\"");
        out.print(",\"kind\":\""); jsonEscape(out, h.kind); out.print("\"");
        out.print(",\"method\":\""); jsonEscape(out, h.method); out.print("\"");
        out.print(",\"url\":\""); jsonEscape(out, h.url); out.print("\"");
        out.print(",\"client\":\""); jsonEscape(out, h.client); out.print("\"");
        out.print(",\"http_version\":\""); jsonEscape(out, h.httpVersion); out.print("\"");
        out.print(",\"host_header\":\""); jsonEscape(out, h.host); out.print("\"");
        out.print(",\"user_agent\":\""); jsonEscape(out, h.userAgent); out.print("\"");
        out.print(",\"req_bytes\":"); out.print(static_cast<unsigned long>(h.reqBytes));
        out.print(",\"req_md5\":\""); jsonEscape(out, h.reqMd5); out.print("\"");
        out.print(",\"req_ctype\":\""); jsonEscape(out, h.reqCtype); out.print("\"");
        out.print(",\"vin\":\""); jsonEscape(out, h.vin); out.print("\"");
        out.print(",\"mode\":\""); jsonEscape(out, h.mode); out.print("\"");
        out.print(",\"req_head\":\""); jsonEscape(out, h.reqHead); out.print("\"");
        out.print(",\"bytes\":"); out.print(static_cast<unsigned long>(h.bytes));
        out.print(",\"md5\":\""); jsonEscape(out, h.md5); out.print("\"");
        out.print(",\"status\":"); out.print(static_cast<unsigned>(h.status));
        out.print(",\"ctype\":\""); jsonEscape(out, h.respCtype); out.print("\"");
        out.print(",\"resp_head\":\""); jsonEscape(out, h.respHead); out.print("\"");
        out.print("}\n");
    }
}

void appendHandshakesJsonl(Print& out) {
    for (size_t i = 0; i < kHandshakeCapacity; ++i) {
        Handshake h;
        if (!readRecentHandshake(i, h)) break;
        out.print("{\"ts\":\""); jsonEscape(out, h.ts); out.print("\"");
        out.print(",\"event\":\""); jsonEscape(out, h.event); out.print("\"");
        out.print(",\"client\":\""); jsonEscape(out, h.client); out.print("\"");
        out.print(",\"sni\":\""); jsonEscape(out, h.sni); out.print("\"");
        out.print(",\"alpn\":\""); jsonEscape(out, h.alpn); out.print("\"");
        out.print(",\"tls\":\""); jsonEscape(out, h.tls); out.print("\"");
        out.print(",\"cipher\":\""); jsonEscape(out, h.cipher); out.print("\"");
        out.print(",\"confdir\":\"emudhra\"");
        out.print(",\"error\":\""); jsonEscape(out, h.error); out.print("\"");
        out.print(",\"first_request\":\""); jsonEscape(out, h.firstRequest); out.print("\"");
        out.print("}\n");
    }
}

void appendConfigHistoryJsonl(Print& out) {
    for (size_t i = 0; i < kConfigHistoryCapacity; ++i) {
        ConfigChange record;
        portENTER_CRITICAL(&gConfigMux);
        if (i >= gConfigCount) {
            portEXIT_CRITICAL(&gConfigMux);
            break;
        }
        const size_t slot = (gConfigWrite + kConfigHistoryCapacity - 1 - i) % kConfigHistoryCapacity;
        record = gConfigHistory[slot];
        portEXIT_CRITICAL(&gConfigMux);
        out.print("{\"ts\":\""); jsonEscape(out, record.ts); out.print("\"");
        out.print(",\"source\":\""); jsonEscape(out, record.source); out.print("\"");
        out.print(",\"bytes\":"); out.print(static_cast<unsigned long>(record.bytes));
        out.print(",\"md5\":\""); jsonEscape(out, record.md5); out.print("\"");
        out.print(",\"prev_md5\":\""); jsonEscape(out, record.prevMd5); out.print("\"");
        out.print(",\"fields\":"); out.print(static_cast<unsigned>(record.fields));
        out.print("}\n");
    }
}

size_t recentHits(Hit* out, size_t capacity) {
    if (!out || capacity == 0) return 0;
    portENTER_CRITICAL(&gHitMux);
    const size_t count = gHitCount < capacity ? gHitCount : capacity;
    for (size_t i = 0; i < count; ++i) {
        const size_t index = (gHitWrite + kRecentHitCapacity - 1 - i) % kRecentHitCapacity;
        out[i] = gHits[index];
    }
    portEXIT_CRITICAL(&gHitMux);
    return count;
}

size_t recentHandshakes(Handshake* out, size_t capacity) {
    if (!out || capacity == 0) return 0;
    portENTER_CRITICAL(&gHandshakeMux);
    const size_t count = gHandshakeCount < capacity ? gHandshakeCount : capacity;
    for (size_t i = 0; i < count; ++i) {
        const size_t index = (gHandshakeWrite + kHandshakeCapacity - 1 - i) % kHandshakeCapacity;
        out[i] = gHandshakes[index];
    }
    portEXIT_CRITICAL(&gHandshakeMux);
    return count;
}

}  // namespace fsd_mock

#endif  // WIFI_BRIDGE_ENABLED && ESP32S3 && ESP_HTTPS_SERVER
