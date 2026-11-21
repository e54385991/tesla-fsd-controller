#include "mod_client_config.h"

#include <sdkconfig.h>

#if defined(WIFI_BRIDGE_ENABLED) && defined(CONFIG_IDF_TARGET_ESP32S3) && defined(CONFIG_ESP_HTTPS_SERVER_ENABLE)

#include <Arduino.h>
#include <cJSON.h>
#include <esp_https_server.h>
#include <memory>

#include "client_config_state.h"
#include "fsd_tls_identity.h"
#include "mod_client_config_identity.h"
#include "mod_fsd_mock.h"
#include "web_ui_client_config.h"

#ifdef WIFI_BRIDGE_ENABLED
extern void wifiBridgeRecoverApOnly();
extern void wifiBridgePrintDiag();
#endif

namespace client_config {

static nvs_handle_t storage;
static httpd_handle_t server;

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

static esp_err_t sendJson(httpd_req_t* request, const char* status, const char* body) {
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_sendstr(request, body);
}

static esp_err_t sendState(httpd_req_t* request, const ClientConfigState& state) {
    char body[kClientConfigCapacity + 96];
    snprintf(body, sizeof(body), "{\"revision\":%lu,\"applied\":%lu,\"canRollback\":%s,\"config\":%s}",
        static_cast<unsigned long>(state.revision), static_cast<unsigned long>(state.applied),
        state.previous[0] ? "true" : "false", state.current);
    return sendJson(request, "200 OK", body);
}

static bool matchesToken(const char* supplied, const char* expected) {
    volatile uint8_t difference = 0;
    for (size_t index = 0; index < 64; ++index) {
        difference |= static_cast<uint8_t>(supplied[index] ^ expected[index]);
    }
    return difference == 0;
}

enum class Access { Reader, Admin, Client };

static bool authorized(httpd_req_t* request, Access access) {
    constexpr char prefix[] = "Bearer ";
    char header[sizeof(prefix) + 64];
    if (httpd_req_get_hdr_value_str(request, "Authorization", header, sizeof(header)) != ESP_OK ||
        strlen(header) != sizeof(header) - 1 || strncmp(header, prefix, sizeof(prefix) - 1) != 0) {
        return false;
    }
    const char* token = header + sizeof(prefix) - 1;
    const bool admin = matchesToken(token, identity.adminToken);
    const bool client = matchesToken(token, identity.clientToken);
    if (access == Access::Admin) return admin;
    if (access == Access::Client) return client;
    return admin || client;
}

static esp_err_t rejectUnauthorized(httpd_req_t* request) {
    httpd_resp_set_hdr(request, "WWW-Authenticate", "Bearer");
    return sendJson(request, "401 Unauthorized", "{\"error\":\"unauthorized\"}");
}

static esp_err_t dashboard(httpd_req_t* request) {
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "Content-Security-Policy",
        "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; "
        "connect-src 'self'; base-uri 'none'; frame-ancestors 'none'; form-action 'none'");
    return httpd_resp_send(request, CLIENT_CONFIG_HTML, sizeof(CLIENT_CONFIG_HTML) - 1);
}

static esp_err_t fetchConfig(httpd_req_t* request) {
    if (!authorized(request, Access::Reader)) return rejectUnauthorized(request);
    ClientConfigState state;
    if (!readState(state)) return sendJson(request, "500 Internal Server Error", "{\"error\":\"storage\"}");
    return sendState(request, state);
}

enum class Change { Publish, Rollback, Acknowledge };

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

