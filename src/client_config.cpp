#include "mod_client_config.h"

#include <sdkconfig.h>
#include <esp_idf_version.h>

#if defined(WIFI_BRIDGE_ENABLED) && defined(CONFIG_IDF_TARGET_ESP32S3) && defined(CONFIG_ESP_HTTPS_SERVER_ENABLE)

#include <Arduino.h>
#include <WiFi.h>
#include <cJSON.h>
#include <esp_err.h>
#include <esp_http_server.h>
#include <esp_https_server.h>
#include <freertos/FreeRTOS.h>
#include <lwip/sockets.h>
#include <cerrno>
#include <memory>

#include "client_config_state.h"
#include "fsd_tls_identity.h"
#include "mod_client_config_identity.h"
#include "mod_fsd_mock.h"
#include "web_ui_client_config.h"

#ifdef WIFI_BRIDGE_ENABLED
extern void wifiBridgeRecoverApOnly();
extern void wifiBridgePrintDiag();
extern void wifiBridgeSerialList();
extern void wifiBridgeSerialAdd(const char* args);
extern void wifiBridgeSerialRemove(const char* ssid);
extern void wifiBridgeSerialEnableUpstream();
extern void wifiBridgeLog(const char* msg);
extern bool wifiBridgeOtaActive();
#endif

