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
static constexpr uint8_t MAX_UPSTREAM_NETWORKS = 10;

// Exponential-backoff schedule for repeated upstream connect failures.
// Indexed by consecutiveFailures count; clamped to last entry. Replaces the
// old fixed 3 s / 15 s retry pair, which retried wrong-password / out-of-range
// routers every few seconds indefinitely (radio thrash + user-visible spam).
// Reset to 0 on successful connect or when the user re-saves the network list.
static constexpr uint32_t kUpstreamBackoffMs[] = {
    3000, 8000, 15000, 30000, 60000, 120000, 300000
};
static constexpr uint8_t kUpstreamBackoffSteps =
    sizeof(kUpstreamBackoffMs) / sizeof(kUpstreamBackoffMs[0]);

struct SavedUpstreamNetwork {
    char ssid[33] = {};
    char pass[65] = {};
};

struct UpstreamWiFiConfig {
    bool                 enabled             = false;
    SavedUpstreamNetwork networks[MAX_UPSTREAM_NETWORKS];
    uint8_t              networkCount        = 0;
    int8_t               activeIndex         = -1;
    uint8_t              nextTryIndex        = 0;
    volatile bool        applyRequested      = false;
    uint32_t             lastAttemptMillis   = 0;
    uint8_t              consecutiveFailures = 0;
    int8_t               preferredIndex      = -1;  // last network that connected; tried first after apply/boot
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