static esp_err_t changeConfig(httpd_req_t* request, Change change) {
    const Access access = change == Change::Acknowledge ? Access::Client : Access::Admin;
    if (!authorized(request, access)) {
        rejectUnauthorized(request);
        return ESP_FAIL;
    }

    char input[kClientConfigCapacity + 64];
    if (request->content_len >= sizeof(input)) {
        sendJson(request, "413 Payload Too Large", "{\"error\":\"config_too_large\"}");
        return ESP_FAIL;
    }
    size_t received = 0;
    while (received < request->content_len) {
        const int count = httpd_req_recv(request, input + received, request->content_len - received);
        if (count <= 0) return ESP_FAIL;
        received += static_cast<size_t>(count);
    }
    input[received] = '\0';
    if (memchr(input, '\0', received) || !safeJsonDepth(input)) {
        return sendJson(request, "400 Bad Request", "{\"error\":\"invalid_json\"}");
    }
    const std::unique_ptr<cJSON, decltype(&cJSON_Delete)> body(
        cJSON_ParseWithLengthOpts(input, received + 1, nullptr, true), cJSON_Delete);
    const cJSON* revision = cJSON_GetObjectItemCaseSensitive(body.get(), "revision");
    if (!cJSON_IsObject(body.get()) || !cJSON_IsNumber(revision) ||
        !(revision->valuedouble >= 1 && revision->valuedouble <= UINT32_MAX) ||
        revision->valuedouble != static_cast<uint32_t>(revision->valuedouble)) {
        return sendJson(request, "400 Bad Request", "{\"error\":\"invalid_revision\"}");
    }

    ClientConfigState state;
    if (!readState(state)) return sendJson(request, "500 Internal Server Error", "{\"error\":\"storage\"}");
    if (state.revision != static_cast<uint32_t>(revision->valuedouble)) {
        return sendJson(request, "409 Conflict", "{\"error\":\"revision_conflict\"}");
    }
    const uint32_t originalRevision = state.revision;
    const uint32_t originalApplied = state.applied;
    switch (change) {
        case Change::Publish: {
            cJSON* config = cJSON_GetObjectItemCaseSensitive(body.get(), "config");
            if (!cJSON_IsObject(config)) {
                return sendJson(request, "400 Bad Request", "{\"error\":\"expected_object\"}");
            }
            // cJSON_PrintPreallocated needs a few bytes of extra working space.
            char json[kClientConfigCapacity + 5];
            if (!cJSON_PrintPreallocated(config, json, sizeof(json), false) ||
                strlen(json) >= kClientConfigCapacity) {
                return sendJson(request, "413 Payload Too Large", "{\"error\":\"config_too_large\"}");
            }
            if (!state.publish(json)) {
                return sendJson(request, "409 Conflict", "{\"error\":\"revision_exhausted\"}");
            }
            break;
        }
        case Change::Rollback:
            if (!state.rollback()) {
                return sendJson(request, "409 Conflict", "{\"error\":\"rollback_unavailable\"}");
            }
            break;
        case Change::Acknowledge:
            state.acknowledge(static_cast<uint32_t>(revision->valuedouble));
            break;
    }
    if ((state.revision != originalRevision || state.applied != originalApplied) && !writeState(state)) {
        return sendJson(request, "500 Internal Server Error", "{\"error\":\"storage\"}");
    }
    return sendState(request, state);
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

// esp_https_server calls this only after a successful TLS handshake, with the
// negotiated TLS context. It lets the dashboard show the same TLS state that
// Windows mitmproxy prints in handshake.log.
static void tlsUserCallback(esp_https_server_user_cb_arg_t* argument) {
    if (argument && argument->tls) fsd_mock::noteHandshake(argument->tls);
}

bool start() {
    if (nvs_open("client-config", NVS_READWRITE, &storage) != ESP_OK) return false;
    if (!loadIdentity(storage)) {
        nvs_close(storage);
        return false;
    }

    // Same cert chain as TeslaDG/app/certs2: leaf signed by the mitmproxy CA.
    // Feature_configuration and the self-service client config share port 443.
    fsd_mock::installLogHook();
    httpd_ssl_config_t config = HTTPD_SSL_CONFIG_DEFAULT();
    config.port_secure = kHttpsPort;
    config.user_cb = tlsUserCallback;
    config.cacert_pem = reinterpret_cast<const uint8_t*>(kFsdTlsCertificate);
    config.cacert_len = strlen(kFsdTlsCertificate) + 1;
    config.prvtkey_pem = reinterpret_cast<const uint8_t*>(kFsdTlsPrivateKey);
    config.prvtkey_len = strlen(kFsdTlsPrivateKey) + 1;
    config.httpd.uri_match_fn = matchUri;
    config.httpd.max_uri_handlers = 16;
    config.httpd.core_id = 0;
    config.httpd.stack_size = 12288;
    config.httpd.max_open_sockets = 2;
    config.httpd.recv_wait_timeout = 3;
    config.httpd.send_wait_timeout = 3;

    const httpd_uri_t endpoints[] = {
        {"/", HTTP_GET, dashboard, nullptr},
        {"/api/client-config", HTTP_GET, fetchConfig, nullptr},
        {"/api/client-config", HTTP_PUT,
            [](httpd_req_t* request) { return changeConfig(request, Change::Publish); }, nullptr},
        {"/api/client-config/rollback", HTTP_POST,
            [](httpd_req_t* request) { return changeConfig(request, Change::Rollback); }, nullptr},
        {"/api/client-config/ack", HTTP_POST,
            [](httpd_req_t* request) { return changeConfig(request, Change::Acknowledge); }, nullptr},
        // Must stay last: Windows fsd_mock matches /feature_configuration anywhere.
        // IDF 4.4 has no HTTP_ANY, so register the catch-all once per method.
        {"/*", HTTP_GET, catchAll, nullptr},
        {"/*", HTTP_POST, catchAll, nullptr},
        {"/*", HTTP_PUT, catchAll, nullptr},
        {"/*", HTTP_DELETE, catchAll, nullptr},
        {"/*", HTTP_HEAD, catchAll, nullptr},
        {"/*", HTTP_OPTIONS, catchAll, nullptr},
        {"/*", HTTP_PATCH, catchAll, nullptr},
    };
    if (httpd_ssl_start(&server, &config) == ESP_OK) {
        for (const auto& endpoint : endpoints) {
            if (httpd_register_uri_handler(server, &endpoint) == ESP_OK) continue;
            httpd_ssl_stop(server);
            server = nullptr;
            break;
        }
        if (server) {
            // Only redirect api-prd after 443 is actually accepting connections.
            fsd_mock::begin();
            return true;
        }
    }
    nvs_close(storage);
    return false;
}

void serviceSerial() {
    if (!server) return;
    static char command[24];
    static size_t length = 0;
    for (int budget = 48; budget > 0 && Serial.available(); --budget) {
        const int value = Serial.read();
        if (value == '\r') continue;
        if (value == '\n') {
            if (length < sizeof(command)) {
                command[length] = '\0';
                if (strcmp(command, "client-config") == 0) {
                    // Export the CA that signs the port-443 leaf. This is the
                    // same trust anchor as TeslaDG/app/certs2/mitmproxy-ca-cert.pem.
                    Serial.print(kFsdTlsCaCertificate);
                    Serial.printf("\nadmin_token=%s\nclient_token=%s\n", identity.adminToken, identity.clientToken);
                } else if (strcmp(command, "wifi-ap-only") == 0) {
                    wifiBridgeRecoverApOnly();
                } else if (strcmp(command, "wifi-diag") == 0) {
                    wifiBridgePrintDiag();
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
