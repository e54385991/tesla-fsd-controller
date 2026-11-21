#include "mod_fsd_mock.h"

#include <sdkconfig.h>

#if defined(WIFI_BRIDGE_ENABLED) && defined(CONFIG_IDF_TARGET_ESP32S3) && defined(CONFIG_ESP_HTTPS_SERVER_ENABLE)

#include <Arduino.h>
#include <SPIFFS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <cJSON.h>
#include <esp_http_client.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_rom_md5.h>
#include <esp_tls.h>
#include <mbedtls/ssl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <lwip/sockets.h>

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <strings.h>

#include "fsd_payload.h"
#include "mod_dns.h"

namespace fsd_mock {
namespace {

constexpr uint32_t kSettingsMagic = 0x46444D31;  // "FDM1"
constexpr char kPayloadPath[] = "/fsd_payload.json";
constexpr char kPayloadTempPath[] = "/fsd_payload.tmp";
constexpr char kHitLogPath[] = "/fsd_hits.jsonl";
constexpr char kHandshakeLogPath[] = "/fsd_handshake.log";
constexpr size_t kHitLogMaxBytes = 16 * 1024;
constexpr size_t kHitLogKeepBytes = 8 * 1024;
constexpr size_t kRequestCaptureBytes = 512;
constexpr size_t kForwardBodyBytes = 8192;

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
    char vin[18] = {};
    char head[97] = {};
};

State gState;
char gSendBuffer[kPayloadCapacity];
SemaphoreHandle_t gStateMutex = nullptr;

Hit gHits[kRecentHitCapacity];
size_t gHitCount = 0;
size_t gHitWrite = 0;
portMUX_TYPE gHitMux = portMUX_INITIALIZER_UNLOCKED;
uint32_t gHitTotal = 0;
uint32_t gObserveTotal = 0;
uint32_t gFlowTotal = 0;

char gReportedHost[kHostCapacity] = "api-prd.vn.cloud.tesla.cn";
bool gActive = false;

struct HandshakeState {
    uint32_t ok = 0;
    uint32_t fail = 0;
    uint32_t lastOkMs = 0;
    uint32_t lastFailMs = 0;
    uint32_t lastFailLogMs = 0;
    char lastClient[16] = {};
    char lastTls[16] = {};
    char lastCipher[64] = {};
    char lastAlpn[16] = {};
    char lastError[64] = {};
};
HandshakeState gHandshake;
portMUX_TYPE gHandshakeMux = portMUX_INITIALIZER_UNLOCKED;

struct LastResponse {
    char kind[8] = {};
    uint16_t status = 0;
    char ctype[48] = {};
    char md5[33] = {};
    char body[2049] = {};
    uint32_t bytes = 0;
};
LastResponse gLastResponse;
portMUX_TYPE gLastResponseMux = portMUX_INITIALIZER_UNLOCKED;

vprintf_like_t gPreviousLog = nullptr;
bool gInLogHook = false;
constexpr size_t kHandshakeLogMaxBytes = 16 * 1024;

// ── helpers ──────────────────────────────────────────────────────────────────

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
            case '\r': break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) out += ' ';
                else out += static_cast<char>(c);
                break;
        }
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

void syncDnsRedirect() {
    bool enabledNow;
    char hostNow[kHostCapacity];
    lockState();
    enabledNow = gState.enabled;
    strlcpy(hostNow, gState.host, sizeof(hostNow));
    unlockState();

    const IPAddress answer = WiFi.softAPIP();
    dnsSetFsdRedirect(gActive && enabledNow, hostNow, answer);

    strlcpy(gReportedHost, hostNow, sizeof(gReportedHost));
}

