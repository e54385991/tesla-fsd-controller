#include "mod_client_config.h"

#include <sdkconfig.h>
#include <esp_idf_version.h>

#if defined(WIFI_BRIDGE_ENABLED) && defined(CONFIG_IDF_TARGET_ESP32S3) && defined(CONFIG_ESP_HTTPS_SERVER_ENABLE)

#include <Arduino.h>
#include <WiFi.h>
#include <cJSON.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_http_server.h>
#include <esp_https_server.h>
#include <esp_tls.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/sockets.h>
#include <mbedtls/platform.h>
#include <mbedtls/ssl.h>
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
        difference = static_cast<uint8_t>(
            difference | static_cast<uint8_t>(supplied[index] ^ expected[index]));
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

// PC TeslaDG runs a second mitmproxy listener on 4567 in router mode:
//   reverse:http://192.0.2.1:4567 + keep_host_header=true (svc/stack.py)
// 192.0.2.1 is TEST-NET (unreachable), so every non-FSD mothership request
// ends in mitmproxy's 502 page instead of reaching Tesla. The car's
// /feature_configuration is answered locally; everything else gets that 502.
// A plain HTTP server is much cheaper than a second TLS context, so both
// listeners share the same fsd_mock handlers.
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
    config.max_open_sockets = 3;
    config.backlog_conn = 4;
    // Allow concurrent keep-alive sockets and evict the oldest idle socket
    // under burst load (esp_http_server never evicts sockets busy in async proxy).
    config.lru_purge_enable = true;
    config.keep_alive_enable = true;
    config.keep_alive_idle = 5;
    config.keep_alive_interval = 3;
    config.keep_alive_count = 2;
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

