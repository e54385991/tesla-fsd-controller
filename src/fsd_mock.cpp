#include "mod_fsd_mock.h"

#include <sdkconfig.h>

#if defined(WIFI_BRIDGE_ENABLED) && defined(CONFIG_IDF_TARGET_ESP32S3) && defined(CONFIG_ESP_HTTPS_SERVER_ENABLE)

#include <Arduino.h>
#include <Preferences.h>
#include <SPIFFS.h>
#include <WiFi.h>
#include <cJSON.h>
#include <esp_http_client.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_rom_md5.h>
#include <esp_tls.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
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
#include "mod_dns.h"

namespace fsd_mock {
namespace {

constexpr uint32_t kSettingsMagic = 0x46444D32;  // "FDM2"
constexpr char kPayloadPath[] = "/fsd_payload.json";
constexpr char kPayloadTempPath[] = "/fsd_payload.tmp";
constexpr char kPayloadBackupPath[] = "/fsd_payload.bak";
constexpr size_t kRequestCaptureBytes = 64;
constexpr size_t kForwardBodyBytes = 4096;

struct Settings {
    uint32_t magic = kSettingsMagic;
    uint8_t enabled = 1;
    uint8_t mode = static_cast<uint8_t>(Mode::Reply);
    uint8_t reserved[2] = {};
    char host[kHostCapacity] = "api-prd.vn.cloud.tesla.cn";
};

struct State {
    bool enabled = true;
    Mode mode = Mode::Reply;
    char host[kHostCapacity] = "api-prd.vn.cloud.tesla.cn";
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
volatile bool gInUpstreamRequest = false;
volatile uint32_t gLastMbedtlsFailureMs = 0;

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

void jsonEscape(String& out, const char* value) {
    if (!value) return;
    for (const char* p = value; *p; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                // High bytes may be gzip/binary previews. Replacing them keeps
                // /api/wifi-bridge/status parseable as UTF-8 JSON.
                if (c < 0x20 || c >= 0x7f) out += '.';
                else out += static_cast<char>(c);
                break;
        }
    }
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
    for (const char* p = host; *p; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (!(isalnum(c) || c == '.' || c == '-' || c == '_' || isspace(c) || c == ',' || c == ';')) {
            return false;
        }
    }
    return true;
}

size_t countFields(cJSON* root) {
    return root ? static_cast<size_t>(cJSON_GetArraySize(root)) : 0;
}

bool compactJson(const char* json, size_t length, char* out, size_t outCapacity,
                 size_t& outLength, size_t& fields, char* error, size_t errorSize) {
    if (!json || length == 0) {
        snprintf(error, errorSize, "empty payload");
        return false;
    }
    if (length >= outCapacity) {
        snprintf(error, errorSize, "payload_too_large");
        return false;
    }
    cJSON* root = cJSON_ParseWithLength(json, length);
    if (!root) {
        snprintf(error, errorSize, "invalid_json");
        return false;
    }
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        snprintf(error, errorSize, "expected_object");
        return false;
    }
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
    } else if (storage.ss_family == AF_INET6) {
        auto* address = reinterpret_cast<struct sockaddr_in6*>(&storage);
        const uint8_t* bytes = address->sin6_addr.s6_addr;
        static const uint8_t v4mapped[12] = {0,0,0,0,0,0,0,0,0,0,0xff,0xff};
        if (memcmp(bytes, v4mapped, sizeof(v4mapped)) == 0) {
            snprintf(out, capacity, "%u.%u.%u.%u", bytes[12], bytes[13], bytes[14], bytes[15]);
            return;
        }
        if (inet_ntop(AF_INET6, &address->sin6_addr, out, capacity)) return;
    }
    out[0] = '\0';
}

void clientAddress(httpd_req_t* request, char* out, size_t capacity) {
    socketAddress(httpd_req_to_sockfd(request), out, capacity);
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
    out[0] = '\0';
    const size_t length = httpd_req_get_hdr_value_len(request, name);
    if (length == 0 || length + 1 > capacity) return;
    httpd_req_get_hdr_value_str(request, name, out, capacity);
}