    if (gWifiBridgeCfg.networkCount == 0) {
        gWifiBridgeCfg.activeIndex = -1;
        gWifiBridgeCfg.nextTryIndex = 0;
    } else {
        if (gWifiBridgeCfg.activeIndex == index) {
            gWifiBridgeCfg.activeIndex = -1;
        } else if (gWifiBridgeCfg.activeIndex > index) {
            --gWifiBridgeCfg.activeIndex;
        }
        if (gWifiBridgeCfg.nextTryIndex >= gWifiBridgeCfg.networkCount) {
            gWifiBridgeCfg.nextTryIndex = 0;
        }
    }
    portEXIT_CRITICAL(&gWifiBridgeMux);
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

static inline int wifiBridgeSelectNextNetworkIndex(const char* /*apSsid*/) {
    if (!wifiBridgeHasCredentials()) return -1;

    // Do NOT scan on every reconnect retry: a full-channel scan in APSTA mode
    // briefly drops the AP's beacon, kicking the phone off Wi-Fi every 15s.
    // Just round-robin through saved networks — WiFi.begin() will succeed if
    // the SSID is in range and fail fast (WL_NO_SSID_AVAIL) otherwise.
    if (gWifiBridgeCfg.nextTryIndex >= gWifiBridgeCfg.networkCount) {
        gWifiBridgeCfg.nextTryIndex = 0;
    }
    int selectedIndex = gWifiBridgeCfg.nextTryIndex;
    gWifiBridgeCfg.nextTryIndex = (gWifiBridgeCfg.nextTryIndex + 1) % gWifiBridgeCfg.networkCount;
    return selectedIndex;
}

static inline void wifiBridgeStartConnect(const char* apSsid) {
    if (!gWifiBridgeCfg.enabled || !wifiBridgeHasCredentials()) return;

    // Copy ssid/pass under mux; WiFi.begin blocks and must not hold a portMUX.
    char ssid[33] = {};
    char pass[65] = {};
    bool have = false;

    portENTER_CRITICAL(&gWifiBridgeMux);
    int idx = wifiBridgeSelectNextNetworkIndex(apSsid);
    if (idx >= 0 && idx < gWifiBridgeCfg.networkCount) {
        gWifiBridgeCfg.activeIndex = idx;
        gWifiBridgeCfg.nextTryIndex = (idx + 1) % gWifiBridgeCfg.networkCount;
        memcpy(ssid, gWifiBridgeCfg.networks[idx].ssid, sizeof(ssid));
        memcpy(pass, gWifiBridgeCfg.networks[idx].pass, sizeof(pass));
        have = true;
    } else {
        gWifiBridgeCfg.activeIndex = -1;
    }
    portEXIT_CRITICAL(&gWifiBridgeMux);

    if (!have) {
        gWifiBridgeCfg.lastAttemptMillis = millis();
        return;
    }

    WiFi.disconnect(false, false);
    if (pass[0] != '\0') WiFi.begin(ssid, pass);
    else                 WiFi.begin(ssid);
    gWifiBridgeCfg.lastAttemptMillis = millis();
}

static inline void wifiBridgeRequestApply() { gWifiBridgeCfg.applyRequested = true; }

static inline void wifiBridgeApplyConfig(const char* apSsid) {
    // Apply always resets the backoff counter — user-driven config change is
    // an explicit signal to retry promptly even if the previous attempts were
    // in long-backoff territory.
    gWifiBridgeCfg.consecutiveFailures = 0;

    if (thermalProtectActive()) {
        WiFi.disconnect(false, true);
        gWifiBridgeCfg.activeIndex = -1;
        gWifiBridgeCfg.nextTryIndex = 0;
        gWifiBridgeCfg.lastAttemptMillis = 0;
        wifiBridgeForceApChannel1();
        return;
    }

    if (!gWifiBridgeCfg.enabled || !wifiBridgeHasCredentials()) {
        WiFi.disconnect(false, true);
        gWifiBridgeCfg.activeIndex = -1;
        gWifiBridgeCfg.nextTryIndex = 0;
        gWifiBridgeCfg.lastAttemptMillis = 0;
        wifiBridgeForceApChannel1();
        return;
    }

    WiFi.disconnect(false, true);
    gWifiBridgeCfg.activeIndex = -1;
    // Start with the last-known-good network if it's still in the saved list.
    // Falls back to round-robin from index 0 if no preference exists.
    if (gWifiBridgeCfg.preferredIndex >= 0
     && gWifiBridgeCfg.preferredIndex < gWifiBridgeCfg.networkCount) {
        gWifiBridgeCfg.nextTryIndex = (uint8_t)gWifiBridgeCfg.preferredIndex;
    } else {
        gWifiBridgeCfg.nextTryIndex = 0;
    }
    wifiBridgeStartConnect(apSsid);
}

static inline void serviceUpstreamWiFi(const char* apSsid) {
    if (gWifiBridgeCfg.applyRequested) {
        gWifiBridgeCfg.applyRequested = false;
        wifiBridgeApplyConfig(apSsid);
        return;
    }

    if (!gWifiBridgeCfg.enabled || !wifiBridgeHasCredentials()) return;

    if (thermalProtectActive()) {
        if (WiFi.status() == WL_CONNECTED) {
            WiFi.disconnect(false, true);
            gWifiBridgeCfg.activeIndex = -1;
            gWifiBridgeCfg.lastAttemptMillis = 0;
        }
        return;
    }

    if (WiFi.status() == WL_CONNECTED) {
        int idx = wifiBridgeFindNetwork(WiFi.SSID());
        if (idx >= 0) gWifiBridgeCfg.activeIndex = idx;
        // First successful connect (or transition from failures): reset backoff
        // and persist this network as the preferred one for next boot.
        if (gWifiBridgeCfg.consecutiveFailures > 0 || gWifiBridgeCfg.preferredIndex != idx) {
            gWifiBridgeCfg.consecutiveFailures = 0;
            if (idx >= 0 && gWifiBridgeCfg.preferredIndex != idx) {
                gWifiBridgeCfg.preferredIndex = idx;
                Preferences p;
                if (p.begin("fsd", false)) {
                    p.putChar("ub_pref", (int8_t)idx);
                    p.end();
                }
            }
        }
        return;
    }

    uint32_t now = millis();
    uint8_t  fails = gWifiBridgeCfg.consecutiveFailures;
    if (fails >= kUpstreamBackoffSteps) fails = kUpstreamBackoffSteps - 1;
    uint32_t retryDelay = kUpstreamBackoffMs[fails];
    if (thermalThrottleActive() && retryDelay < UPSTREAM_RETRY_THROTTLED_MS) {
        retryDelay = UPSTREAM_RETRY_THROTTLED_MS;
    }
    // In APSTA mode every WiFi.begin() can briefly disturb the AP (channel
    // switch/scan), which users see as "connected, then DHCP/IP lost". When a
    // client is already on the AP and the upstream has failed, retry slowly.
    if (WiFi.softAPgetStationNum() > 0 && gWifiBridgeCfg.consecutiveFailures > 0 &&
        retryDelay < UPSTREAM_RETRY_THROTTLED_MS) {
        retryDelay = UPSTREAM_RETRY_THROTTLED_MS;
    }

    bool shouldRetry = (gWifiBridgeCfg.lastAttemptMillis == 0)
                    || (now - gWifiBridgeCfg.lastAttemptMillis >= retryDelay);

    if (shouldRetry) {
        // Saturate at the last backoff step — incrementing past it would just
        // waste cycles since we already clamp `fails` to the table size above.
        if (gWifiBridgeCfg.consecutiveFailures < kUpstreamBackoffSteps) {
            gWifiBridgeCfg.consecutiveFailures++;
        }
        wifiBridgeStartConnect(apSsid);
    }
}

static inline void syncNATState() {
    bool shouldEnable = WiFi.status() == WL_CONNECTED;
    if (gNatEnabled == shouldEnable) return;

#if ESP_IDF_VERSION_MAJOR >= 5
    // IDF 5.x exposes NAPT through esp_netif; no raw address/NBO handling.
    esp_netif_t* apNetif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (apNetif) {
        if (shouldEnable) {
            esp_netif_napt_enable(apNetif);
        } else {
            esp_netif_napt_disable(apNetif);
        }
    }
#else
    // Must pass address in network byte order — lwip compares netif->ip_addr.addr
    // directly (u32_t in NBO). Manual big-endian shifting mismatches lwip's netif
    // on little-endian ESP32, causing ip_napt_enable to fall through to
    // ip_napt_deinit() which mem_free(NULL)s and asserts. Arduino IPAddress'
    // uint32_t conversion already returns NBO on little-endian targets.
    uint32_t apAddrNbo = static_cast<uint32_t>(WiFi.softAPIP());
    ip_napt_enable(static_cast<u32_t>(apAddrNbo), shouldEnable ? 1 : 0);
#endif
    gNatEnabled = shouldEnable;
}

// ── NVS persistence ───────────────────────────────────────────────────────────
// Uses the "fsd" namespace (shared with main.cpp) and its own key prefixes
// (ub_*) to avoid collision.

// Tesla 推荐方案 — 当前 (v4): 基础服务 + 百度地图 + QQ/网易云/Apple Music。
// 默认额外放行车机常用流媒体，避免音乐/导航因白名单过窄而不可用。
//
// ⚠️ 关于两个特殊域名（默认都不按“可远程车控”处理）：
//   • hermes-prd.vn.cloud.tesla.cn：默认不加入白名单。手动加入后可以远程
//     车控，但可能触发 Tesla 风控甚至封禁，谨慎使用。
//   • api-prd.vn.cloud.tesla.cn：默认放入黑名单。车机恢复出厂设置后，如
//     QQ 音乐无法显示扫码登录 / 网易云音乐异常，可能需要先把它加入白名单
//     （同长度时白名单优先，因此可同时保留黑名单条目）。放行后 Tesla 云端
//     可能改写 FSD 配置，需自行承担该风险；音乐软件恢复后应立刻移回黑名单
//     或断开 Wi-Fi。
//
// ⚠️ 一键恢复默认通过 /api/wifi-bridge/set?dnsReset=allow|block|all 直接使用
//    本文件的 kTeslaMinAllowV4 / kTeslaMinBlockV5，前端不再维护重复的预设常量。
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

// v5 默认黑名单（外网）：api-prd 是 Tesla 手机 App 控车 / 车辆配置端点；
// mothership.* 是车辆母舰配置端点。FSD 劫持开启时它们由本地 443/4567
// 接管；劫持关闭时按黑名单阻断，避免车机直连外网被云端改写配置。
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
#ifdef FSD_MOCK_ONLY
    dnsIpBlockerSetEnabled(p.getBool("ub_ipBlk", false));
#endif
    // Last-known-good network index (-1 = no preference yet). Used to bias
    // the first connect attempt after boot toward the network that succeeded
    // last time, instead of always starting round-robin from index 0.
    int8_t pref = p.getChar("ub_pref", -1);
    gWifiBridgeCfg.preferredIndex = (pref >= 0 && pref < gWifiBridgeCfg.networkCount) ? pref : -1;
    bool initialized = p.getBool("ub_init", false);
#ifndef FSD_MOCK_ONLY
    pingLoadEnabled(p);
#endif
    p.end();

