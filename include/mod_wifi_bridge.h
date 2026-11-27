#pragma once
// ── WiFi bridge module ────────────────────────────────────────────────────────
// Manages up to 10 saved upstream networks, auto-reconnect, RSSI-ordered
// selection, NAPT (Network Address Port Translation) between AP clients and
// STA upstream, and one-shot scan results for the web UI.
//
// Requires mod_dns.h (for dnsIpPolicyService) and mod_thermal.h (for throttle
// level — slows retry when the chip overheats) to be included before this.
//
// Ported from pudge9527/tesla-fsd-wifi-controller main.cpp.

#ifdef WIFI_BRIDGE_ENABLED

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <cstring>

extern "C" {
#include <esp_idf_version.h>
#if ESP_IDF_VERSION_MAJOR >= 5
#include "esp_netif.h"
#else
#include "lwip/lwip_napt.h"
#endif
#include "esp_wifi.h"
}

#include "mod_dns.h"
#include "mod_thermal.h"

// Defined in main.cpp: restart the AP on channel 1 when AP-only mode is active.
extern void wifiBridgeForceApChannel1();

static constexpr uint32_t UPSTREAM_RETRY_THROTTLED_MS = 60000;
static constexpr uint32_t UPSTREAM_CONNECT_TIMEOUT_MS = 12000;
static constexpr uint32_t UPSTREAM_FAST_RETRY_DELAY_MS = 2000;
static constexpr uint8_t  MAX_UPSTREAM_NETWORKS = 10;

// Exponential-backoff schedule for repeated upstream connect failures.
// Indexed by consecutiveFailures count; clamped to last entry.
// Note: WiFi.setAutoReconnect(false) MUST be set in bridge builds so Arduino's
// STA_DISCONNECTED handler does not bypass this schedule with tight retry loops.
// Reset to 0 on successful connect or when the user re-saves the network list.
static constexpr uint32_t kUpstreamBackoffMs[] = {
    3000, 8000, 15000, 30000, 60000, 120000, 300000
};
static constexpr uint8_t kUpstreamBackoffSteps =
    sizeof(kUpstreamBackoffMs) / sizeof(kUpstreamBackoffMs[0]);

struct SavedUpstreamNetwork {
    char    ssid[33]    = {};
    char    pass[65]    = {};
    uint8_t lastChannel = 0;  // RAM-only last connected channel hint (1..13, 0=any)
};

struct UpstreamWiFiConfig {
    bool                 enabled                = false;
    SavedUpstreamNetwork networks[MAX_UPSTREAM_NETWORKS];
    uint8_t              networkCount           = 0;
    int8_t               activeIndex            = -1;
    uint8_t              nextTryIndex           = 0;
    volatile bool        applyRequested         = false;
    uint32_t             lastAttemptMillis      = 0;
    uint32_t             connectStartMillis     = 0;
    uint32_t             nextRetryMillis        = 0;
    uint32_t             lastConnectedSyncMillis = 0;
    uint8_t              consecutiveFailures    = 0;
    int8_t               preferredIndex         = -1;  // last network that connected; tried first after apply/boot
    bool                 connecting             = false;
    bool                 wasConnected           = false;
    bool                 fastRetryPending       = false;
    bool                 usedChannelHint        = false;
    volatile bool        pendingDisconnectEvent = false;
    volatile bool        pendingGotIpEvent      = false;
    volatile uint8_t     lastDisconnectReason   = 0;
    volatile uint32_t    connectSuccessCount    = 0;
    volatile uint32_t    disconnectCount        = 0;
    volatile uint32_t    apClientJoinCount      = 0;
    volatile uint32_t    apClientLeaveCount     = 0;
};

static UpstreamWiFiConfig gWifiBridgeCfg;
static bool               gNatEnabled = false;
static DNSWhitelistServer gDnsFilter;
static DNSFilterConfig    gDnsFilterCfg;

// Guards gWifiBridgeCfg.networks[], networkCount, activeIndex, nextTryIndex.
// HTTP handler task (add/remove/status JSON enumeration) races the Core-1 loop
// task (serviceUpstreamWiFi → WiFi.begin). Short critical sections only —
// WiFi.begin() must be called OUTSIDE the mux (blocking), so copy ssid/pass
// to locals inside the mux, release, then call WiFi.begin.
static portMUX_TYPE gWifiBridgeMux = portMUX_INITIALIZER_UNLOCKED;

// ── Internal helpers ──────────────────────────────────────────────────────────
static inline void wifiBridgeCopyStr(char* dest, size_t size, const String& value) {
    memset(dest, 0, size);
    value.toCharArray(dest, size);
}

static inline String wifiBridgeJsonEscape(const String& value) {
    String escaped;
    escaped.reserve(value.length() + 8);
    for (size_t i = 0; i < value.length(); ++i) {
        char c = value[i];
        switch (c) {
            case '\\': escaped += "\\\\"; break;
            case '"':  escaped += "\\\""; break;
            case '\n': escaped += "\\n"; break;
            case '\r': break;
            case '\t': escaped += "\\t"; break;
            default:
                escaped += (static_cast<unsigned char>(c) < 0x20) ? ' ' : c;
                break;
        }
    }
    return escaped;
}

static inline bool wifiBridgeIsReservedSSID(const String& ssid, const char* apSsid) {
    return apSsid != nullptr && ssid.equals(apSsid);
}

static inline void wifiBridgeClearNetwork(SavedUpstreamNetwork& n) {
    n = SavedUpstreamNetwork{};
}

static inline void wifiBridgeClearAllNetworks() {
    for (uint8_t i = 0; i < MAX_UPSTREAM_NETWORKS; ++i) wifiBridgeClearNetwork(gWifiBridgeCfg.networks[i]);
    gWifiBridgeCfg.networkCount = 0;
    gWifiBridgeCfg.activeIndex = -1;
    gWifiBridgeCfg.nextTryIndex = 0;
}

static inline int wifiBridgeFindNetwork(const String& ssid) {
    for (uint8_t i = 0; i < gWifiBridgeCfg.networkCount; ++i) {
        if (ssid.equals(gWifiBridgeCfg.networks[i].ssid)) return i;
    }
    return -1;
}