void parseRequestMeta(httpd_req_t* request, Hit& hit, Capture& capture) {
    char hostHeader[96] = {};
    copyHeader(request, "Host", hostHeader, sizeof(hostHeader));
    char userAgent[48] = {};
    copyHeader(request, "User-Agent", userAgent, sizeof(userAgent));
    char ctype[32] = {};
    copyHeader(request, "Content-Type", ctype, sizeof(ctype));

    const char* uri = request->uri ? request->uri : "/";
    strlcpy(hit.method, httpMethodName(request->method), sizeof(hit.method));
    // fsd_mock.py logs flow.request.pretty_url (scheme://host/path?query).
    strlcpy(hit.url, "https://", sizeof(hit.url));
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
    strlcpy(settings.host, gState.host, sizeof(settings.host));
    unlockState();

    Preferences prefs;
    if (!prefs.begin("fsdmock", false)) return false;
    const size_t written = prefs.putBytes("cfg", &settings, sizeof(settings));
    prefs.end();
    return written == sizeof(settings);
}

// ── request body ────────────────────────────────────────────────────────────

bool drainRequestBody(httpd_req_t* request, char* body, size_t bodyCapacity,
                      size_t& bodyLength, Capture& capture) {
    bodyLength = 0;
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

        // Copy the complete body for observe/passthrough forwarding. Only the
        // first kRequestCaptureBytes are mirrored into the RAM log preview.
        if (body && bodyLength + 1 < bodyCapacity) {
            size_t copy = receivedSize;
            if (bodyLength + copy >= bodyCapacity) copy = bodyCapacity - 1 - bodyLength;
            memcpy(body + bodyLength, chunk, copy);
            bodyLength += copy;
        }

        if (captured < kRequestCaptureBytes) {
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
        remaining -= receivedSize;
    }
    uint8_t digest[16];
    esp_rom_md5_final(digest, &md5);
    for (size_t i = 0; i < sizeof(digest); ++i) {
        snprintf(capture.requestMd5 + i * 2, 3, "%02x", static_cast<unsigned>(digest[i]));
    }
    if (body && bodyLength < bodyCapacity) body[bodyLength] = '\0';
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

void setStatusLine(httpd_req_t* request, int code) {
    const char* text = "";
    switch (code) {
        case 200: text = "OK"; break;
        case 201: text = "Created"; break;
        case 204: text = "No Content"; break;
        case 301: text = "Moved Permanently"; break;
        case 302: text = "Found"; break;
        case 304: text = "Not Modified"; break;
        case 400: text = "Bad Request"; break;
        case 401: text = "Unauthorized"; break;
        case 403: text = "Forbidden"; break;
        case 404: text = "Not Found"; break;
        case 409: text = "Conflict"; break;
        case 413: text = "Payload Too Large"; break;
        case 429: text = "Too Many Requests"; break;
        case 500: text = "Internal Server Error"; break;
        case 502: text = "Bad Gateway"; break;
        case 503: text = "Service Unavailable"; break;
        default: text = ""; break;
    }
    static char line[32];
    if (text[0]) snprintf(line, sizeof(line), "%d %s", code, text);
    else snprintf(line, sizeof(line), "%d", code);
    httpd_resp_set_status(request, line);
}

void finishJsonError(httpd_req_t* request, Hit& hit, char* responseBody,
                     int status, const char* json) {
    setStatusLine(request, status);
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

bool upstreamReady() {
    return WiFi.status() == WL_CONNECTED &&
        static_cast<uint32_t>(WiFi.localIP()) != 0;
}

int forwardRequest(httpd_req_t* request, const char* body, size_t bodyLength,
                   Hit& hit, char* responseBody, size_t responseCapacity) {
    if (!upstreamReady()) {
        discardRequestBody(request, 8192);
        finishJsonError(request, hit, responseBody, 502, "{\"error\":\"upstream_unavailable\"}");
        return 502;
    }
    hit.status = 0;
    hit.bytes = 0;
    hit.md5[0] = '\0';
    hit.respHead[0] = '\0';
    hit.respCtype[0] = '\0';
    if (responseBody && responseCapacity) responseBody[0] = '\0';

    char hostOnly[kHostCapacity] = {};
    lockState();
    const char* cursor = gState.host;
    while (*cursor) {
        while (*cursor && (isspace(static_cast<unsigned char>(*cursor)) || *cursor == ',' || *cursor == ';')) ++cursor;
        const char* begin = cursor;
        while (*cursor && !(isspace(static_cast<unsigned char>(*cursor)) || *cursor == ',' || *cursor == ';')) ++cursor;
        if (cursor > begin) {
            const size_t length = static_cast<size_t>(cursor - begin);
            const size_t copy = length < sizeof(hostOnly) - 1 ? length : sizeof(hostOnly) - 1;
            memcpy(hostOnly, begin, copy);
            hostOnly[copy] = '\0';
            break;
        }
    }
    unlockState();
    if (hostOnly[0] == '\0') strlcpy(hostOnly, "api-prd.vn.cloud.tesla.cn", sizeof(hostOnly));

    const size_t urlLength = strlen(hostOnly) + strlen(request->uri) + 16;
    char* url = static_cast<char*>(malloc(urlLength));
    if (!url) {
        finishJsonError(request, hit, responseBody, 500, "{\"error\":\"no_memory\"}");
        return 500;
    }
    snprintf(url, urlLength, "https://%s%s", hostOnly, request->uri);

    esp_http_client_config_t config = {};
    config.url = url;
    config.method = upstreamMethod(request->method);
    config.timeout_ms = 5000;
    config.disable_auto_redirect = true;
    config.transport_type = HTTP_TRANSPORT_OVER_SSL;
    config.skip_cert_common_name_check = true;
    config.buffer_size = 2048;
    config.buffer_size_tx = 2048;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(url);
        finishJsonError(request, hit, responseBody, 502, "{\"error\":\"upstream_init\"}");
        return 502;
    }

    static const char* const kForwardHeaders[] = {
        "Authorization", "Content-Type", "Accept", "Accept-Encoding", "Accept-Language",
        "User-Agent", "Cookie", "If-None-Match", "If-Modified-Since",
        "X-Tesla-User-Agent", "Tesla-User-Agent", "X-Tesla-Client",
        "X-Tesla-Protocol-Version", "X-Requested-With",
    };
    for (const char* name : kForwardHeaders) {
        const size_t length = httpd_req_get_hdr_value_len(request, name);
        if (length == 0) continue;
        char* value = static_cast<char*>(malloc(length + 1));
        if (!value) continue;
        if (httpd_req_get_hdr_value_str(request, name, value, length + 1) == ESP_OK) {
            esp_http_client_set_header(client, name, value);
        }
        free(value);
    }
    // Do NOT use esp_http_client_perform() here: it reads and discards the
    // response body internally. Open/write/fetch/read lets us stream the real
    // upstream bytes back to the car (mitmproxy reverse-mode behavior).
    gInUpstreamRequest = true;
    esp_err_t error = esp_http_client_open(client, bodyLength);
    if (error == ESP_OK && bodyLength > 0) {
        const int written = esp_http_client_write(client, body, static_cast<int>(bodyLength));
        if (written != static_cast<int>(bodyLength)) error = ESP_FAIL;
    }
    if (error == ESP_OK && esp_http_client_fetch_headers(client) < 0) {
        error = ESP_FAIL;
    }
    gInUpstreamRequest = false;
    if (error != ESP_OK) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        free(url);
        finishJsonError(request, hit, responseBody, 502, "{\"error\":\"upstream_failed\"}");
        return 502;
    }

    int status = esp_http_client_get_status_code(client);
    if (status <= 0) status = 502;
    hit.status = static_cast<uint16_t>(status);
    setStatusLine(request, status);

    char* contentType = nullptr;
    if (esp_http_client_get_header(client, "Content-Type", &contentType) == ESP_OK && contentType) {
        httpd_resp_set_type(request, contentType);
        strlcpy(hit.respCtype, contentType, sizeof(hit.respCtype));
    }
    static const char* const kResponseHeaders[] = {
        "Cache-Control", "Content-Encoding", "Location", "ETag", "Vary",
        "Set-Cookie",
    };
    for (const char* name : kResponseHeaders) {
        char* value = nullptr;
        if (esp_http_client_get_header(client, name, &value) == ESP_OK && value) {
            httpd_resp_set_hdr(request, name, value);
        }
    }

    md5_context_t md5;
    esp_rom_md5_init(&md5);
    char buffer[512];
    size_t total = 0;
    size_t preview = 0;
    size_t bodyStored = 0;
    int read = 0;
    while ((read = esp_http_client_read(client, buffer, sizeof(buffer))) > 0) {
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
        const esp_err_t sent = httpd_resp_send_chunk(request, buffer, read);
        total += static_cast<size_t>(read);
        if (sent != ESP_OK) break;
    }
    httpd_resp_send_chunk(request, nullptr, 0);
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
    if (format && !gInUpstreamRequest) {
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
    const bool newMessage = strcmp(message, gLastHandshakeError) != 0;
    ++gHandshakeFail;
    if (newFailure || newMessage || strlen(message) > strlen(gLastHandshakeError)) {
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

void noteHandshake(const esp_tls_t* tls) {
    char client[46] = {};
    char version[16] = {};
    char cipher[48] = {};
    char alpn[16] = {};
    char ts[32] = {};
    formatTimestamp(ts);
    if (tls) {
        socketAddress(tls->sockfd, client, sizeof(client));
#if defined(CONFIG_ESP_TLS_USING_MBEDTLS)
        const char* value = mbedtls_ssl_get_version(&tls->ssl);
        if (value) strlcpy(version, value, sizeof(version));
        value = mbedtls_ssl_get_ciphersuite(&tls->ssl);
        if (value) strlcpy(cipher, value, sizeof(cipher));
        value = mbedtls_ssl_get_alpn_protocol(&tls->ssl);
        if (value) strlcpy(alpn, value, sizeof(alpn));
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
    if (prefs.begin("fsdmock", true)) {
        Settings settings;
        const size_t length = prefs.getBytesLength("cfg");
        if (length == sizeof(settings) &&
            prefs.getBytes("cfg", &settings, sizeof(settings)) == sizeof(settings) &&
            settings.magic == kSettingsMagic) {
            lockState();
            gState.enabled = settings.enabled != 0;
            gState.mode = settings.mode == static_cast<uint8_t>(Mode::Observe) ? Mode::Observe : Mode::Reply;
            if (validHostList(settings.host)) strlcpy(gState.host, settings.host, sizeof(gState.host));
            unlockState();
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
    gActive = true;
    syncDnsRedirect();
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

    if (observe && !upstreamReady()) {
        discardRequestBody(request, 8192);
        strlcpy(hit.reqCtype, capture.ctype, sizeof(hit.reqCtype));
        strlcpy(hit.vin, capture.vin, sizeof(hit.vin));
        hit.reqBytes = static_cast<uint32_t>(request->content_len);
        const char* errorBody = "{\"error\":\"upstream_unavailable\"}";
        finishJsonError(request, hit, nullptr, 502, errorBody);
        recordHit(hit, errorBody);
        return ESP_OK;
    }

    if (observe && request->content_len > kForwardBodyBytes) {
        discardRequestBody(request, 8192);
        const char* errorBody = "{\"error\":\"request_too_large\"}";
        finishJsonError(request, hit, nullptr, 413, errorBody);
        recordHit(hit, errorBody);
        return ESP_OK;
    }

    char* forwarded = nullptr;
    if (observe) forwarded = static_cast<char*>(malloc(kForwardBodyBytes + 1));
    size_t forwardedLength = 0;
    const bool bodyOk = drainRequestBody(request, forwarded, kForwardBodyBytes + 1,
                                           forwardedLength, capture);
    strlcpy(hit.reqMd5, capture.requestMd5, sizeof(hit.reqMd5));
    strlcpy(hit.reqCtype, capture.ctype, sizeof(hit.reqCtype));
    strlcpy(hit.reqHead, capture.head, sizeof(hit.reqHead));
    hit.reqBytes = capture.requestBytes;
    strlcpy(hit.vin, capture.vin, sizeof(hit.vin));
    if (!bodyOk) {
        free(forwarded);
        const char* errorBody = "{\"error\":\"request_read_failed\"}";
        finishJsonError(request, hit, nullptr, 400, errorBody);
        recordHit(hit, errorBody);
        return ESP_OK;
    }

    if (observe) {
        char responseBody[kLastResponseCapacity];
        forwardRequest(request, forwarded, forwardedLength, hit, responseBody, sizeof(responseBody));
        free(forwarded);
        recordHit(hit, responseBody);
        return ESP_OK;
    }

    char* body = static_cast<char*>(malloc(kPayloadCapacity));
    if (!body) {
        free(forwarded);
        const char* errorBody = "{\"error\":\"no_memory\"}";
        finishJsonError(request, hit, nullptr, 500, errorBody);
        recordHit(hit, errorBody);
        return ESP_OK;
    }
    char checksum[33];
    size_t bytes = 0;
    lockState();
    bytes = gState.bytes;
    memcpy(body, gState.payload, bytes + 1);
    strlcpy(checksum, gState.md5, sizeof(checksum));
    unlockState();

    setStatusLine(request, 200);
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "X-Checksum", checksum);
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    const esp_err_t result = httpd_resp_send(request, body, bytes);

    hit.status = 200;
    hit.bytes = static_cast<uint32_t>(bytes);
    strlcpy(hit.md5, checksum, sizeof(hit.md5));
    strlcpy(hit.respCtype, "application/json", sizeof(hit.respCtype));
    copyPrintable(reinterpret_cast<const uint8_t*>(body), bytes, hit.respHead, sizeof(hit.respHead));
    recordHit(hit, body);
    free(body);
    free(forwarded);
    return result;
}

esp_err_t forwardPassthrough(httpd_req_t* request) {
    Hit hit;
    Capture capture;
    parseRequestMeta(request, hit, capture);
    strlcpy(hit.kind, "FLOW", sizeof(hit.kind));
    strlcpy(hit.mode, "flow", sizeof(hit.mode));
    strlcpy(hit.reqCtype, capture.ctype, sizeof(hit.reqCtype));
    strlcpy(hit.vin, capture.vin, sizeof(hit.vin));
    hit.reqBytes = static_cast<uint32_t>(request->content_len);

    if (!upstreamReady()) {
        discardRequestBody(request, 8192);
        const char* errorBody = "{\"error\":\"upstream_unavailable\"}";
        finishJsonError(request, hit, nullptr, 502, errorBody);
        recordHit(hit, errorBody);
        return ESP_OK;
    }

    if (request->content_len > kForwardBodyBytes) {
        discardRequestBody(request, 8192);
        const char* errorBody = "{\"error\":\"request_too_large\"}";
        finishJsonError(request, hit, nullptr, 413, errorBody);
        recordHit(hit, errorBody);
        return ESP_OK;
    }

    char* body = static_cast<char*>(malloc(kForwardBodyBytes + 1));
    size_t bodyLength = 0;
    if (!drainRequestBody(request, body, kForwardBodyBytes + 1, bodyLength, capture)) {
        free(body);
        const char* errorBody = "{\"error\":\"request_read_failed\"}";
        finishJsonError(request, hit, nullptr, 400, errorBody);
        recordHit(hit, errorBody);
        return ESP_OK;
    }
    strlcpy(hit.reqMd5, capture.requestMd5, sizeof(hit.reqMd5));
    strlcpy(hit.reqCtype, capture.ctype, sizeof(hit.reqCtype));
    strlcpy(hit.reqHead, capture.head, sizeof(hit.reqHead));
    hit.reqBytes = capture.requestBytes;
    strlcpy(hit.vin, capture.vin, sizeof(hit.vin));

    char responseBody[kLastResponseCapacity];
    forwardRequest(request, body, bodyLength, hit, responseBody, sizeof(responseBody));
    free(body);
    recordHit(hit, responseBody);
    return ESP_OK;
}

bool enabled() {
    lockState();
    const bool value = gState.enabled;
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

size_t copyPayload(char* output, size_t capacity) {
    if (!output || capacity == 0) return 0;
    lockState();
    const size_t bytes = gState.bytes;
    const size_t copy = bytes < capacity - 1 ? bytes : capacity - 1;
    memcpy(output, gState.payload, copy);
    output[copy] = '\0';
    unlockState();
    return copy;
}

bool setEnabled(bool value) {
    lockState();
    gState.enabled = value;
    unlockState();
    syncDnsRedirect();
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

bool setPayload(const char* json, size_t length, char* error, size_t errorSize) {
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
    lockState();
    memcpy(gState.payload, compacted, compactLength + 1);
    gState.bytes = compactLength;
    gState.fields = fields;
    md5Hex(gState.payload, gState.bytes, gState.md5);
    unlockState();
    free(compacted);
    return true;
}

bool loadPreset(const char* name, char* error, size_t errorSize) {
    if (!name) {
        snprintf(error, errorSize, "unknown_preset");
        return false;
    }
    if (strcasecmp(name, "default") == 0 || strcasecmp(name, "windows") == 0) {
        return setPayload(kFsdPayloadDefault, strlen(kFsdPayloadDefault), error, errorSize);
    }
    if (strcasecmp(name, "16f") == 0 || strcasecmp(name, "payload-16f") == 0) {
        return setPayload(kFsdPayload16f, strlen(kFsdPayload16f), error, errorSize);
    }
    if (strcasecmp(name, "282b") == 0 || strcasecmp(name, "payload-282b") == 0 ||
        strcasecmp(name, "official") == 0) {
        return setPayload(kFsdPayload282b, strlen(kFsdPayload282b), error, errorSize);
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

static bool readRecentHit(size_t index, Hit& out);
static bool readRecentHandshake(size_t index, Handshake& out);

void appendStatusJson(String& out) {
    bool enabledNow;
    Mode modeNow;
    char hostNow[kHostCapacity];
    char md5Now[33];
    size_t bytesNow;
    size_t fieldsNow;
    lockState();
    enabledNow = gState.enabled;
    modeNow = gState.mode;
    strlcpy(hostNow, gState.host, sizeof(hostNow));
    strlcpy(md5Now, gState.md5, sizeof(md5Now));
    bytesNow = gState.bytes;
    fieldsNow = gState.fields;
    unlockState();

    uint32_t dnsTotal = 0;
    char dnsLast[128] = {};
    dnsGetFsdRedirectStats(&dnsTotal, dnsLast, sizeof(dnsLast));

    out += "\"fsdMock\":{";
    out += "\"nowMs\":"; out += String(static_cast<unsigned long>(millis()));
    out += ",\"active\":"; out += gActive ? "true" : "false";
    out += ",\"enabled\":"; out += enabledNow ? "true" : "false";
    out += ",\"mode\":\""; out += (modeNow == Mode::Observe ? "observe" : "reply"); out += "\"";
    out += ",\"host\":\""; jsonEscape(out, hostNow); out += "\"";
    out += ",\"bytes\":"; out += String(static_cast<unsigned>(bytesNow));
    out += ",\"fields\":"; out += String(static_cast<unsigned>(fieldsNow));
    out += ",\"md5\":\""; out += md5Now; out += "\"";
    out += ",\"canonical\":";
    out += (bytesNow == kFsdCanonicalBytes && strcmp(md5Now, kFsdCanonicalMd5) == 0) ? "true" : "false";
    out += ",\"hits\":"; out += String(static_cast<unsigned>(hitCount()));
    out += ",\"observes\":"; out += String(static_cast<unsigned>(observeCount()));
    out += ",\"flows\":"; out += String(static_cast<unsigned>(flowCount()));
    out += ",\"dnsRedirects\":"; out += String(static_cast<unsigned>(dnsTotal));
    out += ",\"dnsLast\":\""; jsonEscape(out, dnsLast); out += "\"";

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

    out += ",\"handshake\":{";
    out += "\"ok\":"; out += String(static_cast<unsigned long>(hsOk));
    out += ",\"fail\":"; out += String(static_cast<unsigned long>(hsFail));
    out += ",\"lastOkMs\":"; out += String(static_cast<unsigned long>(hsOkMs));
    out += ",\"lastFailMs\":"; out += String(static_cast<unsigned long>(hsFailMs));
    out += ",\"client\":\""; jsonEscape(out, hsClient); out += "\"";
    out += ",\"tls\":\""; jsonEscape(out, hsTls); out += "\"";
    out += ",\"cipher\":\""; jsonEscape(out, hsCipher); out += "\"";
    out += ",\"alpn\":\""; jsonEscape(out, hsAlpn); out += "\"";
    out += ",\"error\":\""; jsonEscape(out, hsError); out += "\"";
    out += "}";

    auto* last = new (std::nothrow) LastResponse;
    if (last) {
        portENTER_CRITICAL(&gLastResponseMux);
        *last = gLastResponse;
        portEXIT_CRITICAL(&gLastResponseMux);
        out += ",\"lastResponse\":{";
        out += "\"kind\":\""; jsonEscape(out, last->kind); out += "\"";
        out += ",\"status\":"; out += String(static_cast<unsigned>(last->status));
        out += ",\"ctype\":\""; jsonEscape(out, last->ctype); out += "\"";
        out += ",\"bytes\":"; out += String(static_cast<unsigned long>(last->bytes));
        out += ",\"md5\":\""; jsonEscape(out, last->md5); out += "\"";
        out += ",\"body\":\""; jsonEscape(out, last->body); out += "\"";
        out += "}";
    }
    delete last;

    constexpr size_t kStatusRecentLimit = 3;
    out += ",\"recent\":[";
    size_t shown = 0;
    for (size_t i = 0; i < kRecentHitCapacity && shown < kStatusRecentLimit; ++i) {
        Hit recent;
        if (!readRecentHit(i, recent)) break;
        // Windows fsd_panel's timeline only lists feature_configuration
        // intercepts; non-FSD FLOW entries stay in the JSONL log.
        if (strcmp(recent.kind, "FLOW") == 0) continue;
        if (shown) out += ',';
        ++shown;
        out += "{\"kind\":\""; jsonEscape(out, recent.kind); out += "\"";
        out += ",\"ts\":\""; jsonEscape(out, recent.ts); out += "\"";
        out += ",\"client\":\""; jsonEscape(out, recent.client); out += "\"";
        out += ",\"method\":\""; jsonEscape(out, recent.method); out += "\"";
        out += ",\"url\":\""; jsonEscape(out, recent.url); out += "\"";
        out += ",\"vin\":\""; jsonEscape(out, recent.vin); out += "\"";
        out += ",\"atMs\":"; out += String(static_cast<unsigned long>(recent.atMs));
        out += ",\"reqBytes\":"; out += String(static_cast<unsigned long>(recent.reqBytes));
        out += ",\"bytes\":"; out += String(static_cast<unsigned long>(recent.bytes));
        out += ",\"md5\":\""; jsonEscape(out, recent.md5); out += "\"";
        out += ",\"mode\":\""; jsonEscape(out, recent.mode); out += "\"";
        out += ",\"status\":"; out += String(static_cast<unsigned>(recent.status));
        out += ",\"ctype\":\""; jsonEscape(out, recent.respCtype); out += "\"";
        out += ",\"respHead\":\""; jsonEscape(out, recent.respHead); out += "\"";
        out += "}";
    }
    out += "]}";
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