bool savePayloadFile(const char* payload, size_t length) {
    File file = SPIFFS.open(kPayloadTempPath, "w");
    if (!file) return false;
    const size_t written = file.write(reinterpret_cast<const uint8_t*>(payload), length);
    file.close();
    if (written != length) {
        SPIFFS.remove(kPayloadTempPath);
        return false;
    }
    SPIFFS.remove(kPayloadPath);
    if (!SPIFFS.rename(kPayloadTempPath, kPayloadPath)) {
        SPIFFS.remove(kPayloadTempPath);
        return false;
    }
    return true;
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

void rotateHitLog() {
    File source = SPIFFS.open(kHitLogPath, "r");
    if (!source) return;
    const size_t size = source.size();
    if (size <= kHitLogMaxBytes) {
        source.close();
        return;
    }
    source.seek(size - kHitLogKeepBytes);
    File target = SPIFFS.open("/fsd_hits.tmp", "w");
    if (!target) {
        source.close();
        return;
    }
    uint8_t buffer[512];
    while (source.available()) {
        const size_t count = source.read(buffer, sizeof(buffer));
        if (count == 0) break;
        target.write(buffer, count);
    }
    source.close();
    target.close();
    SPIFFS.remove(kHitLogPath);
    SPIFFS.rename("/fsd_hits.tmp", kHitLogPath);
}

void logSafe(const char* in, char* out, size_t capacity) {
    if (capacity == 0) return;
    size_t written = 0;
    for (const char* p = in; p && *p && written + 1 < capacity; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        out[written++] = (c == '"' || c == '\\' || c < 0x20) ? '_' : static_cast<char>(c);
    }
    out[written] = '\0';
}

void appendHitLog(const Hit& hit) {
    char url[193];
    char head[97];
    char resp[130];
    char ctype[49];
    logSafe(hit.url, url, sizeof(url));
    logSafe(hit.reqHead, head, sizeof(head));
    logSafe(hit.respHead, resp, sizeof(resp));
    logSafe(hit.respCtype, ctype, sizeof(ctype));
    char line[1024];
    const int length = snprintf(line, sizeof(line),
        "{\"kind\":\"%s\",\"uptime_ms\":%lu,\"client\":\"%s\",\"method\":\"%s\",\"url\":\"%s\",\"vin\":\"%s\","
        "\"req_bytes\":%lu,\"req_md5\":\"%s\",\"req_head\":\"%s\",\"status\":%u,\"bytes\":%lu,"
        "\"md5\":\"%s\",\"ctype\":\"%s\",\"resp_head\":\"%s\",\"mode\":\"%s\"}\n",
        hit.kind[0] ? hit.kind : "HIT",
        static_cast<unsigned long>(hit.uptimeMs), hit.client, hit.method, url, hit.vin,
        static_cast<unsigned long>(hit.reqBytes), hit.reqMd5, head, static_cast<unsigned>(hit.status),
        static_cast<unsigned long>(hit.bytes), hit.md5, ctype, resp, hit.mode);
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(line)) return;
    File file = SPIFFS.open(kHitLogPath, "a");
    if (!file) return;
    file.write(reinterpret_cast<const uint8_t*>(line), static_cast<size_t>(length));
    const size_t size = file.size();
    file.close();
    if (size > kHitLogMaxBytes) rotateHitLog();
}

void recordHit(const Hit& hit) {
    portENTER_CRITICAL(&gHitMux);
    gHits[gHitWrite] = hit;
    gHitWrite = (gHitWrite + 1) % kRecentHitCapacity;
    if (gHitCount < kRecentHitCapacity) ++gHitCount;
    if (strcmp(hit.kind, "OBSERVE") == 0) ++gObserveTotal;
    else if (strcmp(hit.kind, "FLOW") == 0) ++gFlowTotal;
    else ++gHitTotal;
    portEXIT_CRITICAL(&gHitMux);
    appendHitLog(hit);
}

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
        case 429: text = "Too Many Requests"; break;
        case 500: text = "Internal Server Error"; break;
        case 502: text = "Bad Gateway"; break;
        case 503: text = "Service Unavailable"; break;
        default: text = ""; break;
    }
    // httpd stores the pointer until the response is sent, so keep it alive.
    static char line[32];
    if (text[0]) snprintf(line, sizeof(line), "%d %s", code, text);
    else snprintf(line, sizeof(line), "%d", code);
    httpd_resp_set_status(request, line);
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