// esp_https_server calls this on session create/close/error. Recording
// SESS_CREATE mirrors mitmproxy's handshake_ok. Failures are counted only by
// the aa0a69f8b log hook; SESS_ERROR (IDF 6+) never counts, it only appends
// the peer's TLS alert when the logged error is -0x7780.
static void tlsUserCallback(esp_https_server_user_cb_arg_t* argument) {
    if (!argument || !argument->tls) return;
    if (argument->user_cb_state == HTTPD_SSL_USER_CB_SESS_CREATE) {
        fsd_mock::noteHandshake(argument->tls);
    }
#if ESP_IDF_VERSION_MAJOR >= 6
    else if (argument->user_cb_state == HTTPD_SSL_USER_CB_SESS_ERROR) {
        fsd_mock::noteHandshakeError(argument->tls);
    }
#endif
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
    config.httpd.stack_size = 12288;  // IDF 6.1.1 PSA/mbedTLS 4 needs more than 10240
    // The car opens api-prd and mothership TLS connections in parallel. With a
    // single slot the second ClientHello waits behind the first keep-alive
    // socket; the client gives up and sends RST, which mbedTLS reports as
    // MBEDTLS_ERR_NET_CONN_RESET (-0x0050). Give the vehicle enough slots for
    // its parallel connections plus one retry/browser probe. PSRAM-backed
    // mbedTLS sessions cost ~20-30 KB each; on a board without PSRAM fall
    // back to two slots to protect the internal heap.
    // Do NOT lru-purge: evicting an active car socket causes the same reset
    // and a handshake storm. Four slots remove the queueing that produced it.
    const uint8_t tlsSlots = (ESP.getPsramSize() > 0) ? 4 : 2;
    config.httpd.max_open_sockets = tlsSlots;
    config.httpd.backlog_conn = tlsSlots;
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
    fsd_mock::startProxyWorker();
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
    fsd_mock::stopProxyWorker();
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

    // Stop DNS redirect FIRST so clients never hit a closing listener, drain
    // any in-flight async proxy job, then reclaim tasks and BSD sockets.
    cancelAutoOff();
    fsd_mock::setListenersReady(false);
    fsd_mock::stopProxyWorker();
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

#if defined(CONFIG_SPIRAM) && CONFIG_SPIRAM
static void* psramPreferMbedtlsCalloc(size_t n, size_t size) {
    return heap_caps_calloc_prefer(n, size, 2,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static void psramPreferMbedtlsFree(void* ptr) {
    heap_caps_free(ptr);
}
#endif

bool start() {
#if defined(CONFIG_SPIRAM) && CONFIG_SPIRAM
    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0) {
        // Route all mbedTLS/TF-PSA-Crypto dynamic allocations (including
        // sub-4 KB SSL/X.509/PSA structs) to PSRAM first with automatic
        // internal-RAM fallback, keeping internal DMA SRAM free for Wi-Fi,
        // lwIP, and AES-DMA bounce buffers under concurrent TLS sessions.
        mbedtls_platform_set_calloc_free(psramPreferMbedtlsCalloc, psramPreferMbedtlsFree);
    }
#endif
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

static volatile bool gSelfTestRunning = false;

static bool readHttpHeadersAndOptionalBody(int (*readFn)(void*, char*, size_t), void* ctx,
                                           char* out, size_t outCap, size_t& outLen,
                                           bool expectEofClose, bool& sawEof) {
    outLen = 0;
    sawEof = false;
    if (!out || outCap == 0) return false;
    out[0] = '\0';
    bool headersDone = false;
    const uint32_t deadline = millis() + 3000;
    while (outLen + 1 < outCap && static_cast<int32_t>(deadline - millis()) > 0) {
        const int r = readFn(ctx, out + outLen, outCap - 1 - outLen);
        if (r == ESP_TLS_ERR_SSL_WANT_READ || r == ESP_TLS_ERR_SSL_WANT_WRITE) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        if (r == 0) {
            sawEof = true;
            break;
        }
        if (r < 0) break;
        outLen += static_cast<size_t>(r);
        out[outLen] = '\0';
        if (!expectEofClose && strstr(out, "\r\n\r\n") != nullptr) {
            headersDone = true;
            break;
        }
    }
    return expectEofClose ? (outLen > 0) : headersDone;
}

static bool sendFsdHeadOnTls(esp_tls_t* tls, const char* connectionHdr,
                             bool verifyBodyEmpty, char* resp, size_t respCap) {
    char md5Expected[33] = {};
    const size_t bytesExpected = fsd_mock::copyPayload(nullptr, 0, md5Expected, nullptr);

    char req[192];
    const int reqLen = snprintf(req, sizeof(req),
        "HEAD /feature_configuration HTTP/1.1\r\n"
        "Host: api-prd.vn.cloud.tesla.cn\r\n"
        "Connection: %s\r\n\r\n",
        connectionHdr);
    int sent = 0;
    while (sent < reqLen) {
        const ssize_t w = esp_tls_conn_write(tls, req + sent, static_cast<size_t>(reqLen - sent));
        if (w == ESP_TLS_ERR_SSL_WANT_READ || w == ESP_TLS_ERR_SSL_WANT_WRITE) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        if (w <= 0) return false;
        sent += static_cast<int>(w);
    }

    auto readTls = [](void* c, char* b, size_t n) -> int {
        return static_cast<int>(esp_tls_conn_read(static_cast<esp_tls_t*>(c), b, n));
    };
    size_t got = 0;
    bool sawEof = false;
    if (!readHttpHeadersAndOptionalBody(readTls, tls, resp, respCap, got, false, sawEof)) {
        return false;
    }
    const char* hdrEnd = strstr(resp, "\r\n\r\n");
    if (!hdrEnd) return false;
    const size_t hdrBytes = static_cast<size_t>((hdrEnd + 4) - resp);
    if (verifyBodyEmpty && got != hdrBytes) return false;

    char expectedLenHdr[48];
    char expectedMd5Hdr[64];
    snprintf(expectedLenHdr, sizeof(expectedLenHdr), "Content-Length: %u\r\n",
             static_cast<unsigned>(bytesExpected));
    snprintf(expectedMd5Hdr, sizeof(expectedMd5Hdr), "X-Checksum: %s\r\n", md5Expected);
    return (strstr(resp, "200 OK") != nullptr) &&
           (strstr(resp, "Content-Type: application/json\r\n") != nullptr) &&
           (strstr(resp, "Cache-Control: no-store\r\n") != nullptr) &&
           (strstr(resp, expectedLenHdr) != nullptr) &&
           (strstr(resp, expectedMd5Hdr) != nullptr);
}

static bool runLoopbackTlsProbe(esp_tls_proto_ver_t ver, const char* label,
                                bool keepOpen, int extraKeepAliveReqs,
                                esp_tls_t** outKeep, bool verifyPeer = true,
                                uint32_t timeoutMs = 4000) {
    if (outKeep) *outKeep = nullptr;
    esp_tls_cfg_t cfg = {};
    if (verifyPeer) {
        cfg.cacert_buf = reinterpret_cast<const unsigned char*>(kFsdTlsCaCertificate);
        cfg.cacert_bytes = strlen(kFsdTlsCaCertificate) + 1;
        cfg.common_name = "api-prd.vn.cloud.tesla.cn";
    }
    cfg.timeout_ms = timeoutMs;
    cfg.tls_version = ver;

    const uint32_t t0 = millis();
    esp_tls_t* tls = esp_tls_init();
    if (!tls) {
        Serial.printf("[fsd-selftest] %s FAIL alloc\n", label);
        return false;
    }
    const int rc = esp_tls_conn_new_sync("127.0.0.1", 9, kFsdPort, &cfg, tls);
    const uint32_t hsMs = millis() - t0;
    if (rc != 1) {
        esp_tls_error_handle_t eh = nullptr;
        int mbedErr = 0, mbedFlags = 0;
        if (esp_tls_get_error_handle(tls, &eh) == ESP_OK && eh) {
            esp_tls_get_and_clear_last_error(eh, &mbedErr, &mbedFlags);
        }
        Serial.printf("[fsd-selftest] %s FAIL hs rc=%d mbed=-0x%04x flags=0x%x ms=%lu heap=%u\n",
                      label, rc, mbedErr, mbedFlags, static_cast<unsigned long>(hsMs),
                      static_cast<unsigned>(ESP.getFreeHeap()));
        esp_tls_conn_destroy(tls);
        return false;
    }

    const char* tlsVer = "?";
    const char* cipher = "?";
    auto* ssl = static_cast<mbedtls_ssl_context*>(esp_tls_get_ssl_context(tls));
    if (ssl) {
        const char* v = mbedtls_ssl_get_version(ssl);
        const char* c = mbedtls_ssl_get_ciphersuite(ssl);
        if (v) tlsVer = v;
        if (c) cipher = c;
    }

    char resp[768] = {};
    bool ok = sendFsdHeadOnTls(tls, (keepOpen || extraKeepAliveReqs > 0) ? "keep-alive" : "close",
                               true, resp, sizeof(resp));
    for (int i = 0; ok && i < extraKeepAliveReqs; ++i) {
        const char* conn = (keepOpen || (i + 1 < extraKeepAliveReqs)) ? "keep-alive" : "close";
        ok = sendFsdHeadOnTls(tls, conn, true, resp, sizeof(resp));
    }

    const uint32_t totalMs = millis() - t0;
    Serial.printf("[fsd-selftest] %s %s ver=%s cipher=%s hs=%lums total=%lums heap=%u minHeap=%u psram=%u\n",
                  label, ok ? "PASS" : "FAIL", tlsVer, cipher,
                  static_cast<unsigned long>(hsMs),
                  static_cast<unsigned long>(totalMs),
                  static_cast<unsigned>(ESP.getFreeHeap()),
                  static_cast<unsigned>(ESP.getMinFreeHeap()),
                  static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    if (ok && keepOpen && outKeep) {
        *outKeep = tls;
    } else {
        esp_tls_conn_destroy(tls);
    }
    return ok;
}

// Handshake-only probe used for version negotiation and SNI coverage. It does
// not send an HTTP request, so it isolates the TLS layer from the FSD handler.
static bool runLoopbackTlsHandshakeProbe(esp_tls_proto_ver_t ver, const char* commonName,
                                         const char* label, const char* expectVersion) {
    esp_tls_cfg_t cfg = {};
    cfg.cacert_buf = reinterpret_cast<const unsigned char*>(kFsdTlsCaCertificate);
    cfg.cacert_bytes = strlen(kFsdTlsCaCertificate) + 1;
    cfg.common_name = commonName;
    cfg.timeout_ms = 4000;
    cfg.tls_version = ver;

    const uint32_t t0 = millis();
    esp_tls_t* tls = esp_tls_init();
    if (!tls) {
        Serial.printf("[fsd-selftest] %s FAIL alloc\n", label);
        return false;
    }
    const int rc = esp_tls_conn_new_sync("127.0.0.1", 9, kFsdPort, &cfg, tls);
    const uint32_t hsMs = millis() - t0;
    const char* tlsVer = "?";
    const char* cipher = "?";
    if (rc == 1) {
        auto* ssl = static_cast<mbedtls_ssl_context*>(esp_tls_get_ssl_context(tls));
        if (ssl) {
            const char* v = mbedtls_ssl_get_version(ssl);
            const char* c = mbedtls_ssl_get_ciphersuite(ssl);
            if (v) tlsVer = v;
            if (c) cipher = c;
        }
    }
    const bool versionOk = !expectVersion || strcmp(tlsVer, expectVersion) == 0;
    const bool ok = rc == 1 && versionOk;
    Serial.printf("[fsd-selftest] %s %s ver=%s cipher=%s expect=%s hs=%lums heap=%u minHeap=%u\n",
                  label, ok ? "PASS" : "FAIL", tlsVer, cipher,
                  expectVersion ? expectVersion : "-",
                  static_cast<unsigned long>(hsMs),
                  static_cast<unsigned>(ESP.getFreeHeap()),
                  static_cast<unsigned>(ESP.getMinFreeHeap()));
    esp_tls_conn_destroy(tls);
    return ok;
}

struct ConcurrentTlsProbe {
    esp_tls_proto_ver_t ver;
    const char* label;
    volatile bool done;
    bool ok;
};

static void concurrentTlsProbeTask(void* arg) {
    auto* job = static_cast<ConcurrentTlsProbe*>(arg);
    // Skip peer-certificate verification here: P-384 verification on the same
    // ESP32 competes with the server's P-384 signature and made this test a
    // CPU/heap benchmark instead of a socket-slot regression. The sequential
    // tls12/tls13 probes still verify the full chain.
    job->ok = runLoopbackTlsProbe(job->ver, job->label, false, 0, nullptr,
                                  false, 8000);
    job->done = true;
    vTaskDelete(nullptr);
}

// Regression guard for the single-socket bug: the car opens api-prd and
// mothership TLS connections in parallel. With max_open_sockets=1 the second
// ClientHello waited for the first keep-alive socket, the peer RSTs, and
// esp-tls logged -0x0050. Two concurrent probes (the car's two connections)
// must both complete against the four-slot listener.
static bool runConcurrentTlsProbes(const char* label) {
    static ConcurrentTlsProbe jobs[] = {
        {ESP_TLS_VER_TLS_1_2, "concurrent-t12", false, false},
        {ESP_TLS_VER_TLS_1_3, "concurrent-t13", false, false},
    };
    for (auto& job : jobs) {
        job.done = false;
        job.ok = false;
    }

    size_t created = 0;
    for (auto& job : jobs) {
        if (xTaskCreatePinnedToCore(concurrentTlsProbeTask, "TlsProbe", 8192,
                                    &job, 1, nullptr, 1) != pdPASS) {
            Serial.printf("[fsd-selftest] %s FAIL task_create at %u\n",
                          label, static_cast<unsigned>(created));
            break;
        }
        ++created;
    }
    if (created == 0) return false;

    const uint32_t deadline = millis() + 30000;
    while (millis() < deadline) {
        bool allDone = true;
        for (size_t i = 0; i < created; ++i) {
            if (!jobs[i].done) {
                allDone = false;
                break;
            }
        }
        if (allDone) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    bool ok = true;
    size_t doneCount = 0;
    for (size_t i = 0; i < created; ++i) {
        if (!jobs[i].done || !jobs[i].ok) ok = false;
        if (jobs[i].done) ++doneCount;
    }
    Serial.printf("[fsd-selftest] %s %s created=%u done=%u/%u\n",
                  label, ok ? "PASS" : "FAIL",
                  static_cast<unsigned>(created),
                  static_cast<unsigned>(doneCount),
                  static_cast<unsigned>(created));
    return ok;
}

static bool runLoopbackPlainHttpProbe(const char* label, const char* rawReq,
                                      bool expectEofClose,
                                      bool (*validate)(const char* resp, size_t len, bool sawEof)) {
    const uint32_t t0 = millis();
    const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        Serial.printf("[fsd-selftest] %s FAIL socket errno=%d\n", label, errno);
        return false;
    }
    struct timeval tv = {};
    tv.tv_sec = 3;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in dst = {};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(fsd_mock::kMothershipHttpPort);
    dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, reinterpret_cast<const struct sockaddr*>(&dst), sizeof(dst)) != 0) {
        Serial.printf("[fsd-selftest] %s FAIL connect errno=%d\n", label, errno);
        close(fd);
        return false;
    }

    const size_t reqLen = strlen(rawReq);
    size_t sent = 0;
    while (sent < reqLen) {
        const ssize_t w = send(fd, rawReq + sent, reqLen - sent, 0);
        if (w <= 0) break;
        sent += static_cast<size_t>(w);
    }

    char resp[1024] = {};
    size_t got = 0;
    bool sawEof = false;
    auto readSock = [](void* c, char* b, size_t n) -> int {
        const int fdVal = *static_cast<const int*>(c);
        const ssize_t r = recv(fdVal, b, n, 0);
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            return ESP_TLS_ERR_SSL_WANT_READ;
        }
        return static_cast<int>(r);
    };
    const bool readOk = (sent == reqLen) &&
        readHttpHeadersAndOptionalBody(readSock, const_cast<int*>(&fd),
                                       resp, sizeof(resp), got, expectEofClose, sawEof);
    close(fd);

    const bool ok = readOk && validate(resp, got, sawEof);
    Serial.printf("[fsd-selftest] %s %s ms=%lu bytes=%u eof=%d heap=%u minHeap=%u\n",
                  label, ok ? "PASS" : "FAIL",
                  static_cast<unsigned long>(millis() - t0),
                  static_cast<unsigned>(got), sawEof ? 1 : 0,
                  static_cast<unsigned>(ESP.getFreeHeap()),
                  static_cast<unsigned>(ESP.getMinFreeHeap()));
    return ok;
}

static bool runLoopbackTlsFlowProbe(const char* label, esp_tls_proto_ver_t ver) {
    esp_tls_cfg_t cfg = {};
    cfg.cacert_buf = reinterpret_cast<const unsigned char*>(kFsdTlsCaCertificate);
    cfg.cacert_bytes = strlen(kFsdTlsCaCertificate) + 1;
    cfg.common_name = "api-prd.vn.cloud.tesla.cn";
    cfg.timeout_ms = 4000;
    cfg.tls_version = ver;

    const uint32_t t0 = millis();
    esp_tls_t* tls = esp_tls_init();
    if (!tls) return false;
    if (esp_tls_conn_new_sync("127.0.0.1", 9, kFsdPort, &cfg, tls) != 1) {
        Serial.printf("[fsd-selftest] %s FAIL hs\n", label);
        esp_tls_conn_destroy(tls);
        return false;
    }

    static const char req[] =
        "GET /api/1/test HTTP/1.1\r\n"
        "Host: api-prd.vn.cloud.tesla.cn\r\n"
        "Connection: keep-alive\r\n\r\n";
    int sent = 0;
    const int reqLen = static_cast<int>(sizeof(req) - 1);
    while (sent < reqLen) {
        const ssize_t w = esp_tls_conn_write(tls, req + sent, static_cast<size_t>(reqLen - sent));
        if (w == ESP_TLS_ERR_SSL_WANT_READ || w == ESP_TLS_ERR_SSL_WANT_WRITE) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        if (w <= 0) break;
        sent += static_cast<int>(w);
    }

    char resp[1024] = {};
    size_t got = 0;
    bool sawEof = false;
    auto readTls = [](void* c, char* b, size_t n) -> int {
        return static_cast<int>(esp_tls_conn_read(static_cast<esp_tls_t*>(c), b, n));
    };
    const bool readOk = (sent == reqLen) &&
        readHttpHeadersAndOptionalBody(readTls, tls, resp, sizeof(resp), got, true, sawEof);
    esp_tls_conn_destroy(tls);

    // Offline/blocked fast-reject returns 502 or 503 mitmproxy HTML with Connection: close.
    // Online upstream passthrough returns an HTTP status line via FsdProxy.
    const bool isProxyError = (strstr(resp, "502 Bad Gateway") != nullptr ||
                               strstr(resp, "503 Service Unavailable") != nullptr) &&
                              strstr(resp, "Server: mitmproxy 12.3.1\r\n") != nullptr &&
                              strstr(resp, "Connection: close\r\n") != nullptr;
    const bool isUpstreamHttp = strncmp(resp, "HTTP/1.1 ", 9) == 0 && strstr(resp, "\r\n\r\n") != nullptr;
    const bool ok = readOk && (isProxyError || isUpstreamHttp);
    Serial.printf("[fsd-selftest] %s %s ms=%lu bytes=%u proxyErr=%d eof=%d heap=%u minHeap=%u\n",
                  label, ok ? "PASS" : "FAIL",
                  static_cast<unsigned long>(millis() - t0),
                  static_cast<unsigned>(got), isProxyError ? 1 : 0, sawEof ? 1 : 0,
                  static_cast<unsigned>(ESP.getFreeHeap()),
                  static_cast<unsigned>(ESP.getMinFreeHeap()));
    return ok;
}

static void fsdSelfTestTask(void*) {
    int passed = 0;
    int total = 0;
    auto record = [&](bool ok) {
        ++total;
        if (ok) ++passed;
        return ok;
    };

    Serial.printf("[fsd-selftest] start heap=%u minHeap=%u psram=%u\n",
                  static_cast<unsigned>(ESP.getFreeHeap()),
                  static_cast<unsigned>(ESP.getMinFreeHeap()),
                  static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));

    // Version matrix: forced TLS 1.2, forced TLS 1.3, and the default
    // negotiation path. The default must choose TLS 1.3 when both are enabled.
    record(runLoopbackTlsProbe(ESP_TLS_VER_TLS_1_2, "tls12-verify", false, 0, nullptr));
    record(runLoopbackTlsProbe(ESP_TLS_VER_TLS_1_3, "tls13-verify", false, 0, nullptr));
    record(runLoopbackTlsHandshakeProbe(ESP_TLS_VER_ANY, "api-prd.vn.cloud.tesla.cn",
                                        "tls-default-negotiate", "TLSv1.3"));

    // SNI coverage: the static leaf carries api-prd, mothership and fsd.local.
    record(runLoopbackTlsHandshakeProbe(ESP_TLS_VER_TLS_1_2, "mothership.vn.teslamotors.com",
                                        "tls12-sni-mothership", "TLSv1.2"));
    record(runLoopbackTlsHandshakeProbe(ESP_TLS_VER_TLS_1_2, "fsd.local",
                                        "tls12-sni-fsdlocal", "TLSv1.2"));

    // Repeated handshakes catch slot leaks and the TLS1.3->1.2 fallback path.
    for (int i = 0; i < 3; ++i) {
        char label[32];
        snprintf(label, sizeof(label), "tls12-repeat%d", i + 1);
        record(runLoopbackTlsProbe(ESP_TLS_VER_TLS_1_2, label, false, 0, nullptr));
        snprintf(label, sizeof(label), "tls13-repeat%d", i + 1);
        record(runLoopbackTlsProbe(ESP_TLS_VER_TLS_1_3, label, false, 0, nullptr));
    }

    // Keep-alive reuse (2 requests on sockA) and immediate slot release on
    // close so sockB is accepted right away.
    esp_tls_t* sockA = nullptr;
    const bool okA = record(runLoopbackTlsProbe(ESP_TLS_VER_TLS_1_3, "serial-sockA-keepalive",
                                                true, 1, &sockA));
    if (sockA) esp_tls_conn_destroy(sockA);
    bool okB = false;
    if (okA) {
        okB = runLoopbackTlsProbe(ESP_TLS_VER_TLS_1_2, "serial-sockB-reconnect",
                                  false, 0, nullptr);
    }
    record(okB);

    if (mothershipHttpStarted()) {
        record(runLoopbackPlainHttpProbe(
                "http4567-fsd-head",
                "HEAD /feature_configuration HTTP/1.1\r\nHost: mothership.local\r\nConnection: close\r\n\r\n",
                false,
                [](const char* resp, size_t len, bool) -> bool {
                    char md5Expected[33] = {};
                    const size_t bytesExpected = fsd_mock::copyPayload(nullptr, 0, md5Expected, nullptr);
                    char expectedLenHdr[48];
                    char expectedMd5Hdr[64];
                    snprintf(expectedLenHdr, sizeof(expectedLenHdr), "Content-Length: %u\r\n",
                             static_cast<unsigned>(bytesExpected));
                    snprintf(expectedMd5Hdr, sizeof(expectedMd5Hdr), "X-Checksum: %s\r\n", md5Expected);
                    const char* hdrEnd = strstr(resp, "\r\n\r\n");
                    return hdrEnd != nullptr &&
                           len == static_cast<size_t>((hdrEnd + 4) - resp) &&
                           strstr(resp, "200 OK") != nullptr &&
                           strstr(resp, expectedLenHdr) != nullptr &&
                           strstr(resp, expectedMd5Hdr) != nullptr;
                }));
        record(runLoopbackPlainHttpProbe(
                "http4567-flow-502",
                "GET /api/1/test HTTP/1.1\r\nHost: mothership.local\r\nConnection: keep-alive\r\n\r\n",
                true,
                [](const char* resp, size_t, bool sawEof) -> bool {
                    return sawEof &&
                           strstr(resp, "502 Bad Gateway") != nullptr &&
                           strstr(resp, "Server: mitmproxy 12.3.1\r\n") != nullptr &&
                           strstr(resp, "Content-Type: text/html\r\n") != nullptr &&
                           strstr(resp, "Connection: close\r\n") != nullptr &&
                           strstr(resp, "[Errno 113] No route to host") != nullptr;
                }));
    } else {
        Serial.println("[fsd-selftest] http4567 SKIP not_started");
    }

    record(runLoopbackTlsFlowProbe("tls13-443-flow", ESP_TLS_VER_TLS_1_3));
    record(runLoopbackTlsFlowProbe("tls12-443-flow", ESP_TLS_VER_TLS_1_2));
    record(runLoopbackTlsProbe(ESP_TLS_VER_TLS_1_3, "tls13-post-flow", false, 0, nullptr));

    // Regression for mbedtls_ssl_handshake -0x0050 with max_open_sockets=1.
    record(runConcurrentTlsProbes("concurrent-2sockets"));

    Serial.printf("[fsd-selftest] done pass=%d/%d heap=%u minHeap=%u\n",
                  passed, total,
                  static_cast<unsigned>(ESP.getFreeHeap()),
                  static_cast<unsigned>(ESP.getMinFreeHeap()));
    gSelfTestRunning = false;
    vTaskDelete(nullptr);
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
                    // Same trust anchors as TeslaDG's --emudhra wire chain
                    // (not certs2's self-signed mitmproxy CA).
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
                } else if (strcmp(command, "fsd-selftest") == 0) {
                    if (!fsdHttpsStarted()) {
                        Serial.println("[fsd-selftest] FAIL fsd443 not started (run fsd-on first)");
                    } else if (gSelfTestRunning) {
                        Serial.println("[fsd-selftest] already running");
                    } else {
                        gSelfTestRunning = true;
                        if (xTaskCreatePinnedToCore(fsdSelfTestTask, "FsdSelfTest", 8192,
                                                    nullptr, 1, nullptr, 1) != pdPASS) {
                            gSelfTestRunning = false;
                            Serial.println("[fsd-selftest] FAIL task_create");
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
                           strcmp(command, "fsd-2031") == 0 ||
                           strcmp(command, "fsd-1990") == 0 ||
                           strcmp(command, "fsd-2020") == 0 ||
                           strcmp(command, "fsd-1466") == 0 ||
                           strcmp(command, "fsd-282b") == 0) {
                    const char* preset = strcmp(command, "fsd-default") == 0 ? "default" :
                        (strcmp(command, "fsd-16f") == 0 ? "16f" :
                         ((strcmp(command, "fsd-2031") == 0 || strcmp(command, "fsd-1990") == 0 ||
                           strcmp(command, "fsd-2020") == 0) ? "2031" :
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