namespace client_config {

static nvs_handle_t storage;
static httpd_handle_t fsdServer;
static httpd_handle_t mothershipServer;
static bool initialized = false;
static volatile bool listenerSyncRequested = false;

static constexpr uint32_t kHeartbeatIntervalMs = 15000;
static constexpr uint8_t kMissedPauseDns = 2;    // ~30 s unresponsive -> drop DNS hijack
static constexpr uint32_t kRetryBackoffMs[] = {5000, 15000, 30000};

struct ListenerHealth {
    volatile uint32_t sentSeq = 0;
    volatile uint32_t pongSeq = 0;
    uint8_t missed = 0;
};

static ListenerHealth fsdHealth;
static ListenerHealth mothershipHealth;
static uint32_t lastHealthCheckMs = 0;
static uint32_t nextRetryMs = 0;
static uint8_t retryStep = 0;
static uint32_t gRecoveryCount = 0;

static portMUX_TYPE gWatchdogMux = portMUX_INITIALIZER_UNLOCKED;
static bool gAutoOffScheduled = false;
static uint32_t gAutoOffDeadlineMs = 0;
static char gLastListenerError[64] = {};

static void resetHealth(ListenerHealth& health) {
    health.sentSeq = 0;
    health.pongSeq = 0;
    health.missed = 0;
}

static void setLastErrorText(const char* text) {
    portENTER_CRITICAL(&gWatchdogMux);
    strlcpy(gLastListenerError, text ? text : "", sizeof(gLastListenerError));
    portEXIT_CRITICAL(&gWatchdogMux);
}

static void setLastErrorCode(const char* prefix, esp_err_t err) {
    char msg[64];
    snprintf(msg, sizeof(msg), "%s:%s", prefix, esp_err_to_name(err));
    setLastErrorText(msg);
}

static void clearLastError() {
    setLastErrorText("");
}

static void heartbeatWork(void* arg) {
    auto* health = static_cast<ListenerHealth*>(arg);
    if (health) health->pongSeq = health->sentSeq;
}

static bool readState(ClientConfigState& state) {
    size_t length = sizeof(state);
    const esp_err_t error = nvs_get_blob(storage, "state-v1", &state, &length);
    return error == ESP_ERR_NVS_NOT_FOUND ||
        (error == ESP_OK && length == sizeof(state) && state.valid());
}

static bool writeState(const ClientConfigState& state) {
    return nvs_set_blob(storage, "state-v1", &state, sizeof(state)) == ESP_OK &&
        nvs_commit(storage) == ESP_OK;
}

static void stateJson(const ClientConfigState& state, String& out) {
    char body[kClientConfigCapacity + 96];
    snprintf(body, sizeof(body), "{\"revision\":%lu,\"applied\":%lu,\"canRollback\":%s,\"config\":%s}",
        static_cast<unsigned long>(state.revision), static_cast<unsigned long>(state.applied),
        state.previous[0] ? "true" : "false", state.current);
    out = body;
}

static void errorJson(int code, const char* error, String& out, int& status) {
    status = code;
    out = "{\"error\":\"";
    out += error;
    out += "\"}";
}

static bool matchesToken(const char* supplied, const char* expected) {
    volatile uint8_t difference = 0;
    for (size_t index = 0; index < 64; ++index) {
        difference |= static_cast<uint8_t>(supplied[index] ^ expected[index]);
    }
    return difference == 0;
}

enum class Access { Reader, Admin, Client };

static bool authorized(const char* header, Access access) {
    constexpr char prefix[] = "Bearer ";
    constexpr size_t tokenLength = 64;
    if (!header) return false;
    // Exactly "Bearer " + 64 hex characters, no trailing garbage.
    if (strlen(header) != sizeof(prefix) - 1 + tokenLength) return false;
    if (strncmp(header, prefix, sizeof(prefix) - 1) != 0) return false;
    const char* token = header + sizeof(prefix) - 1;
    const bool admin = matchesToken(token, identity.adminToken);
    const bool client = matchesToken(token, identity.clientToken);
    if (access == Access::Admin) return admin;
    if (access == Access::Client) return client;
    return admin || client;
}

bool apiGet(const char* authorization, String& response, int& status) {
    if (!authorized(authorization, Access::Reader)) {
        errorJson(401, "unauthorized", response, status);
        return false;
    }
    ClientConfigState state;
    if (!readState(state)) {
        errorJson(500, "storage", response, status);
        return false;
    }
    stateJson(state, response);
    status = 200;
    return true;
}

static bool safeJsonDepth(const char* json) {
    unsigned depth = 0;
    bool quoted = false;
    for (const char* p = json; *p; ++p) {
        if (quoted && *p == '\\' && p[1]) {
            // cJSON represents strings as NUL-terminated UTF-8.
            if (strncmp(p + 1, "u0000", 5) == 0) return false;
            ++p;
        } else if (*p == '"') {
            quoted = !quoted;
        } else if (!quoted && (*p == '{' || *p == '[')) {
            if (++depth > 16) return false;
        } else if (!quoted && (*p == '}' || *p == ']')) {
            if (depth == 0) return false;
            --depth;
        }
    }
    return !quoted && depth == 0;
}

bool apiChange(const char* authorization, const char* body, size_t length,
               Change change, String& response, int& status) {
    const Access access = change == Change::Acknowledge ? Access::Client : Access::Admin;
    if (!authorized(authorization, access)) {
        errorJson(401, "unauthorized", response, status);
        return false;
    }
    if (!body || length == 0 || length >= kClientConfigCapacity + 64) {
        errorJson(413, "config_too_large", response, status);
        return false;
    }

    // The AsyncWebServer body handler already materialised the request; copy
    // only to guarantee NUL termination and to run the same depth checks.
    char input[kClientConfigCapacity + 64];
    memcpy(input, body, length);
    input[length] = '\0';
    if (memchr(input, '\0', length) || !safeJsonDepth(input)) {
        errorJson(400, "invalid_json", response, status);
        return false;
    }
    const std::unique_ptr<cJSON, decltype(&cJSON_Delete)> parsed(
        cJSON_ParseWithLengthOpts(input, length + 1, nullptr, true), cJSON_Delete);
    const cJSON* revision = cJSON_GetObjectItemCaseSensitive(parsed.get(), "revision");
    if (!cJSON_IsObject(parsed.get()) || !cJSON_IsNumber(revision) ||
        !(revision->valuedouble >= 1 && revision->valuedouble <= UINT32_MAX) ||
        revision->valuedouble != static_cast<uint32_t>(revision->valuedouble)) {
        errorJson(400, "invalid_revision", response, status);
        return false;
    }

    ClientConfigState state;
    if (!readState(state)) {
        errorJson(500, "storage", response, status);
        return false;
    }
    if (state.revision != static_cast<uint32_t>(revision->valuedouble)) {
        errorJson(409, "revision_conflict", response, status);
        return false;
    }
    const uint32_t originalRevision = state.revision;
    const uint32_t originalApplied = state.applied;
    switch (change) {
        case Change::Publish: {
            cJSON* config = cJSON_GetObjectItemCaseSensitive(parsed.get(), "config");
            if (!cJSON_IsObject(config)) {
                errorJson(400, "expected_object", response, status);
                return false;
            }
            // cJSON_PrintPreallocated needs a few bytes of extra working space.
            char json[kClientConfigCapacity + 5];
            if (!cJSON_PrintPreallocated(config, json, sizeof(json), false) ||
                strlen(json) >= kClientConfigCapacity) {
                errorJson(413, "config_too_large", response, status);
                return false;
            }
            if (!state.publish(json)) {
                errorJson(409, "revision_exhausted", response, status);
                return false;
            }
            break;
        }
        case Change::Rollback:
            if (!state.rollback()) {
                errorJson(409, "rollback_unavailable", response, status);
                return false;
            }
            break;
        case Change::Acknowledge:
            state.acknowledge(static_cast<uint32_t>(revision->valuedouble));
            break;
    }
    if ((state.revision != originalRevision || state.applied != originalApplied) && !writeState(state)) {
        errorJson(500, "storage", response, status);
        return false;
    }
    stateJson(state, response);
    status = 200;
    return true;
}

const char* dashboardHtml() {
    return CLIENT_CONFIG_HTML;
}

static bool matchUri(const char* registered, const char* requested, size_t) {
    if (strcmp(registered, "/*") == 0) return true;
    const size_t length = strlen(registered);
    if (strncmp(registered, requested, length) != 0) return false;
    return requested[length] == '\0' || requested[length] == '?';
}

static esp_err_t catchAll(httpd_req_t* request) {
    if (fsd_mock::isFeatureConfigurationUri(request->uri)) {
        return fsd_mock::handleRequest(request);
    }
    // Same reverse-mode behavior as the Windows mitmdump chain: everything
    // else goes to the real api-prd host instead of being answered locally.
    return fsd_mock::forwardPassthrough(request);
}

// Request/response limits shared by 443 and 4567 (TeslaDG mitmproxy parity).
// IDF's default 1024 B header cap rejects real Tesla JWT/cookie headers and
// browser probes with HTTP 431 ("Header fields are too long") before catchAll.
static constexpr size_t kFsdMaxReqHdrLen = 8192;
static constexpr size_t kFsdMaxUriLen = 1024;
static constexpr size_t kFsdMaxRespHeaders = 28;

// PC TeslaDG runs a second mitmproxy listener on 4567:
//   reverse:http://<mothership-ip>:4567 + keep_host_header=true
// The car's plaintext mothership requests land there, /feature_configuration
// is answered locally, and everything else is reverse-proxied. A plain HTTP
// server is much cheaper than a second TLS context, so both listeners share
// the same fsd_mock handlers.
static bool startMothershipHttp() {
    if (mothershipServer != nullptr) return true;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = fsd_mock::kMothershipHttpPort;
    config.ctrl_port = 32770;  // 443 TLS server uses ESP_HTTPD_DEF_CTRL_PORT+1 (32769)
    config.uri_match_fn = matchUri;
    config.max_uri_handlers = 8;
    config.max_req_hdr_len = kFsdMaxReqHdrLen;
    config.max_uri_len = kFsdMaxUriLen;
    config.max_resp_headers = kFsdMaxRespHeaders;
    config.core_id = 0;
    config.stack_size = 8192;
    config.max_open_sockets = 1;
    config.backlog_conn = 1;
    // Do not evict the active socket under burst load: each extra TLS
    // handshake allocates AES/SSL heap and can OOM. Queue one reconnect and
    // refuse the rest instead.
    config.lru_purge_enable = false;
    config.keep_alive_enable = true;
    config.keep_alive_idle = 5;
    config.keep_alive_interval = 5;
    config.keep_alive_count = 3;
    config.recv_wait_timeout = 3;
    config.send_wait_timeout = 3;

    const esp_err_t startErr = httpd_start(&mothershipServer, &config);
    if (startErr != ESP_OK) {
        mothershipServer = nullptr;
        setLastErrorCode("http_4567_start", startErr);
        return false;
    }

    const httpd_method_t methods[] = {
        HTTP_GET, HTTP_POST, HTTP_PUT, HTTP_DELETE, HTTP_HEAD, HTTP_OPTIONS, HTTP_PATCH,
    };
    for (httpd_method_t method : methods) {
        httpd_uri_t endpoint = {};
        endpoint.uri = "/*";
        endpoint.method = method;
        endpoint.handler = catchAll;
        endpoint.user_ctx = nullptr;
        const esp_err_t regErr = httpd_register_uri_handler(mothershipServer, &endpoint);
        if (regErr == ESP_OK) continue;
        setLastErrorCode("http_4567_uri", regErr);
        httpd_stop(mothershipServer);
        mothershipServer = nullptr;
        return false;
    }
    resetHealth(mothershipHealth);
    return true;
}

bool fsdHttpsStarted() {
    return fsdServer != nullptr;
}

bool mothershipHttpStarted() {
    return mothershipServer != nullptr;
}

uint32_t recoveryCount() {
    return gRecoveryCount;
}

const char* lastListenerError() {
    static char snapshot[sizeof(gLastListenerError)];
    portENTER_CRITICAL(&gWatchdogMux);
    strlcpy(snapshot, gLastListenerError, sizeof(snapshot));
    portEXIT_CRITICAL(&gWatchdogMux);
    return snapshot;
}

// esp_https_server calls this only after a successful TLS handshake, with the
// negotiated TLS context. It lets the dashboard show the same TLS state that
// Windows mitmproxy prints in handshake.log.
static void tlsUserCallback(esp_https_server_user_cb_arg_t* argument) {
    if (argument && argument->tls) fsd_mock::noteHandshake(argument->tls);
}

static bool startFsdHttps() {
    if (fsdServer != nullptr) return true;
    fsd_mock::installLogHook();

    // 443 is FSD-only: the catch-all handles /feature_configuration and
    // reverse-proxies every other vehicle path (Windows mitmdump behavior).
    // Client-config API lives on the 10010 management server, so no second
    // HTTPS server/TLS socket is allocated here.
    httpd_ssl_config_t config = HTTPD_SSL_CONFIG_DEFAULT();
    config.port_secure = kFsdPort;
#if ESP_IDF_VERSION_MAJOR >= 5
    // IDF 5.x renamed the server-cert fields; cacert_pem is now client CA only.
    config.servercert = reinterpret_cast<const uint8_t*>(kFsdTlsCertificate);
    config.servercert_len = strlen(kFsdTlsCertificate) + 1;
    // Cap stalled ClientHello handshakes to 4 s (IDF default is 10 s).
    config.tls_handshake_timeout_ms = 4000;
#else
    config.cacert_pem = reinterpret_cast<const uint8_t*>(kFsdTlsCertificate);
    config.cacert_len = strlen(kFsdTlsCertificate) + 1;
#endif
    config.prvtkey_pem = reinterpret_cast<const uint8_t*>(kFsdTlsPrivateKey);
    config.prvtkey_len = strlen(kFsdTlsPrivateKey) + 1;
    config.user_cb = tlsUserCallback;
    config.httpd.uri_match_fn = matchUri;
    config.httpd.max_uri_handlers = 8;
    config.httpd.max_req_hdr_len = kFsdMaxReqHdrLen;
    config.httpd.max_uri_len = kFsdMaxUriLen;
    config.httpd.max_resp_headers = kFsdMaxRespHeaders;
    config.httpd.core_id = 0;
    config.httpd.stack_size = 10240;
    // One vehicle TLS socket at a time. A second concurrent car connection
    // plus the upstream esp_http_client TLS context can exhaust internal RAM.
    // Do NOT lru-purge: under bursts, purging causes a handshake storm and
    // mbedtls_ssl_handshake -0x7F00 (ALLOC_FAILED). Queue one reconnect and
    // refuse the rest; keep-alive idle is short so the socket frees quickly.
    config.httpd.max_open_sockets = 1;
    config.httpd.backlog_conn = 1;  // serialize reconnects; car uses one TLS socket
    config.httpd.lru_purge_enable = false;
    config.httpd.keep_alive_enable = true;
    config.httpd.keep_alive_idle = 5;
    config.httpd.keep_alive_interval = 5;
    config.httpd.keep_alive_count = 3;
    config.httpd.recv_wait_timeout = 3;
    config.httpd.send_wait_timeout = 3;

    const httpd_uri_t fsdEndpoints[] = {
        {"/*", HTTP_GET, catchAll, nullptr},
        {"/*", HTTP_POST, catchAll, nullptr},
        {"/*", HTTP_PUT, catchAll, nullptr},
        {"/*", HTTP_DELETE, catchAll, nullptr},
        {"/*", HTTP_HEAD, catchAll, nullptr},
        {"/*", HTTP_OPTIONS, catchAll, nullptr},
        {"/*", HTTP_PATCH, catchAll, nullptr},
    };
    const esp_err_t startErr = httpd_ssl_start(&fsdServer, &config);
    if (startErr != ESP_OK) {
        fsdServer = nullptr;
        setLastErrorCode("https_443_start", startErr);
        return false;
    }
    for (const auto& endpoint : fsdEndpoints) {
        const esp_err_t regErr = httpd_register_uri_handler(fsdServer, &endpoint);
        if (regErr == ESP_OK) continue;
        setLastErrorCode("https_443_uri", regErr);
        httpd_ssl_stop(fsdServer);
        fsdServer = nullptr;
        return false;
    }
    resetHealth(fsdHealth);
    return true;
}

static void stopMothershipHttp() {
    if (!mothershipServer) return;
    httpd_stop(mothershipServer);
    mothershipServer = nullptr;
    resetHealth(mothershipHealth);
}

static void stopFsdHttps() {
    if (!fsdServer) return;
    httpd_ssl_stop(fsdServer);
    fsdServer = nullptr;
    resetHealth(fsdHealth);
}

static bool syncFsdListenersNow() {
    if (!initialized) return false;
    const bool want = fsd_mock::enabled();
    if (want) {
        const bool httpsOk = startFsdHttps();
        if (httpsOk) {
            const bool httpOk = mothershipServer != nullptr || startMothershipHttp();
            // Only redirect api-prd/mothership after 443 is actually accepting connections.
            fsd_mock::setListenersReady(true);
            if (httpOk) clearLastError();
            lastHealthCheckMs = millis();
            return httpOk;
        }
        fsd_mock::setListenersReady(false);
        return false;
    }

    // Stop DNS redirect FIRST so clients never hit a closing listener, then
    // reclaim ~28 KB RAM, 2 Core-0 tasks, and 6 BSD sockets.
    cancelAutoOff();
    fsd_mock::setListenersReady(false);
    stopMothershipHttp();
    stopFsdHttps();
    clearLastError();
    retryStep = 0;
    nextRetryMs = 0;
    lastHealthCheckMs = 0;
    return true;
}

void requestListenerSync() {
    listenerSyncRequested = true;
}

void scheduleAutoOff(uint32_t delayMs) {
    const uint32_t now = millis();
    portENTER_CRITICAL(&gWatchdogMux);
    // Keep the first successful delivery's deadline so periodic car retries
    // during the 2-minute window still get served without pushing off shutdown.
    if (!gAutoOffScheduled) {
        gAutoOffScheduled = true;
        gAutoOffDeadlineMs = now + delayMs;
    }
    portEXIT_CRITICAL(&gWatchdogMux);
}

void cancelAutoOff() {
    portENTER_CRITICAL(&gWatchdogMux);
    gAutoOffScheduled = false;
    gAutoOffDeadlineMs = 0;
    portEXIT_CRITICAL(&gWatchdogMux);
}

bool autoOffPending() {
    portENTER_CRITICAL(&gWatchdogMux);
    const bool pending = gAutoOffScheduled;
    portEXIT_CRITICAL(&gWatchdogMux);
    return pending;
}

uint32_t autoOffRemainingSec() {
    const uint32_t now = millis();
    portENTER_CRITICAL(&gWatchdogMux);
    const bool pending = gAutoOffScheduled;
    const uint32_t deadline = gAutoOffDeadlineMs;
    portEXIT_CRITICAL(&gWatchdogMux);
    if (!pending) return 0;
    const int32_t remMs = static_cast<int32_t>(deadline - now);
    if (remMs <= 0) return 0;
    return static_cast<uint32_t>((remMs + 999) / 1000);
}

static void checkServerHeartbeat(httpd_handle_t server, ListenerHealth& health,
                                 const char* label, const char* errTag) {
    if (!server) return;
    if (health.sentSeq != health.pongSeq) {
        if (health.missed < 255) ++health.missed;
        if (health.missed == 1) {
            char msg[64];
            snprintf(msg, sizeof(msg), "FSD %s heartbeat delayed (15s)", label);
            wifiBridgeLog(msg);
        } else if (health.missed == kMissedPauseDns) {
            // Never pause DNS hijack for a busy httpd task: a slow upstream
            // request would otherwise make api-prd intermittently resolve
            // somewhere else. Keep hijacking; only record the delay.
            setLastErrorText(errTag);
            char msg[64];
            snprintf(msg, sizeof(msg), "FSD %s heartbeat delayed 30s (DNS stays hijacked)", label);
            wifiBridgeLog(msg);
        }
        // Do not flood the control mailbox while a previous ping is still queued.
        return;
    }

    if (health.missed > 0) {
        health.missed = 0;
        ++gRecoveryCount;
        clearLastError();
        char msg[64];
        snprintf(msg, sizeof(msg), "FSD %s heartbeat recovered", label);
        wifiBridgeLog(msg);
    }

    const uint32_t nextSeq = health.sentSeq + 1;
    health.sentSeq = nextSeq;
    if (httpd_queue_work(server, heartbeatWork, &health) != ESP_OK) {
        // Leave sentSeq != pongSeq so the next cycle escalates if ctrl queue stays stuck.
    }
}

void serviceListeners() {
    if (!initialized) return;

    // 1) Non-blocking 2-minute post-delivery auto-disable timer.
    const uint32_t now = millis();
    bool dueAutoOff = false;
    portENTER_CRITICAL(&gWatchdogMux);
    if (gAutoOffScheduled && static_cast<int32_t>(now - gAutoOffDeadlineMs) >= 0) {
        gAutoOffScheduled = false;
        gAutoOffDeadlineMs = 0;
        dueAutoOff = true;
    }
    portEXIT_CRITICAL(&gWatchdogMux);

    if (dueAutoOff && fsd_mock::enabled() && fsd_mock::autoOffOnHit()) {
        fsd_mock::setEnabled(false);
        fsd_mock::noteAutoDisabled();
        syncFsdListenersNow();
        wifiBridgeLog("FSD delivered -> auto-disabled (2m delay)");
        return;
    }

    // 2) Explicit toggle / config sync from Web UI or API.
    if (listenerSyncRequested) {
        listenerSyncRequested = false;
        retryStep = 0;
        nextRetryMs = 0;
        const bool want = fsd_mock::enabled();
        const bool ok = syncFsdListenersNow();
        if (want) {
            if (ok) {
                wifiBridgeLog("FSD HTTPS 443 + HTTP 4567 started");
            } else {
                char msg[96];
                snprintf(msg, sizeof(msg), "FSD start failed (%.36s, heap=%u)",
                         lastListenerError(), static_cast<unsigned>(ESP.getFreeHeap()));
                wifiBridgeLog(msg);
                nextRetryMs = now + kRetryBackoffMs[0];
                retryStep = 1;
            }
        } else {
            wifiBridgeLog("FSD mock disabled; listeners stopped");
        }
        return;
    }

    if (wifiBridgeOtaActive() || !fsd_mock::enabled()) return;

    // 3) Missing-listener automatic retry with backoff (5s -> 15s -> 30s).
    if (!fsdServer || !mothershipServer) {
        if (nextRetryMs == 0 || static_cast<int32_t>(now - nextRetryMs) >= 0) {
            const bool had443 = fsdServer != nullptr;
            const bool had4567 = mothershipServer != nullptr;
            const bool httpsOk = had443 || startFsdHttps();
            const bool httpOk = httpsOk && (had4567 || startMothershipHttp());
            if (httpsOk) {
                fsd_mock::setListenersReady(true);
            }
            if (httpsOk && httpOk) {
                ++gRecoveryCount;
                clearLastError();
                retryStep = 0;
                nextRetryMs = 0;
                lastHealthCheckMs = now;
                wifiBridgeLog("FSD listeners auto-recovered (443+4567)");
            } else {
                const uint8_t idx = retryStep < 3 ? retryStep : 2;
                nextRetryMs = now + kRetryBackoffMs[idx];
                if (retryStep < 2) ++retryStep;
                if (idx == 0) {
                    char msg[96];
                    snprintf(msg, sizeof(msg), "FSD retry failed (%.36s, heap=%u)",
                             lastListenerError(), static_cast<unsigned>(ESP.getFreeHeap()));
                    wifiBridgeLog(msg);
                }
            }
        }
    }

    // 4) Zero-intrusion httpd_queue_work() heartbeat every 15 s (never touches
    //    active car sockets or TLS sessions) + DNS redirect self-check.
    if (now - lastHealthCheckMs < kHeartbeatIntervalMs) return;
    lastHealthCheckMs = now;

    if (fsdServer) {
        fsd_mock::resyncDnsRedirect();
    }
    checkServerHeartbeat(fsdServer, fsdHealth, "443",
                         "https_443_unresponsive");
    checkServerHeartbeat(mothershipServer, mothershipHealth, "4567",
                         "http_4567_unresponsive");
}

bool start() {
    if (nvs_open("client-config", NVS_READWRITE, &storage) != ESP_OK) return false;
    if (!loadIdentity(storage)) {
        nvs_close(storage);
        return false;
    }
    initialized = true;

    // Load FSD mock settings & payload first; only start 443/4567 listeners
    // when the FSD virtual server is enabled (default OFF to keep maximum
    // RAM/Wi-Fi headroom).
    fsd_mock::begin();
    if (fsd_mock::enabled()) {
        if (!syncFsdListenersNow()) {
            nextRetryMs = millis() + kRetryBackoffMs[0];
            retryStep = 1;
        }
    }
    return true;
}

void serviceSerial() {
    static char command[100];
    static size_t length = 0;
    for (int budget = 48; budget > 0 && Serial.available(); --budget) {
        const int value = Serial.read();
        if (value == '\r') continue;
        if (value == '\n') {
            if (length < sizeof(command)) {
                command[length] = '\0';
                if (strcmp(command, "client-config") == 0) {
                    // Same trust anchors as TeslaDG/app/certs2 --emudhra.
                    Serial.print(kFsdTlsCaCertificate);
                    Serial.printf("\nadmin_token=%s\nclient_token=%s\n", identity.adminToken, identity.clientToken);
                } else if (strcmp(command, "wifi-ap-only") == 0) {
                    wifiBridgeRecoverApOnly();
                } else if (strcmp(command, "wifi-diag") == 0) {
                    wifiBridgePrintDiag();
                } else if (strcmp(command, "wifi-list") == 0) {
                    wifiBridgeSerialList();
                } else if (strcmp(command, "wifi-up") == 0) {
                    wifiBridgeSerialEnableUpstream();
                } else if (strncmp(command, "wifi-add ", 9) == 0) {
                    wifiBridgeSerialAdd(command + 9);
                } else if (strncmp(command, "wifi-del ", 9) == 0) {
                    wifiBridgeSerialRemove(command + 9);
                } else if (strcmp(command, "fsd-payload") == 0) {
                    char* payload = static_cast<char*>(malloc(fsd_mock::kPayloadCapacity));
                    if (payload) {
                        const size_t payloadLength = fsd_mock::copyPayload(payload, fsd_mock::kPayloadCapacity);
                        Serial.write(reinterpret_cast<const uint8_t*>(payload), payloadLength);
                        Serial.println();
                        free(payload);
                    }
                } else if (strcmp(command, "net-test") == 0) {
                    IPAddress address;
                    const int dns = WiFi.hostByName("api-prd.vn.cloud.tesla.cn", address);
                    Serial.printf("net-test dns=%d ip=%s\n", dns, address.toString().c_str());
                    if (dns == 1) {
                        const int sock = socket(AF_INET, SOCK_STREAM, 0);
                        if (sock >= 0) {
                            struct sockaddr_in target = {};
                            target.sin_family = AF_INET;
                            target.sin_port = htons(443);
                            target.sin_addr.s_addr = static_cast<uint32_t>(address);
                            struct timeval tv = {5, 0};
                            setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
                            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                            const int connected = connect(sock, reinterpret_cast<struct sockaddr*>(&target), sizeof(target));
                            Serial.printf("net-test tcp=%d errno=%d heap=%u\n", connected, errno,
                                          static_cast<unsigned>(ESP.getFreeHeap()));
                            close(sock);
                        }
                    }
                } else if (strcmp(command, "fsd-on") == 0) {
                    cancelAutoOff();
                                fsd_mock::setEnabled(true);
                    syncFsdListenersNow();
                    Serial.printf("[fsd] enabled=1 autoOff=%d fsd443=%d fsdHttp4567=%d heap=%u err=%s\n",
                                  fsd_mock::autoOffOnHit() ? 1 : 0,
                                  fsdHttpsStarted() ? 1 : 0,
                                  mothershipHttpStarted() ? 1 : 0,
                                  static_cast<unsigned>(ESP.getFreeHeap()),
                                  lastListenerError());
                } else if (strcmp(command, "fsd-off") == 0) {
                    cancelAutoOff();
                    fsd_mock::setEnabled(false);
                    syncFsdListenersNow();
                    Serial.printf("[fsd] enabled=0 fsd443=%d fsdHttp4567=%d heap=%u\n",
                                  fsdHttpsStarted() ? 1 : 0,
                                  mothershipHttpStarted() ? 1 : 0,
                                  static_cast<unsigned>(ESP.getFreeHeap()));
                } else if (strcmp(command, "fsd-default") == 0 ||
                           strcmp(command, "fsd-16f") == 0 ||
                           strcmp(command, "fsd-2020") == 0 ||
                           strcmp(command, "fsd-1466") == 0 ||
                           strcmp(command, "fsd-282b") == 0) {
                    const char* preset = strcmp(command, "fsd-default") == 0 ? "default" :
                        (strcmp(command, "fsd-16f") == 0 ? "16f" :
                         (strcmp(command, "fsd-2020") == 0 ? "2020" :
                          (strcmp(command, "fsd-1466") == 0 ? "1466" : "282b")));
                    char error[48] = {};
                    const bool ok = fsd_mock::loadPreset(preset, error, sizeof(error));
                    Serial.printf("fsd preset %s: %s (%u B, md5 %s)\n", preset,
                                  ok ? "ok" : error,
                                  static_cast<unsigned>(fsd_mock::payloadBytes()),
                                  fsd_mock::payloadMd5());
                }
            }
            length = 0;
        } else if (length < sizeof(command)) {
            command[length++] = static_cast<char>(value);
        }
    }
}

}  // namespace client_config

#endif