static inline bool wifiBridgeAddOrUpdateNetwork(const String& ssidInput, const String& passInput, bool overwritePass, const char* apSsid) {
    String ssid = ssidInput;
    ssid.trim();

    if (ssid.isEmpty() || ssid.length() > 32 || passInput.length() > 63 ||
        wifiBridgeIsReservedSSID(ssid, apSsid)) {
        return false;
    }

    bool ok = true;
    portENTER_CRITICAL(&gWifiBridgeMux);
    int index = wifiBridgeFindNetwork(ssid);
    bool isNew = index < 0;

    if (isNew) {
        if (gWifiBridgeCfg.networkCount >= MAX_UPSTREAM_NETWORKS) {
            ok = false;
        } else {
            index = gWifiBridgeCfg.networkCount++;
            wifiBridgeClearNetwork(gWifiBridgeCfg.networks[index]);
            wifiBridgeCopyStr(gWifiBridgeCfg.networks[index].ssid, sizeof(gWifiBridgeCfg.networks[index].ssid), ssid);
        }
    }

    if (ok && (isNew || overwritePass)) {
        wifiBridgeCopyStr(gWifiBridgeCfg.networks[index].pass, sizeof(gWifiBridgeCfg.networks[index].pass), passInput);
    }
    portEXIT_CRITICAL(&gWifiBridgeMux);
    return ok;
}

static inline bool wifiBridgeRemoveNetwork(const String& ssidInput) {
    String ssid = ssidInput;
    ssid.trim();

    portENTER_CRITICAL(&gWifiBridgeMux);
    int index = wifiBridgeFindNetwork(ssid);
    if (index < 0) {
        portEXIT_CRITICAL(&gWifiBridgeMux);
        return false;
    }

    for (uint8_t i = index; i + 1 < gWifiBridgeCfg.networkCount; ++i) {
        gWifiBridgeCfg.networks[i] = gWifiBridgeCfg.networks[i + 1];
    }
    if (gWifiBridgeCfg.networkCount > 0) {
        --gWifiBridgeCfg.networkCount;
        wifiBridgeClearNetwork(gWifiBridgeCfg.networks[gWifiBridgeCfg.networkCount]);
    }

    bool prefChanged = false;
    int8_t newPref = -1;
    if (gWifiBridgeCfg.networkCount == 0) {
        gWifiBridgeCfg.activeIndex = -1;
        gWifiBridgeCfg.nextTryIndex = 0;
        if (gWifiBridgeCfg.preferredIndex != -1) {
            gWifiBridgeCfg.preferredIndex = -1;
            prefChanged = true;
            newPref = -1;
        }
    } else {
        if (gWifiBridgeCfg.activeIndex == index) {
            gWifiBridgeCfg.activeIndex = -1;
        } else if (gWifiBridgeCfg.activeIndex > index) {
            --gWifiBridgeCfg.activeIndex;
        }
        if (gWifiBridgeCfg.preferredIndex == index) {
            gWifiBridgeCfg.preferredIndex = -1;
            prefChanged = true;
            newPref = -1;
        } else if (gWifiBridgeCfg.preferredIndex > index) {
            --gWifiBridgeCfg.preferredIndex;
            prefChanged = true;
            newPref = gWifiBridgeCfg.preferredIndex;
        }
        if (gWifiBridgeCfg.nextTryIndex >= gWifiBridgeCfg.networkCount) {
            gWifiBridgeCfg.nextTryIndex = 0;
        }
    }
    portEXIT_CRITICAL(&gWifiBridgeMux);
    if (prefChanged) {
        Preferences p;
        if (p.begin("fsd", false)) {
            p.putChar("ub_pref", newPref);
            p.end();
        }
    }
    return true;
}

static inline bool wifiBridgeHasCredentials() { return gWifiBridgeCfg.networkCount > 0; }

static inline String wifiBridgeConnectedSSID() {
    return WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String();
}

static inline String wifiBridgeActiveSSID() {
    if (WiFi.status() == WL_CONNECTED) return WiFi.SSID();
    if (gWifiBridgeCfg.activeIndex >= 0 && gWifiBridgeCfg.activeIndex < gWifiBridgeCfg.networkCount) {
        return String(gWifiBridgeCfg.networks[gWifiBridgeCfg.activeIndex].ssid);
    }
    return String();
}

static inline bool wifiBridgeIsTransientDisconnect(uint8_t reason) {
    switch (reason) {
        case WIFI_REASON_BEACON_TIMEOUT:
        case WIFI_REASON_AP_TSF_RESET:
        case WIFI_REASON_CONNECTION_FAIL:
#if defined(ESP_IDF_VERSION_MAJOR) && ESP_IDF_VERSION_MAJOR >= 6
        case WIFI_REASON_DISASSOC_DUE_TO_INACTIVITY:
#else
        case WIFI_REASON_ASSOC_EXPIRE:
#endif
        case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT:
        case WIFI_REASON_ROAMING:
            return true;
        default:
            return false;
    }
}

static inline const char* wifiBridgeDisconnectReasonText(uint8_t reason) {
    switch (reason) {
        case 0:                                     return "none";
#if defined(ESP_IDF_VERSION_MAJOR) && ESP_IDF_VERSION_MAJOR >= 6
        case WIFI_REASON_DISASSOC_DUE_TO_INACTIVITY:return "assoc_expire";
#else
        case WIFI_REASON_ASSOC_EXPIRE:              return "assoc_expire";
#endif
        case WIFI_REASON_ASSOC_LEAVE:               return "assoc_leave";
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:    return "4way_timeout";
        case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT:  return "gtk_timeout";
        case WIFI_REASON_BEACON_TIMEOUT:            return "beacon_timeout";
        case WIFI_REASON_NO_AP_FOUND:               return "no_ap_found";
        case WIFI_REASON_AUTH_FAIL:                 return "auth_fail";
        case WIFI_REASON_ASSOC_FAIL:                return "assoc_fail";
        case WIFI_REASON_HANDSHAKE_TIMEOUT:         return "handshake_timeout";
        case WIFI_REASON_CONNECTION_FAIL:           return "connection_fail";
        case WIFI_REASON_AP_TSF_RESET:              return "ap_tsf_reset";
        case WIFI_REASON_ROAMING:                   return "roaming";
        default:                                    return "other";
    }
}