void socketAddress(int fd, char* out, size_t capacity) {
    out[0] = '\0';
    if (fd < 0) return;
    struct sockaddr_in address = {};
    socklen_t length = sizeof(address);
    if (getpeername(fd, reinterpret_cast<struct sockaddr*>(&address), &length) != 0) return;
    const char* text = inet_ntoa(address.sin_addr);
    if (text) strlcpy(out, text, capacity);
}

void clientAddress(httpd_req_t* request, char* out, size_t capacity) {
    socketAddress(httpd_req_to_sockfd(request), out, capacity);
}

void copyPrintable(const char* source, size_t sourceLength, char* out, size_t capacity) {
    if (capacity == 0) return;
    size_t written = 0;
    for (size_t i = 0; i < sourceLength && written + 1 < capacity; ++i) {
        const unsigned char c = static_cast<unsigned char>(source[i]);
        out[written++] = (c < 0x20 || c > 0x7e) ? '.' : static_cast<char>(c);
    }
    out[written] = '\0';
}

void rotateHandshakeLog() {
    File source = SPIFFS.open(kHandshakeLogPath, "r");
    if (!source) return;
    const size_t size = source.size();
    if (size <= kHandshakeLogMaxBytes) {
        source.close();
        return;
    }
    source.seek(size - kHandshakeLogMaxBytes / 2);
    File target = SPIFFS.open("/fsd_handshake.tmp", "w");
    if (!target) {
        source.close();
        return;
    }
    uint8_t buffer[512];
    while (source.available()) {
        const size_t count = source.read(buffer, sizeof(buffer));
        if (count == 0) break;
        target.write(buffer, count);
    }
    source.close();
    target.close();
    SPIFFS.remove(kHandshakeLogPath);
    SPIFFS.rename("/fsd_handshake.tmp", kHandshakeLogPath);
}

void appendHandshakeLog(const char* event, const char* client, const char* tls,
                        const char* cipher, const char* alpn, const char* error) {
    char line[640];
    if (error && *error) {
        snprintf(line, sizeof(line),
            "{\"event\":\"%s\",\"uptime_ms\":%lu,\"client\":\"%s\",\"error\":\"%s\"}\n",
            event, static_cast<unsigned long>(millis()), client ? client : "", error);
    } else {
        snprintf(line, sizeof(line),
            "{\"event\":\"%s\",\"uptime_ms\":%lu,\"client\":\"%s\",\"tls\":\"%s\","
            "\"cipher\":\"%s\",\"alpn\":\"%s\"}\n",
            event, static_cast<unsigned long>(millis()), client ? client : "",
            tls ? tls : "", cipher ? cipher : "", alpn ? alpn : "");
    }
    const size_t length = strlen(line);
    File file = SPIFFS.open(kHandshakeLogPath, "a");
    if (!file) return;
    file.write(reinterpret_cast<const uint8_t*>(line), length);
    const size_t size = file.size();
    file.close();
    if (size > kHandshakeLogMaxBytes) rotateHandshakeLog();
}