    // First-run safety: default DNS filter ON with Tesla-recommended preset so
    // a user who forgets to configure before connecting their car to the bridge
    // still benefits from cloud-side protection. User can turn off later —
    // the ub_init flag persists so we never overwrite their choices.
    if (!initialized) {
        gDnsFilterCfg.enabled = true;
        wifiBridgeCopyStr(gDnsFilterCfg.allowlist, sizeof(gDnsFilterCfg.allowlist), kTeslaMinAllowV4);
        wifiBridgeCopyStr(gDnsFilterCfg.blocklist, sizeof(gDnsFilterCfg.blocklist), kTeslaMinBlockV5);
        Preferences p2;
        p2.begin("fsd", false);
        p2.putBool("ub_dnsEn", true);
        p2.putString("ub_allow", gDnsFilterCfg.allowlist);
        p2.putString("ub_block", gDnsFilterCfg.blocklist);
        p2.putBool("ub_init", true);
        p2.end();
        return;
    }

    // Migration: 老用户如果 allowlist 完全等于 v1 / v2 / v3 默认值（未自定义过），
    // 自动升级到 v4（去掉 hermes-prd / signaling / api-prd，补充音乐与视频域名）。
    // 黑名单只要仍是旧默认值（v3 的整段 tesla.* 或 v4 的仅 api-prd）就升级到 v5，
    // 加入 mothership 母舰域名；用户改过则不动，尊重用户选择。
    bool allowChanged = false;
    bool blockChanged = false;
    if (strcmp(gDnsFilterCfg.allowlist, kTeslaMinAllowV1) == 0 ||
        strcmp(gDnsFilterCfg.allowlist, kTeslaMinAllowV2) == 0 ||
        strcmp(gDnsFilterCfg.allowlist, kTeslaMinAllowV3) == 0) {
        wifiBridgeCopyStr(gDnsFilterCfg.allowlist, sizeof(gDnsFilterCfg.allowlist), kTeslaMinAllowV4);
        allowChanged = true;
    }
    if (strcmp(gDnsFilterCfg.blocklist, kTeslaMinBlockV3) == 0 ||
        strcmp(gDnsFilterCfg.blocklist, kTeslaMinBlockV4) == 0) {
        wifiBridgeCopyStr(gDnsFilterCfg.blocklist, sizeof(gDnsFilterCfg.blocklist), kTeslaMinBlockV5);
        blockChanged = true;
    }
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
#ifdef FSD_MOCK_ONLY
    p.putBool("ub_ipBlk", dnsIpBlockerEnabled());
#endif
#ifndef FSD_MOCK_ONLY
    pingSaveEnabled(p);
#endif
    p.end();
}

// ── Status text helpers ───────────────────────────────────────────────────────
static inline const char* wifiBridgeUpstreamStatusText() {
    if (!gWifiBridgeCfg.enabled)        return "未启用";
    if (!wifiBridgeHasCredentials())    return "未配置热点";
    if (thermalProtectActive())         return "过热保护中";

    switch (WiFi.status()) {
        case WL_CONNECTED:       return "已连接";
        case WL_NO_SSID_AVAIL:   return "找不到热点";
        case WL_CONNECT_FAILED:  return "连接失败";
        case WL_CONNECTION_LOST: return "连接丢失";
        case WL_DISCONNECTED:    return "未连接";
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

static inline uint8_t wifiBridgeDnsRuleCount(const char* rules) {
    uint8_t count = 0;
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