static inline uint32_t wifiBridgeComputeBackoffMs(uint8_t consecutiveFailures,
                                                  bool hasApStations,
                                                  bool thermalThrottled) {
    uint8_t step = consecutiveFailures;
    if (step >= kUpstreamBackoffSteps) step = kUpstreamBackoffSteps - 1;
    uint32_t delayMs = kUpstreamBackoffMs[step];
    if (thermalThrottled && delayMs < UPSTREAM_RETRY_THROTTLED_MS) {
        delayMs = UPSTREAM_RETRY_THROTTLED_MS;
    }
    // In APSTA mode every STA scan/begin can briefly disturb the AP. Once at
    // least one upstream attempt has failed while a phone/car is connected to
    // the AP, back off to >=60 s so the AP stays rock-solid.
    if (hasApStations && consecutiveFailures > 0 && delayMs < UPSTREAM_RETRY_THROTTLED_MS) {
        delayMs = UPSTREAM_RETRY_THROTTLED_MS;
    }
    return delayMs;
}

static inline void wifiBridgeOnEvent(arduino_event_t* ev) {
    if (!ev) return;
    switch (ev->event_id) {
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            gWifiBridgeCfg.pendingGotIpEvent = true;
            gWifiBridgeCfg.pendingDisconnectEvent = false;
            gWifiBridgeCfg.lastDisconnectReason = 0;
            gWifiBridgeCfg.connectSuccessCount = gWifiBridgeCfg.connectSuccessCount + 1;
            break;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
            const uint8_t reason = ev->event_info.wifi_sta_disconnected.reason;
            // Voluntary disconnect (our own WiFi.disconnect()) emits ASSOC_LEAVE.
            // Ignore it while starting a fresh connection so we don't abort ourselves.
            if (reason == WIFI_REASON_ASSOC_LEAVE && gWifiBridgeCfg.connecting) {
                break;
            }
            gWifiBridgeCfg.lastDisconnectReason = reason;
            gWifiBridgeCfg.disconnectCount = gWifiBridgeCfg.disconnectCount + 1;
            gWifiBridgeCfg.pendingDisconnectEvent = true;
            break;
        }
        case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
            gWifiBridgeCfg.apClientJoinCount = gWifiBridgeCfg.apClientJoinCount + 1;
            break;
        case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
            gWifiBridgeCfg.apClientLeaveCount = gWifiBridgeCfg.apClientLeaveCount + 1;
            break;
        default:
            break;
    }
}

static inline void wifiBridgeRegisterEvents() {
    static bool registered = false;
    if (registered) return;
    registered = true;
    WiFi.onEvent(wifiBridgeOnEvent);
}

static inline int wifiBridgeSelectNextNetworkIndex(const char* /*apSsid*/) {
    if (!wifiBridgeHasCredentials()) return -1;

    // Do NOT scan on every reconnect retry: a full-channel scan in APSTA mode
    // briefly drops the AP's beacon, kicking the phone off Wi-Fi.
    // Round-robin through saved networks instead.
    if (gWifiBridgeCfg.nextTryIndex >= gWifiBridgeCfg.networkCount) {
        gWifiBridgeCfg.nextTryIndex = 0;
    }
    int selectedIndex = gWifiBridgeCfg.nextTryIndex;
    gWifiBridgeCfg.nextTryIndex = (gWifiBridgeCfg.nextTryIndex + 1) % gWifiBridgeCfg.networkCount;
    return selectedIndex;
}

static inline void wifiBridgeStartConnect(const char* apSsid, bool useChannelHint = true) {
    if (!gWifiBridgeCfg.enabled || !wifiBridgeHasCredentials()) return;

    // Copy ssid/pass/channel under mux; WiFi.begin blocks and must not hold a portMUX.
    char ssid[33] = {};
    char pass[65] = {};
    uint8_t channelHint = 0;
    bool have = false;

    portENTER_CRITICAL(&gWifiBridgeMux);
    int idx = wifiBridgeSelectNextNetworkIndex(apSsid);
    if (idx >= 0 && idx < gWifiBridgeCfg.networkCount) {
        gWifiBridgeCfg.activeIndex = idx;
        gWifiBridgeCfg.nextTryIndex = (idx + 1) % gWifiBridgeCfg.networkCount;
        memcpy(ssid, gWifiBridgeCfg.networks[idx].ssid, sizeof(ssid));
        memcpy(pass, gWifiBridgeCfg.networks[idx].pass, sizeof(pass));
        if (useChannelHint) channelHint = gWifiBridgeCfg.networks[idx].lastChannel;
        have = true;
    } else {
        gWifiBridgeCfg.activeIndex = -1;
    }
    portEXIT_CRITICAL(&gWifiBridgeMux);

    const uint32_t now = millis();
    if (!have) {
        gWifiBridgeCfg.connecting = false;
        gWifiBridgeCfg.lastAttemptMillis = now;
        return;
    }

    gWifiBridgeCfg.connecting = true;
    gWifiBridgeCfg.usedChannelHint = (channelHint >= 1 && channelHint <= 13);
    gWifiBridgeCfg.pendingDisconnectEvent = false;
    gWifiBridgeCfg.pendingGotIpEvent = false;
    gWifiBridgeCfg.connectStartMillis = now;
    gWifiBridgeCfg.lastAttemptMillis = now;

    if (WiFi.status() == WL_CONNECTED) {
        WiFi.disconnect(false, false);
    }
    const int32_t ch = gWifiBridgeCfg.usedChannelHint ? static_cast<int32_t>(channelHint) : 0;
    WiFi.begin(static_cast<const char*>(ssid),
               pass[0] != '\0' ? static_cast<const char*>(pass) : static_cast<const char*>(nullptr),
               ch);
}

static inline void wifiBridgeRequestApply() { gWifiBridgeCfg.applyRequested = true; }