void setLastResponse(const Hit& hit, const char* body) {
    portENTER_CRITICAL(&gLastResponseMux);
    strlcpy(gLastResponse.kind, hit.kind, sizeof(gLastResponse.kind));
    gLastResponse.status = hit.status;
    strlcpy(gLastResponse.ctype, hit.respCtype, sizeof(gLastResponse.ctype));
    strlcpy(gLastResponse.md5, hit.md5, sizeof(gLastResponse.md5));
    gLastResponse.bytes = hit.bytes;
    const size_t length = body ? strlen(body) : 0;
    const size_t copy = length < sizeof(gLastResponse.body) - 1
        ? length : sizeof(gLastResponse.body) - 1;
    if (copy) memcpy(gLastResponse.body, body, copy);
    gLastResponse.body[copy] = '\0';
    portEXIT_CRITICAL(&gLastResponseMux);
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

// Read the request body, hash it, keep the first bytes for diagnostics and the
// whole body for observe-mode forwarding. Returns false only on socket errors.
bool drainRequestBody(httpd_req_t* request, char* body, size_t bodyCapacity, size_t& bodyLength, Capture& capture) {
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
        esp_rom_md5_update(&md5, chunk, static_cast<uint32_t>(received));
        if (captured < kRequestCaptureBytes) {
            const size_t copy = (kRequestCaptureBytes - captured) < static_cast<size_t>(received)
                ? (kRequestCaptureBytes - captured) : static_cast<size_t>(received);
            if (body && bodyLength + copy < bodyCapacity) {
                memcpy(body + bodyLength, chunk, copy);
                bodyLength += copy;
            }
            if (capture.head[0] == '\0') {
                const size_t headCopy = copy < 96 ? copy : 96;
                memcpy(capture.head, chunk, headCopy);
                capture.head[headCopy] = '\0';
                for (size_t i = 0; i < headCopy; ++i) {
                    const unsigned char c = static_cast<unsigned char>(capture.head[i]);
                    if (c < 0x20 || c > 0x7e) capture.head[i] = '.';
                }
            }
            captured += copy;
        }
        remaining -= static_cast<size_t>(received);
    }
    uint8_t digest[16];
    esp_rom_md5_final(digest, &md5);
    for (size_t i = 0; i < sizeof(digest); ++i) {
        snprintf(capture.requestMd5 + i * 2, 3, "%02x", static_cast<unsigned>(digest[i]));
    }
    if (body && bodyLength < bodyCapacity) body[bodyLength] = '\0';
    return true;
}

void finishJsonError(httpd_req_t* request, Hit& hit, char* respBody, size_t respCapacity,
                      int status, const char* json) {
    setStatusLine(request, status);
    httpd_resp_set_type(request, "application/json");
    const size_t length = strlen(json);
    httpd_resp_send(request, json, length);
    hit.status = static_cast<uint16_t>(status);
    hit.bytes = static_cast<uint32_t>(length);
    md5Hex(json, length, hit.md5);
    strlcpy(hit.respCtype, "application/json", sizeof(hit.respCtype));
    copyPrintable(json, length, hit.respHead, sizeof(hit.respHead));
    if (respBody && respCapacity) strlcpy(respBody, json, respCapacity);
}

