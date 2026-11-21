#include "mod_client_config.h"

#include <sdkconfig.h>

#if defined(WIFI_BRIDGE_ENABLED) && defined(CONFIG_IDF_TARGET_ESP32S3) && defined(CONFIG_ESP_HTTPS_SERVER_ENABLE)

#include <Arduino.h>
#include <WiFi.h>
#include <cJSON.h>
#include <esp_https_server.h>
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
#endif

namespace client_config {

static nvs_handle_t storage;
static httpd_handle_t fsdServer;

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

    fsd_mock::installLogHook();

    // 443 is FSD-only: the catch-all handles /feature_configuration and
    // reverse-proxies every other vehicle path (Windows mitmdump behavior).
    // Client-config API lives on the 10010 management server, so no second
    // HTTPS server/TLS socket is allocated here.
    httpd_ssl_config_t config = HTTPD_SSL_CONFIG_DEFAULT();
    config.port_secure = kFsdPort;
    config.cacert_pem = reinterpret_cast<const uint8_t*>(kFsdTlsCertificate);
    config.cacert_len = strlen(kFsdTlsCertificate) + 1;
    config.prvtkey_pem = reinterpret_cast<const uint8_t*>(kFsdTlsPrivateKey);
    config.prvtkey_len = strlen(kFsdTlsPrivateKey) + 1;
    config.user_cb = tlsUserCallback;
    config.httpd.uri_match_fn = matchUri;
    config.httpd.max_uri_handlers = 8;
    config.httpd.core_id = 0;
    config.httpd.stack_size = 10240;
    // One vehicle TLS socket at a time. A second concurrent car connection
    // plus the upstream esp_http_client TLS context can exhaust the ~80 KB
    // heap; lru_purge_enable closes the stale socket when the car reconnects.
    config.httpd.max_open_sockets = 1;
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
    if (httpd_ssl_start(&fsdServer, &config) != ESP_OK) {
        fsdServer = nullptr;
        nvs_close(storage);
        return false;
    }
    for (const auto& endpoint : fsdEndpoints) {
        if (httpd_register_uri_handler(fsdServer, &endpoint) == ESP_OK) continue;
        httpd_ssl_stop(fsdServer);
        fsdServer = nullptr;
        nvs_close(storage);
        return false;
    }

    // Only redirect api-prd after 443 is actually accepting connections.
    fsd_mock::begin();
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
                } else if (strcmp(command, "fsd-default") == 0 ||
                           strcmp(command, "fsd-16f") == 0 ||
                           strcmp(command, "fsd-282b") == 0) {
                    const char* preset = strcmp(command, "fsd-default") == 0 ? "default" :
                        (strcmp(command, "fsd-16f") == 0 ? "16f" : "282b");
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