static inline void wifiBridgeApplyConfig(const char* apSsid) {
    // Apply always resets the backoff counter — user-driven config change is
    // an explicit signal to retry promptly even if the previous attempts were
    // in long-backoff territory.
    gWifiBridgeCfg.consecutiveFailures = 0;
    gWifiBridgeCfg.connecting = false;
    gWifiBridgeCfg.wasConnected = false;
    gWifiBridgeCfg.fastRetryPending = false;
    gWifiBridgeCfg.usedChannelHint = false;
    gWifiBridgeCfg.pendingDisconnectEvent = false;
    gWifiBridgeCfg.pendingGotIpEvent = false;
    gWifiBridgeCfg.lastDisconnectReason = 0;
    gWifiBridgeCfg.nextRetryMillis = 0;
    gWifiBridgeCfg.lastConnectedSyncMillis = 0;

    if (thermalProtectActive() || !gWifiBridgeCfg.enabled || !wifiBridgeHasCredentials()) {
        WiFi.disconnect(false, true);
        gDnsFilter.clearCache();
        gWifiBridgeCfg.activeIndex = -1;
        gWifiBridgeCfg.nextTryIndex = 0;
        gWifiBridgeCfg.lastAttemptMillis = 0;
        wifiBridgeForceApChannel1();
        return;
    }

    WiFi.disconnect(false, true);
    gDnsFilter.clearCache();
    gWifiBridgeCfg.activeIndex = -1;
    // Start with the last-known-good network if it's still in the saved list.
    // Falls back to round-robin from index 0 if no preference exists.
    if (gWifiBridgeCfg.preferredIndex >= 0
     && gWifiBridgeCfg.preferredIndex < gWifiBridgeCfg.networkCount) {
        gWifiBridgeCfg.nextTryIndex = (uint8_t)gWifiBridgeCfg.preferredIndex;
    } else {
        gWifiBridgeCfg.nextTryIndex = 0;
    }
    wifiBridgeStartConnect(apSsid, /*useChannelHint=*/false);
}

static inline void serviceUpstreamWiFi(const char* apSsid) {
    if (gWifiBridgeCfg.applyRequested) {
        gWifiBridgeCfg.applyRequested = false;
        wifiBridgeApplyConfig(apSsid);
        return;
    }

    if (!gWifiBridgeCfg.enabled || !wifiBridgeHasCredentials()) return;

    if (thermalProtectActive()) {
        if (WiFi.status() == WL_CONNECTED || gWifiBridgeCfg.connecting) {
            WiFi.disconnect(false, true);
            gDnsFilter.clearCache();
            gWifiBridgeCfg.activeIndex = -1;
            gWifiBridgeCfg.connecting = false;
            gWifiBridgeCfg.wasConnected = false;
            gWifiBridgeCfg.fastRetryPending = false;
            gWifiBridgeCfg.lastAttemptMillis = 0;
            gWifiBridgeCfg.nextRetryMillis = 0;
        }
        return;
    }

    // Never collide with an active async Wi-Fi scan triggered by /api/scan.
    if (WiFi.scanComplete() == WIFI_SCAN_RUNNING) {
        return;
    }

    const uint32_t now = millis();

    // 1. Connected path: record channel hint, reset backoff, persist preferred SSID.
    //    Throttle WiFi.SSID() / esp_wifi_get_channel() IPC + heap String to
    //    connect transitions or every 5 s instead of running at 100 Hz in loop().
    if (WiFi.status() == WL_CONNECTED) {
        const bool needSync = !gWifiBridgeCfg.wasConnected ||
                              gWifiBridgeCfg.pendingGotIpEvent ||
                              (now - gWifiBridgeCfg.lastConnectedSyncMillis >= 5000u);
        gWifiBridgeCfg.connecting = false;
        gWifiBridgeCfg.wasConnected = true;
        gWifiBridgeCfg.fastRetryPending = false;
        gWifiBridgeCfg.pendingDisconnectEvent = false;
        gWifiBridgeCfg.pendingGotIpEvent = false;
        gWifiBridgeCfg.consecutiveFailures = 0;
        if (!needSync) return;
        gWifiBridgeCfg.lastConnectedSyncMillis = now;

        String currentSsid = WiFi.SSID();
        uint8_t currentCh = 0;
        wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
        if (esp_wifi_get_channel(&currentCh, &second) != ESP_OK) currentCh = 0;

        int idx = -1;
        bool prefChanged = false;
        portENTER_CRITICAL(&gWifiBridgeMux);
        idx = wifiBridgeFindNetwork(currentSsid);
        if (idx >= 0) {
            gWifiBridgeCfg.activeIndex = idx;
            if (currentCh >= 1 && currentCh <= 13) {
                gWifiBridgeCfg.networks[idx].lastChannel = currentCh;
            }
            if (gWifiBridgeCfg.preferredIndex != idx) {
                gWifiBridgeCfg.preferredIndex = idx;
                prefChanged = true;
            }
        }
        portEXIT_CRITICAL(&gWifiBridgeMux);

        if (prefChanged && idx >= 0) {
            Preferences p;
            if (p.begin("fsd", false)) {
                p.putChar("ub_pref", (int8_t)idx);
                p.end();
            }
        }
        return;
    }

    // 2. Process asynchronous STA_DISCONNECTED event.
    if (gWifiBridgeCfg.pendingDisconnectEvent) {
        gWifiBridgeCfg.pendingDisconnectEvent = false;
        const uint8_t reason = gWifiBridgeCfg.lastDisconnectReason;
        const bool droppedFromConnected = gWifiBridgeCfg.wasConnected;
        gWifiBridgeCfg.wasConnected = false;
        gWifiBridgeCfg.connecting = false;
        gDnsFilter.clearCache();

        if (droppedFromConnected && wifiBridgeIsTransientDisconnect(reason) &&
            gWifiBridgeCfg.activeIndex >= 0 &&
            gWifiBridgeCfg.activeIndex < gWifiBridgeCfg.networkCount &&
            !gWifiBridgeCfg.fastRetryPending) {
            // Brief link loss on a healthy upstream: retry the SAME SSID once
            // after 2 s using its cached channel hint (~150 ms re-assoc).
            gWifiBridgeCfg.fastRetryPending = true;
            portENTER_CRITICAL(&gWifiBridgeMux);
            if (gWifiBridgeCfg.activeIndex >= 0 && gWifiBridgeCfg.activeIndex < gWifiBridgeCfg.networkCount) {
                gWifiBridgeCfg.nextTryIndex = static_cast<uint8_t>(gWifiBridgeCfg.activeIndex);
            }
            portEXIT_CRITICAL(&gWifiBridgeMux);
            gWifiBridgeCfg.nextRetryMillis = now + UPSTREAM_FAST_RETRY_DELAY_MS;
            return;
        }

        // Failed attempt or non-transient disconnect: clear stale channel hint
        // so the next try scans all channels (hotspot may have changed channel).
        portENTER_CRITICAL(&gWifiBridgeMux);
        if (gWifiBridgeCfg.activeIndex >= 0 && gWifiBridgeCfg.activeIndex < gWifiBridgeCfg.networkCount) {
            gWifiBridgeCfg.networks[gWifiBridgeCfg.activeIndex].lastChannel = 0;
        }
        portEXIT_CRITICAL(&gWifiBridgeMux);

        gWifiBridgeCfg.fastRetryPending = false;
        if (gWifiBridgeCfg.consecutiveFailures < kUpstreamBackoffSteps) {
            gWifiBridgeCfg.consecutiveFailures++;
        }
        const uint32_t delayMs = wifiBridgeComputeBackoffMs(
            gWifiBridgeCfg.consecutiveFailures - 1,
            WiFi.softAPgetStationNum() > 0,
            thermalThrottleActive());
        gWifiBridgeCfg.nextRetryMillis = now + delayMs;
        return;
    }

    // 3. While an attempt is actively in flight (scan / WPA handshake / DHCP),
    //    wait up to UPSTREAM_CONNECT_TIMEOUT_MS (12 s) instead of aborting at 3 s.
    if (gWifiBridgeCfg.connecting) {
        if (now - gWifiBridgeCfg.connectStartMillis < UPSTREAM_CONNECT_TIMEOUT_MS) {
            return;
        }
        // Timed out waiting for association or DHCP.
        WiFi.disconnect(false, false);
        gWifiBridgeCfg.connecting = false;
        gWifiBridgeCfg.wasConnected = false;
        gWifiBridgeCfg.fastRetryPending = false;
        portENTER_CRITICAL(&gWifiBridgeMux);
        if (gWifiBridgeCfg.activeIndex >= 0 && gWifiBridgeCfg.activeIndex < gWifiBridgeCfg.networkCount) {
            gWifiBridgeCfg.networks[gWifiBridgeCfg.activeIndex].lastChannel = 0;
        }
        portEXIT_CRITICAL(&gWifiBridgeMux);
        if (gWifiBridgeCfg.consecutiveFailures < kUpstreamBackoffSteps) {
            gWifiBridgeCfg.consecutiveFailures++;
        }
        const uint32_t delayMs = wifiBridgeComputeBackoffMs(
            gWifiBridgeCfg.consecutiveFailures - 1,
            WiFi.softAPgetStationNum() > 0,
            thermalThrottleActive());
        gWifiBridgeCfg.nextRetryMillis = now + delayMs;
        return;
    }

    // 4. Handle unexpected silent drop without event (safety net).
    if (gWifiBridgeCfg.wasConnected) {
        gWifiBridgeCfg.wasConnected = false;
        gDnsFilter.clearCache();
        gWifiBridgeCfg.nextRetryMillis = now + UPSTREAM_FAST_RETRY_DELAY_MS;
        return;
    }

    // 5. Waiting for backoff timer to expire before next attempt.
    if (gWifiBridgeCfg.lastAttemptMillis == 0 ||
        static_cast<int32_t>(now - gWifiBridgeCfg.nextRetryMillis) >= 0) {
        const bool useHint = gWifiBridgeCfg.fastRetryPending;
        gWifiBridgeCfg.fastRetryPending = false;
        wifiBridgeStartConnect(apSsid, useHint);
    }
}