int forwardRequest(httpd_req_t* request, const char* body, size_t bodyLength,
                   Hit& hit, char* respBody, size_t respCapacity) {
    hit.status = 0;
    hit.bytes = 0;
    hit.md5[0] = '\0';
    hit.respHead[0] = '\0';
    hit.respCtype[0] = '\0';
    if (respBody && respCapacity) respBody[0] = '\0';

    // Windows runs reverse:https://api-prd.vn.cloud.tesla.cn@443, i.e. the
    // upstream is fixed by config and the client's Host header is not reused
    // (which also prevents a 9.9.9.9 -> 9.9.9.9 self-loop).
    char hostOnly[kHostCapacity] = {};
    lockState();
    const char* cursor = gState.host;
    while (*cursor) {
        while (*cursor && (isspace(static_cast<unsigned char>(*cursor)) || *cursor == ',' || *cursor == ';')) ++cursor;
        const char* begin = cursor;
        while (*cursor && !(isspace(static_cast<unsigned char>(*cursor)) || *cursor == ',' || *cursor == ';')) ++cursor;
        if (cursor > begin && *begin != '*') {
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
        finishJsonError(request, hit, respBody, respCapacity, 500, "{\"error\":\"no_memory\"}");
        return 500;
    }
    snprintf(url, urlLength, "https://%s%s", hostOnly, request->uri);

    esp_http_client_config_t config = {};
    config.url = url;
    config.method = upstreamMethod(request->method);
    config.timeout_ms = 8000;
    config.disable_auto_redirect = true;
    config.transport_type = HTTP_TRANSPORT_OVER_SSL;
    config.skip_cert_common_name_check = true;
    config.buffer_size = 2048;
    config.buffer_size_tx = 2048;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(url);
        finishJsonError(request, hit, respBody, respCapacity, 502, "{\"error\":\"upstream_init\"}");
        return 502;
    }

    // IDF 4.4 has no public header-iteration API; forward the headers Tesla uses.
    static const char* const kForwardHeaders[] = {
        "Authorization", "Content-Type", "Accept", "User-Agent", "Cookie",
        "X-Tesla-User-Agent", "Tesla-User-Agent", "X-Tesla-Client",
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
    if (bodyLength > 0) esp_http_client_set_post_field(client, body, static_cast<int>(bodyLength));

    esp_err_t error = esp_http_client_perform(client);
    if (error != ESP_OK) {
        esp_http_client_cleanup(client);
        free(url);
        finishJsonError(request, hit, respBody, respCapacity, 502, "{\"error\":\"upstream_failed\"}");
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
    char* cacheControl = nullptr;
    if (esp_http_client_get_header(client, "Cache-Control", &cacheControl) == ESP_OK && cacheControl) {
        httpd_resp_set_hdr(request, "Cache-Control", cacheControl);
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
            copyPrintable(buffer, copy, hit.respHead + preview, sizeof(hit.respHead) - preview);
            preview += copy;
        }
        if (respBody && respCapacity && bodyStored + 1 < respCapacity) {
            const size_t copy = (respCapacity - 1 - bodyStored) < static_cast<size_t>(read)
                ? (respCapacity - 1 - bodyStored) : static_cast<size_t>(read);
            memcpy(respBody + bodyStored, buffer, copy);
            bodyStored += copy;
            respBody[bodyStored] = '\0';
        }
        if (httpd_resp_send_chunk(request, buffer, read) != ESP_OK) {
            total += static_cast<size_t>(read);
            break;
        }
        total += static_cast<size_t>(read);
    }
    httpd_resp_send_chunk(request, nullptr, 0);
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

static int fsdLogHook(const char* format, va_list args) {
    if (!gInLogHook && format) {
        if (strstr(format, "mbedtls_ssl_handshake returned")) {
            char message[96];
            va_list copy;
            va_copy(copy, args);
            vsnprintf(message, sizeof(message), format, copy);
            va_end(copy);
            gInLogHook = true;
            noteHandshakeFailure(message);
            gInLogHook = false;
        } else if (strstr(format, "esp_tls_create_server_session failed")) {
            gInLogHook = true;
            noteHandshakeFailure("esp_tls_create_server_session failed");
            gInLogHook = false;
        }
    }
    return gPreviousLog ? gPreviousLog(format, args) : vprintf(format, args);
}

void installLogHook() {
    if (!gPreviousLog) gPreviousLog = esp_log_set_vprintf(fsdLogHook);
}

void noteHandshakeFailure(const char* error) {
    const uint32_t now = millis();
    bool logNow = false;
    portENTER_CRITICAL(&gHandshakeMux);
    // esp_https_server emits two error lines for one failed handshake
    // (mbedtls code + generic session failure); count the pair once.
    const bool newFailure = gHandshake.fail == 0 || now - gHandshake.lastFailMs > 1000;
    if (newFailure) ++gHandshake.fail;
    if (error && (newFailure || strlen(error) > strlen(gHandshake.lastError))) {
        strlcpy(gHandshake.lastError, error, sizeof(gHandshake.lastError));
    }
    gHandshake.lastFailMs = now;
    if (now - gHandshake.lastFailLogMs >= 5000) {
        gHandshake.lastFailLogMs = now;
        logNow = true;
    }
    portEXIT_CRITICAL(&gHandshakeMux);
    // A client that retries a broken handshake can otherwise force a SPIFFS
    // write per attempt and starve the Wi-Fi/HTTP tasks.
    if (logNow) appendHandshakeLog("handshake_failed", "", "", "", "", gHandshake.lastError[0] ? gHandshake.lastError : "handshake_failed");
}

void noteHandshake(const esp_tls_t* tls) {
    char client[16] = {};
    char version[16] = {};
    char cipher[64] = {};
    char alpn[16] = {};
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
    ++gHandshake.ok;
    gHandshake.lastOkMs = millis();
    strlcpy(gHandshake.lastClient, client, sizeof(gHandshake.lastClient));
    strlcpy(gHandshake.lastTls, version, sizeof(gHandshake.lastTls));
    strlcpy(gHandshake.lastCipher, cipher, sizeof(gHandshake.lastCipher));
    strlcpy(gHandshake.lastAlpn, alpn, sizeof(gHandshake.lastAlpn));
    portEXIT_CRITICAL(&gHandshakeMux);
    appendHandshakeLog("handshake_ok", client, version, cipher, alpn, nullptr);
}

void begin() {
    gStateMutex = xSemaphoreCreateMutex();
    if (!gStateMutex) return;

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
    Capture capture;
    extractVin(request->uri, capture.vin);
    char client[16] = {};
    clientAddress(request, client, sizeof(client));

    Hit hit;
    hit.uptimeMs = millis();
    strlcpy(hit.client, client, sizeof(hit.client));
    strlcpy(hit.method, httpMethodName(request->method), sizeof(hit.method));
    strlcpy(hit.url, request->uri, sizeof(hit.url));
    strlcpy(hit.vin, capture.vin, sizeof(hit.vin));
    hit.reqBytes = static_cast<uint32_t>(request->content_len);

    const bool mockEnabled = enabled();
    const bool observe = mode() == Mode::Observe && mockEnabled;
    strlcpy(hit.kind, observe ? "OBSERVE" : "HIT", sizeof(hit.kind));
    strlcpy(hit.mode, observe ? "observe" : "reply", sizeof(hit.mode));
    if (observe && request->content_len > kForwardBodyBytes) {
        finishJsonError(request, hit, nullptr, 0, 413, "{\"error\":\"request_too_large\"}");
        recordHit(hit);
        setLastResponse(hit, "{\"error\":\"request_too_large\"}");
        return ESP_OK;
    }
    char* forwarded = nullptr;
    if (observe) {
        forwarded = static_cast<char*>(malloc(kForwardBodyBytes + 1));
    }
    size_t forwardedLength = 0;
    const bool bodyOk = drainRequestBody(request, forwarded, kForwardBodyBytes + 1, forwardedLength, capture);
    strlcpy(hit.reqMd5, capture.requestMd5, sizeof(hit.reqMd5));
    strlcpy(hit.reqHead, capture.head, sizeof(hit.reqHead));
    hit.reqBytes = capture.requestBytes;
    if (!bodyOk) {
        free(forwarded);
        finishJsonError(request, hit, nullptr, 0, 400, "{\"error\":\"request_read_failed\"}");
        recordHit(hit);
        setLastResponse(hit, "{\"error\":\"request_read_failed\"}");
        return ESP_OK;
    }

    if (!mockEnabled) {
        free(forwarded);
        finishJsonError(request, hit, nullptr, 0, 404, "{\"error\":\"fsd_mock_disabled\"}");
        recordHit(hit);
        setLastResponse(hit, "{\"error\":\"fsd_mock_disabled\"}");
        return ESP_OK;
    }

    if (observe) {
        char responseBody[2049];
        forwardRequest(request, forwarded, forwardedLength, hit, responseBody, sizeof(responseBody));
        free(forwarded);
        recordHit(hit);
        setLastResponse(hit, responseBody);
        return ESP_OK;
    }

    // Reply mode: byte-for-byte Windows fsd_mock response. The httpd task is
    // single-threaded, so one static send buffer is enough.
    char checksum[33];
    size_t bytes = 0;
    lockState();
    bytes = gState.bytes;
    memcpy(gSendBuffer, gState.payload, bytes + 1);
    strlcpy(checksum, gState.md5, sizeof(checksum));
    unlockState();

    setStatusLine(request, 200);
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "X-Checksum", checksum);
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    const esp_err_t result = httpd_resp_send(request, gSendBuffer, bytes);

    hit.status = 200;
    hit.bytes = static_cast<uint32_t>(bytes);
    strlcpy(hit.md5, checksum, sizeof(hit.md5));
    strlcpy(hit.respCtype, "application/json", sizeof(hit.respCtype));
    copyPrintable(gSendBuffer, bytes, hit.respHead, sizeof(hit.respHead));
    recordHit(hit);
    setLastResponse(hit, gSendBuffer);
    free(forwarded);
    return result;
}

esp_err_t forwardPassthrough(httpd_req_t* request) {
    Capture capture;
    extractVin(request->uri, capture.vin);
    char client[16] = {};
    clientAddress(request, client, sizeof(client));

    Hit hit;
    hit.uptimeMs = millis();
    strlcpy(hit.kind, "FLOW", sizeof(hit.kind));
    strlcpy(hit.mode, "flow", sizeof(hit.mode));
    strlcpy(hit.client, client, sizeof(hit.client));
    strlcpy(hit.method, httpMethodName(request->method), sizeof(hit.method));
    strlcpy(hit.url, request->uri, sizeof(hit.url));
    strlcpy(hit.vin, capture.vin, sizeof(hit.vin));
    hit.reqBytes = static_cast<uint32_t>(request->content_len);

    if (!enabled()) {
        finishJsonError(request, hit, nullptr, 0, 404, "{\"error\":\"fsd_mock_disabled\"}");
        recordHit(hit);
        setLastResponse(hit, "{\"error\":\"fsd_mock_disabled\"}");
        return ESP_OK;
    }
    if (request->content_len > kForwardBodyBytes) {
        finishJsonError(request, hit, nullptr, 0, 413, "{\"error\":\"request_too_large\"}");
        recordHit(hit);
        setLastResponse(hit, "{\"error\":\"request_too_large\"}");
        return ESP_OK;
    }

    char* body = static_cast<char*>(malloc(kForwardBodyBytes + 1));
    size_t bodyLength = 0;
    if (!drainRequestBody(request, body, kForwardBodyBytes + 1, bodyLength, capture)) {
        free(body);
        finishJsonError(request, hit, nullptr, 0, 400, "{\"error\":\"request_read_failed\"}");
        recordHit(hit);
        setLastResponse(hit, "{\"error\":\"request_read_failed\"}");
        return ESP_OK;
    }
    strlcpy(hit.reqMd5, capture.requestMd5, sizeof(hit.reqMd5));
    strlcpy(hit.reqHead, capture.head, sizeof(hit.reqHead));
    hit.reqBytes = capture.requestBytes;

    char responseBody[2049];
    forwardRequest(request, body, bodyLength, hit, responseBody, sizeof(responseBody));
    free(body);
    recordHit(hit);
    setLastResponse(hit, responseBody);
    return ESP_OK;
}

uint32_t dnsRedirectCount() {
    uint32_t total = 0;
    char last[128];
    dnsGetFsdRedirectStats(&total, last, sizeof(last));
    return total;
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
    return gReportedHost;
}

const char* payload() {
    return gState.payload;
}

const char* payloadMd5() {
    return gState.md5;
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
    // Normalize separators to single spaces and lowercase (DNS is case-insensitive).
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
    SPIFFS.remove(kHitLogPath);
}

void clearHandshakes() {
    portENTER_CRITICAL(&gHandshakeMux);
    gHandshake = HandshakeState{};
    portEXIT_CRITICAL(&gHandshakeMux);
    SPIFFS.remove(kHandshakeLogPath);
}

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
    uint32_t flowTotal = 0;
    portENTER_CRITICAL(&gHitMux);
    flowTotal = gFlowTotal;
    portEXIT_CRITICAL(&gHitMux);
    out += ",\"flows\":"; out += String(static_cast<unsigned>(flowTotal));
    out += ",\"dnsRedirects\":"; out += String(static_cast<unsigned>(dnsTotal));
    out += ",\"dnsLast\":\""; jsonEscape(out, dnsLast); out += "\"";

    HandshakeState handshake;
    portENTER_CRITICAL(&gHandshakeMux);
    handshake = gHandshake;
    portEXIT_CRITICAL(&gHandshakeMux);
    out += ",\"handshake\":{";
    out += "\"ok\":"; out += String(static_cast<unsigned long>(handshake.ok));
    out += ",\"fail\":"; out += String(static_cast<unsigned long>(handshake.fail));
    out += ",\"lastOkMs\":"; out += String(static_cast<unsigned long>(handshake.lastOkMs));
    out += ",\"lastFailMs\":"; out += String(static_cast<unsigned long>(handshake.lastFailMs));
    out += ",\"client\":\""; jsonEscape(out, handshake.lastClient); out += "\"";
    out += ",\"tls\":\""; jsonEscape(out, handshake.lastTls); out += "\"";
    out += ",\"cipher\":\""; jsonEscape(out, handshake.lastCipher); out += "\"";
    out += ",\"alpn\":\""; jsonEscape(out, handshake.lastAlpn); out += "\"";
    out += ",\"error\":\""; jsonEscape(out, handshake.lastError); out += "\"";
    out += "}";

    LastResponse* last = new (std::nothrow) LastResponse;
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
        char bodyPreview[513];
        strlcpy(bodyPreview, last->body, sizeof(bodyPreview));
        out += ",\"body\":\""; jsonEscape(out, bodyPreview); out += "\"";
        out += "}";
    }
    delete last;

    lockState();
    out += ",\"payload\":\"";
    jsonEscape(out, gState.payload);
    out += "\"";
    unlockState();

    // 16 × Hit ≈ 5.6 KB: heap, not the AsyncTCP task stack.
    // The 3 s /api/wifi-bridge/status poll only needs the latest few rows.
    constexpr size_t kStatusRecentLimit = 8;
    Hit* recent = new (std::nothrow) Hit[kStatusRecentLimit];
    const size_t count = recent ? recentHits(recent, kStatusRecentLimit) : 0;
    out += ",\"recent\":[";
    for (size_t i = 0; i < count; ++i) {
        if (i) out += ',';
        out += "{\"uptimeMs\":"; out += String(static_cast<unsigned long>(recent[i].uptimeMs));
        out += ",\"client\":\""; jsonEscape(out, recent[i].client); out += "\"";
        out += ",\"method\":\""; jsonEscape(out, recent[i].method); out += "\"";
        out += ",\"url\":\""; jsonEscape(out, recent[i].url); out += "\"";
        out += ",\"vin\":\""; jsonEscape(out, recent[i].vin); out += "\"";
        out += ",\"reqHead\":\""; jsonEscape(out, recent[i].reqHead); out += "\"";
        out += ",\"reqBytes\":"; out += String(static_cast<unsigned long>(recent[i].reqBytes));
        out += ",\"bytes\":"; out += String(static_cast<unsigned long>(recent[i].bytes));
        out += ",\"md5\":\""; jsonEscape(out, recent[i].md5); out += "\"";
        out += ",\"mode\":\""; jsonEscape(out, recent[i].mode); out += "\"";
        out += ",\"kind\":\""; jsonEscape(out, recent[i].kind); out += "\"";
        out += ",\"status\":"; out += String(static_cast<unsigned>(recent[i].status));
        out += ",\"statusText\":\""; out += (recent[i].status == 200 ? "OK" : (recent[i].status == 404 ? "Not Found" : (recent[i].status == 502 ? "Bad Gateway" : ""))); out += "\"";
        out += ",\"ctype\":\""; jsonEscape(out, recent[i].respCtype); out += "\"";
        out += ",\"respHead\":\""; jsonEscape(out, recent[i].respHead); out += "\"";
        out += "}";
    }
    delete[] recent;
    out += "]}";
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

}  // namespace fsd_mock

#endif  // WIFI_BRIDGE_ENABLED && ESP32S3 && ESP_HTTPS_SERVER