static inline void syncNATState() {
    bool shouldEnable = WiFi.status() == WL_CONNECTED;
    if (gNatEnabled == shouldEnable) return;

#if ESP_IDF_VERSION_MAJOR >= 5
    // IDF 5.x exposes NAPT through esp_netif; verify return status so a
    // transient failure retries on the next loop tick.
    esp_netif_t* apNetif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!apNetif) return;
    if (shouldEnable) {
        if (esp_netif_napt_enable(apNetif) != ESP_OK) return;
    } else {
        esp_netif_napt_disable(apNetif);
        gDnsFilter.clearCache();
    }
#else
    // Must pass address in network byte order — lwip compares netif->ip_addr.addr
    // directly (u32_t in NBO). Arduino IPAddress' uint32_t conversion already
    // returns NBO on little-endian targets.
    uint32_t apAddrNbo = static_cast<uint32_t>(WiFi.softAPIP());
    if (apAddrNbo == 0) return;
    ip_napt_enable(static_cast<u32_t>(apAddrNbo), shouldEnable ? 1 : 0);
    if (!shouldEnable) gDnsFilter.clearCache();
#endif
    gNatEnabled = shouldEnable;
}

// ── NVS persistence ───────────────────────────────────────────────────────────
// Uses the "fsd" namespace (shared with main.cpp) and its own key prefixes
// (ub_*) to avoid collision.

// Tesla 推荐方案 — 当前 (v8): 基础服务 + 百度地图 + QQ/网易云/Apple Music
// + 豆包（Doubao）相关域名。
// v7 相比 v6：把 connman.vn.cloud.tesla.cn 加回白名单首位（用户指定）；
// www.tesla.cn 仍不放行。黑名单沿用 v6（含 assistant-api.prd.cnn1）。
// 默认额外放行车机常用流媒体，避免音乐/导航因白名单过窄而不可用。
//
// ⚠️ 关于两个特殊域名：
//   • hermes-prd.vn.cloud.tesla.cn：默认不放行（无车控版更安全）。仅 Model
//     3/Y 可能需要开启；Model S/X 不开启也会请求配置，且 S/X 请勿开启。
//     管理页可勾选「hermes 远程车控」，运行时追加到白名单最后一项；可能
//     触发 Tesla 风控甚至封禁，请谨慎使用。
//   • api-prd.vn.cloud.tesla.cn：默认放入黑名单。车机恢复出厂设置后，如
//     QQ 音乐无法显示扫码登录 / 网易云音乐异常，可能需要先把它加入白名单
//     （同长度时白名单优先，因此可同时保留黑名单条目）。放行后 Tesla 云端
//     可能改写 FSD 配置，需自行承担该风险；音乐软件恢复后应立刻移回黑名单
//     或断开 Wi-Fi。
//
// ⚠️ 一键恢复默认通过 /api/wifi-bridge/set?dnsReset=allow|block|all 直接使用
//    本文件的 kTeslaMinAllowV8 / kTeslaMinBlockV7，前端不再维护重复的预设常量。
// v8 默认 allowlist：在 v7 基础上补上 QQ 音乐显式域名
// （父域规则已覆盖，这里按用户要求显式列出）。
static const char* kTeslaMinAllowV8 =
    "connman.vn.cloud.tesla.cn nav-prd-maps.tesla.cn media-server-me.tesla.cn maps-cn-prd.go.tesla.services volcengine.com volces.com volcengineapi.com volccdn.com baidu.com bdimg.com bcebos.com qq.com qlogo.cn gtimg.cn qpic.cn tencentmusic.com bdycdn.cn qplaycloud.y.qq.com y.gtimg.cn y.qq.com stat6.y.qq.com music-file.y.qq.com qpic.y.qq.com isure6.stream.qqmusic.qq.com isure6-stream-qqmusic.a.bdycdn.cn 163.com 126.com 126.net netease.com 163jiasu.com 163yun.com *.apple.com mzstatic.com doubao.com doubao.cn doubaoapps.com doubaocdn.com butterflyai.cn butterfly-static.cn picpic-ai.cn pixeldance.com byteimg.com bytescm.com bytednsdoc.com pstatp.com snssdk.com bytedance.com";

// v7 默认 allowlist — 仅用于识别未自定义过的配置并迁移到 v8
static const char* kTeslaMinAllowV7 =
    "connman.vn.cloud.tesla.cn nav-prd-maps.tesla.cn media-server-me.tesla.cn maps-cn-prd.go.tesla.services volcengine.com volces.com volcengineapi.com volccdn.com baidu.com bdimg.com bcebos.com qq.com qlogo.cn gtimg.cn qpic.cn tencentmusic.com bdycdn.cn 163.com 126.com 126.net netease.com 163jiasu.com 163yun.com *.apple.com mzstatic.com doubao.com doubao.cn doubaoapps.com doubaocdn.com butterflyai.cn butterfly-static.cn picpic-ai.cn pixeldance.com byteimg.com bytescm.com bytednsdoc.com pstatp.com snssdk.com bytedance.com";

// v6 默认 allowlist — 仅用于识别未自定义过的配置并迁移到 v7
static const char* kTeslaMinAllowV6 =
    "nav-prd-maps.tesla.cn media-server-me.tesla.cn maps-cn-prd.go.tesla.services volcengine.com volces.com volcengineapi.com volccdn.com baidu.com bdimg.com bcebos.com qq.com qlogo.cn gtimg.cn qpic.cn tencentmusic.com bdycdn.cn 163.com 126.com 126.net netease.com 163jiasu.com 163yun.com *.apple.com mzstatic.com doubao.com doubao.cn doubaoapps.com doubaocdn.com butterflyai.cn butterfly-static.cn picpic-ai.cn pixeldance.com byteimg.com bytescm.com bytednsdoc.com pstatp.com snssdk.com bytedance.com";

// v5 默认 allowlist — 仅用于识别未自定义过的配置并迁移到 v7
static const char* kTeslaMinAllowV5 =
    "connman.vn.cloud.tesla.cn nav-prd-maps.tesla.cn media-server-me.tesla.cn maps-cn-prd.go.tesla.services www.tesla.cn volcengine.com volces.com volcengineapi.com volccdn.com baidu.com bdimg.com bcebos.com qq.com qlogo.cn gtimg.cn qpic.cn tencentmusic.com bdycdn.cn 163.com 126.com 126.net netease.com 163jiasu.com 163yun.com *.apple.com mzstatic.com doubao.com doubao.cn doubaoapps.com doubaocdn.com butterflyai.cn butterfly-static.cn picpic-ai.cn pixeldance.com byteimg.com bytescm.com bytednsdoc.com pstatp.com snssdk.com bytedance.com";

// v4 默认 allowlist — 仅用于识别未自定义过的配置并迁移
static const char* kTeslaMinAllowV4 =
    "connman.vn.cloud.tesla.cn nav-prd-maps.tesla.cn media-server-me.tesla.cn maps-cn-prd.go.tesla.services volcengine.com volces.com volcengineapi.com volccdn.com api.map.baidu.com lc.map.baidu.com newvector.map.baidu.com route.map.baidu.com newclient.map.baidu.com tracknavi.baidu.com itsmap3.baidu.com app.navi.baidu.com mapapip0.bdimg.com mapapisp0.bdimg.com automap0.bdimg.com baidunavi.cdn.bcebos.com lbsnavi.cdn.bcebos.com enlargeroad-view.su.bcebos.com qplaycloud.y.qq.com y.gtimg.cn y.qq.com stat6.y.qq.com music-file.y.qq.com qpic.y.qq.com isure6.stream.qqmusic.qq.com isure6-stream-qqmusic.a.bdycdn.cn music.163.com music.126.net aod-ssl.itunes.apple.com api.music.apple.com is1-ssl.mzstatic.com play.itunes.apple.com isure6-stream-xyy6.tencentmusic.com www.baidu.com www.qq.com *.tencentmusic.com www.tesla.cn";

// v3 默认 allowlist（含 hermes-prd / signaling / api-prd）— 用于一次性迁移识别
static const char* kTeslaMinAllowV3 =
    "connman.vn.cloud.tesla.cn nav-prd-maps.tesla.cn hermes-prd.vn.cloud.tesla.cn signaling.vn.cloud.tesla.cn api-prd.vn.cloud.tesla.cn media-server-me.tesla.cn www.tesla.cn maps-cn-prd.go.tesla.services volcengine.com volces.com volcengineapi.com volccdn.com api.map.baidu.com lc.map.baidu.com newvector.map.baidu.com route.map.baidu.com newclient.map.baidu.com tracknavi.baidu.com itsmap3.baidu.com app.navi.baidu.com mapapip0.bdimg.com mapapisp0.bdimg.com automap0.bdimg.com baidunavi.cdn.bcebos.com lbsnavi.cdn.bcebos.com enlargeroad-view.su.bcebos.com";

// v1.4.11 – v1.4.24 的默认 allowlist — 用于一次性迁移识别
static const char* kTeslaMinAllowV2 =
    "connman.vn.cloud.tesla.cn nav-prd-maps.tesla.cn hermes-prd.vn.cloud.tesla.cn signaling.vn.cloud.tesla.cn media-server-me.tesla.cn www.tesla.cn maps-cn-prd.go.tesla.services volcengine.com volces.com volcengineapi.com volccdn.com api.map.baidu.com lc.map.baidu.com newvector.map.baidu.com route.map.baidu.com newclient.map.baidu.com tracknavi.baidu.com itsmap3.baidu.com app.navi.baidu.com mapapip0.bdimg.com mapapisp0.bdimg.com automap0.bdimg.com baidunavi.cdn.bcebos.com lbsnavi.cdn.bcebos.com enlargeroad-view.su.bcebos.com";

// v1.4.10 及之前的默认 allowlist — 用于一次性迁移识别
static const char* kTeslaMinAllowV1 =
    "connman.vn.cloud.tesla.cn nav-prd-maps.tesla.cn hermes-prd.vn.cloud.tesla.cn signaling.vn.cloud.tesla.cn media-server-me.tesla.cn www.tesla.cn maps-cn-prd.go.tesla.services volcengine.com volces.com volcengineapi.com volccdn.com";

// v7 默认黑名单（外网）：控车/配置、母舰、语音助手、遥测、文件与升级端点，
// 以及 amazonaws.com.cn。FSD 劫持开启时其中 mothership/api-prd 由本地
// 443/4567 接管；劫持关闭时按黑名单阻断，避免车机直连外网被云端改写配置。
// hermes-prd 默认也在这里；启用「hermes 远程车控」时前端会把它从黑名单
// 临时移除，关闭车控后再加回。
static const char* kTeslaMinBlockV7 =
    "api-prd.vn.cloud.tesla.cn mothership.vn.teslamotors.com mothership.vn.cloud.tesla.cn assistant-api.prd.cnn1.vn.cloud.tesla.cn telemetry-prd.vn.cloud.tesla.cn hermes-stream-prd.vn.cloud.tesla.cn hermes-prd.vn.cloud.tesla.cn apigateway-x2-trigger.tesla.cn vehicle-files.prd.cnn1.vn.cloud.tesla.cn vehicle-files.prd.cn1.vn.cloud.tesla.cn upgrade.vn.cloud.tesla.cn amazonaws.com.cn";

// v6 默认黑名单 — 仅用于识别未自定义过的配置并迁移到 v7
static const char* kTeslaMinBlockV6 =
    "api-prd.vn.cloud.tesla.cn mothership.vn.teslamotors.com mothership.vn.cloud.tesla.cn assistant-api.prd.cnn1.vn.cloud.tesla.cn";

// v5 默认黑名单 — 仅用于识别未自定义过的配置并迁移到 v6
static const char* kTeslaMinBlockV5 =
    "api-prd.vn.cloud.tesla.cn mothership.vn.teslamotors.com mothership.vn.cloud.tesla.cn";

// v4 默认黑名单 — 仅用于识别未自定义过的配置并迁移
static const char* kTeslaMinBlockV4 =
    "api-prd.vn.cloud.tesla.cn";

// 旧版默认黑名单 — 仅用于识别未自定义过的配置并迁移
static const char* kTeslaMinBlockV3 =
    "tesla.cn tesla.com teslamotors.com tesla.services";

static inline void wifiBridgeLoadConfig() {
    Preferences p;
    p.begin("fsd", true);
    wifiBridgeClearAllNetworks();
    gWifiBridgeCfg.enabled = p.getBool("ub_en", false);
    uint8_t n = p.getUChar("ub_cnt", 0);
    for (uint8_t i = 0; i < n && i < MAX_UPSTREAM_NETWORKS; ++i) {
        char ssidKey[10]; char passKey[10];
        snprintf(ssidKey, sizeof(ssidKey), "ub_s%u", i);
        snprintf(passKey, sizeof(passKey), "ub_p%u", i);
        String ssid = p.getString(ssidKey, "");
        String pass = p.getString(passKey, "");
        if (!ssid.isEmpty() && pass.length() <= 63) {
            wifiBridgeAddOrUpdateNetwork(ssid, pass, true, nullptr);
        }
    }
    gDnsFilterCfg.enabled = p.getBool("ub_dnsEn", false);
    wifiBridgeCopyStr(gDnsFilterCfg.allowlist, sizeof(gDnsFilterCfg.allowlist), p.getString("ub_allow", ""));
    wifiBridgeCopyStr(gDnsFilterCfg.blocklist, sizeof(gDnsFilterCfg.blocklist), p.getString("ub_block", ""));
    const bool legacyIpBlk = p.getBool("ub_ipBlk", false);
    const uint8_t savedIpMode = p.getUChar("ub_ipMod", legacyIpBlk ? 1u : 0u);
    dnsIpPolicySetMode(savedIpMode <= 2u ? savedIpMode : 0u);
    // Last-known-good network index (-1 = no preference yet). Used to bias
    // the first connect attempt after boot toward the network that succeeded
    // last time, instead of always starting round-robin from index 0.
    int8_t pref = p.getChar("ub_pref", -1);
    gWifiBridgeCfg.preferredIndex = (pref >= 0 && pref < gWifiBridgeCfg.networkCount) ? pref : -1;
    bool initialized = p.getBool("ub_init", false);
    p.end();

    // First-run safety: default DNS filter ON with Tesla-recommended preset so
    // a user who forgets to configure before connecting their car to the bridge
    // still benefits from cloud-side protection. User can turn off later —
    // the ub_init flag persists so we never overwrite their choices.
    if (!initialized) {
        gDnsFilterCfg.enabled = true;
        wifiBridgeCopyStr(gDnsFilterCfg.allowlist, sizeof(gDnsFilterCfg.allowlist), kTeslaMinAllowV8);
        wifiBridgeCopyStr(gDnsFilterCfg.blocklist, sizeof(gDnsFilterCfg.blocklist), kTeslaMinBlockV7);
        dnsPublishApInternetPolicy(gDnsFilterCfg);
        Preferences p2;
        p2.begin("fsd", false);
        p2.putBool("ub_dnsEn", true);
        p2.putString("ub_allow", gDnsFilterCfg.allowlist);
        p2.putString("ub_block", gDnsFilterCfg.blocklist);
        p2.putBool("ub_init", true);
        p2.end();
        return;
    }

    // Migration: 老用户如果 allowlist 完全等于 v1~v7 默认值（未自定义过），
    // 自动升级到 v8。黑名单只要仍是旧默认值（v3/v4/v5/v6）就升级到 v7
    // （新增遥测/文件/升级等端点与 hermes 默认阻断）；用户改过则不动。
    bool allowChanged = false;
    bool blockChanged = false;
    if (strcmp(gDnsFilterCfg.allowlist, kTeslaMinAllowV1) == 0 ||
        strcmp(gDnsFilterCfg.allowlist, kTeslaMinAllowV2) == 0 ||
        strcmp(gDnsFilterCfg.allowlist, kTeslaMinAllowV3) == 0 ||
        strcmp(gDnsFilterCfg.allowlist, kTeslaMinAllowV4) == 0 ||
        strcmp(gDnsFilterCfg.allowlist, kTeslaMinAllowV5) == 0 ||
        strcmp(gDnsFilterCfg.allowlist, kTeslaMinAllowV6) == 0 ||
        strcmp(gDnsFilterCfg.allowlist, kTeslaMinAllowV7) == 0) {
        wifiBridgeCopyStr(gDnsFilterCfg.allowlist, sizeof(gDnsFilterCfg.allowlist), kTeslaMinAllowV8);
        allowChanged = true;
    }
    if (strcmp(gDnsFilterCfg.blocklist, kTeslaMinBlockV3) == 0 ||
        strcmp(gDnsFilterCfg.blocklist, kTeslaMinBlockV4) == 0 ||
        strcmp(gDnsFilterCfg.blocklist, kTeslaMinBlockV5) == 0 ||
        strcmp(gDnsFilterCfg.blocklist, kTeslaMinBlockV6) == 0) {
        wifiBridgeCopyStr(gDnsFilterCfg.blocklist, sizeof(gDnsFilterCfg.blocklist), kTeslaMinBlockV7);
        blockChanged = true;
    }
    dnsPublishApInternetPolicy(gDnsFilterCfg);
    if (allowChanged || blockChanged) {
        Preferences p3;
        p3.begin("fsd", false);
        if (allowChanged) p3.putString("ub_allow", gDnsFilterCfg.allowlist);
        if (blockChanged) p3.putString("ub_block", gDnsFilterCfg.blocklist);
        p3.end();
    }
}

static inline void wifiBridgeSaveConfig() {
    // Snapshot networks[] under mux so HTTP handlers can't mutate the array
    // mid-iteration (torn ssid/pass strings in NVS). NVS writes run outside
    // the critical section since Preferences API may block.
    bool     snapEnabled = false;
    uint8_t  snapCount   = 0;
    SavedUpstreamNetwork snapNetworks[MAX_UPSTREAM_NETWORKS];
    portENTER_CRITICAL(&gWifiBridgeMux);
    snapEnabled = gWifiBridgeCfg.enabled;
    snapCount   = gWifiBridgeCfg.networkCount;
    for (uint8_t i = 0; i < MAX_UPSTREAM_NETWORKS; ++i) {
        snapNetworks[i] = gWifiBridgeCfg.networks[i];
    }
    portEXIT_CRITICAL(&gWifiBridgeMux);

    Preferences p;
    p.begin("fsd", false);
    p.putBool("ub_en", snapEnabled);
    p.putUChar("ub_cnt", snapCount);
    for (uint8_t i = 0; i < MAX_UPSTREAM_NETWORKS; ++i) {
        char ssidKey[10]; char passKey[10];
        snprintf(ssidKey, sizeof(ssidKey), "ub_s%u", i);
        snprintf(passKey, sizeof(passKey), "ub_p%u", i);
        if (i < snapCount) {
            p.putString(ssidKey, snapNetworks[i].ssid);
            p.putString(passKey, snapNetworks[i].pass);
        } else {
            p.remove(ssidKey);
            p.remove(passKey);
        }
    }
    p.putBool("ub_dnsEn", gDnsFilterCfg.enabled);
    p.putString("ub_allow", gDnsFilterCfg.allowlist);
    p.putString("ub_block", gDnsFilterCfg.blocklist);
    const uint8_t ipMode = dnsIpPolicyGetMode();
    p.putBool("ub_ipBlk", ipMode != 0);
    p.putUChar("ub_ipMod", ipMode);
    p.end();
}

// ── Status text helpers ───────────────────────────────────────────────────────
static inline const char* wifiBridgeUpstreamStatusText() {
    if (!gWifiBridgeCfg.enabled)        return "未启用";
    if (!wifiBridgeHasCredentials())    return "未配置热点";
    if (thermalProtectActive())         return "过热保护中";
    if (WiFi.status() == WL_CONNECTED)  return "已连接";
    if (gWifiBridgeCfg.connecting)      return "连接中";

    const uint8_t reason = gWifiBridgeCfg.lastDisconnectReason;
    if (reason == WIFI_REASON_AUTH_FAIL || reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT) {
        return "认证/密码失败";
    }
    if (reason == WIFI_REASON_NO_AP_FOUND)    return "找不到热点";
    if (reason == WIFI_REASON_BEACON_TIMEOUT) return "信号丢失";

    switch (WiFi.status()) {
        case WL_NO_SSID_AVAIL:   return "找不到热点";
        case WL_CONNECT_FAILED:  return "连接失败";
        case WL_CONNECTION_LOST: return "连接丢失";
        case WL_DISCONNECTED:    return "等待重连";
        case WL_IDLE_STATUS:
        default:                 return "连接中";
    }
}

static inline const char* wifiBridgeNatStatusText() {
    if (gNatEnabled) return "已启用";
    if (WiFi.status() == WL_CONNECTED) return "待启用";
    return "未连接上游";
}

static inline const char* wifiBridgeSignalText(int32_t rssi) {
    if (rssi >= -55) return "优秀";
    if (rssi >= -67) return "良好";
    if (rssi >= -75) return "一般";
    return "较弱";
}

static inline size_t wifiBridgeDnsRuleCount(const char* rules) {
    size_t count = 0;
    if (rules == nullptr) return 0;
    const char* cursor = rules;
    while (*cursor) {
        while (*cursor && (isspace(static_cast<unsigned char>(*cursor)) || *cursor == ',' || *cursor == ';')) ++cursor;
        if (!*cursor) break;
        ++count;
        while (*cursor && !(isspace(static_cast<unsigned char>(*cursor)) || *cursor == ',' || *cursor == ';')) ++cursor;
    }
    return count;
}

#endif // WIFI_BRIDGE_ENABLED
