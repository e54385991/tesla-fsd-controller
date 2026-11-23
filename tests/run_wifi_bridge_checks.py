#!/usr/bin/env python3
"""Run Wi-Fi bridge state-machine & policy regression tests without hardware.

Compiles production functions extracted from include/mod_wifi_bridge.h against
a minimal host stub (with ASan + UBSan) to verify:
  * 12 s connect/DHCP timeout does not abort in-flight handshakes at 3 s
  * STA_DISCONNECTED events end attempts immediately and classify transient vs
    non-transient reasons (2 s fast same-SSID channel-hint retry vs backoff)
  * Async scan running pauses reconnect attempts
  * AP clients connected enforce >= 60 s backoff after failure
  * wifiBridgeRemoveNetwork keeps preferredIndex/activeIndex/nextTryIndex coherent
  * FSD mock defaults to disabled and Wi-Fi bridge sets autoReconnect(false)

Run: python3 tests/run_wifi_bridge_checks.py
"""
from __future__ import annotations

import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BRIDGE_SRC = (ROOT / "include/mod_wifi_bridge.h").read_text()
MAIN_SRC = (ROOT / "src/main.cpp").read_text()
FSD_SRC = (ROOT / "src/fsd_mock.cpp").read_text()
CLIENT_CFG_SRC = (ROOT / "src/client_config.cpp").read_text()


def extract_function(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def check_static_contracts() -> None:
    assert "WiFi.useStaticBuffers(true);" in MAIN_SRC, "Bridge must preserve sdkconfig STATIC_RX_BUFFER_NUM"
    assert "esp_task_wdt_reconfigure(" in MAIN_SRC, "IDF 5.x must reconfigure pre-initialized TWDT to 30s"
    assert "WiFi.setAutoReconnect(false);" in MAIN_SRC, "Bridge must disable Arduino autoReconnect"
    assert "esp_wifi_set_inactive_time(WIFI_IF_STA, 12);" in MAIN_SRC
    assert "wifiBridgeRegisterEvents();" in MAIN_SRC
    assert "wifiBridgeConfigureApIpAndDns();" in MAIN_SRC
    assert "client_config::serviceListeners();" in MAIN_SRC
    assert "safeModeActive = true" not in MAIN_SRC, "FSD bridge must never lock out listeners via safe mode"
    assert "uint8_t enabled = 0;" in FSD_SRC, "FSD mock Settings::enabled must default to 0"
    assert "bool enabled = false;" in FSD_SRC, "FSD mock State::enabled must default to false"
    assert "uint8_t autoOffOnHit = 1;" in FSD_SRC, "FSD mock autoOffOnHit must default to 1"
    assert "setListenersReady(true)" in CLIENT_CFG_SRC
    assert "setListenersReady(false)" in CLIENT_CFG_SRC
    dns_src = (ROOT / "include/mod_dns.h").read_text()
    assert "static_cast<uint32_t>(answer) != 0" in dns_src, "dnsSetFsdRedirect must reject 0.0.0.0"
    assert "vTaskDelay(pdMS_TO_TICKS(" in dns_src, "DNS waitForRequest must not busy-spin on error"

    pio = (ROOT / "platformio.ini").read_text()
    assert pio.count("-DIP_NAPT_TIMEOUT_MS_UDP=30000") == 4, "All 4 bridge envs must set 30s NAPT UDP timeout"
    assert "TWAI_" not in pio, "CAN TWAI pins must be removed from platformio.ini"
    assert "[env:esp32]" not in pio and "[env:esp32s3-waveshare]" not in pio, "CAN-only envs must be removed"
    assert "DNSServer" not in MAIN_SRC and "driver/twai.h" not in MAIN_SRC and "canTask" not in MAIN_SRC

    s3_cfg = (ROOT / "sdkconfig.esp32s3-waveshare-wifi").read_text()
    for needle in (
        "CONFIG_COMPILER_OPTIMIZATION_PERF=y",
        "CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=240",
        "CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=10",
        "CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=16",
        "CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM=16",
        "CONFIG_ESP_WIFI_RX_BA_WIN=10",
        "CONFIG_ESP_WIFI_TX_BA_WIN=10",
        "# CONFIG_ESP_WIFI_EXTRA_IRAM_OPT is not set",
        "CONFIG_MBEDTLS_DYNAMIC_BUFFER=y",
        "# CONFIG_MBEDTLS_DYNAMIC_FREE_CONFIG_DATA is not set",
        "CONFIG_LWIP_MAX_SOCKETS=16",
        "CONFIG_LWIP_UDP_RECVMBOX_SIZE=16",
        "CONFIG_LWIP_TCPIP_RECVMBOX_SIZE=48",
        "CONFIG_LWIP_TCP_SACK_OUT=y",
        "CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU1=y",
    ):
        assert needle in s3_cfg, f"sdkconfig.esp32s3-waveshare-wifi missing {needle}"

    assert "forEachBlockedDomainSorted(" in MAIN_SRC, "Status/CSV endpoints must stream blocked domains without 6.8 KB heap copy"
    assert "new (std::nothrow) DNSBlockedDomainStatEntry[" not in MAIN_SRC, "Avoid 6.8 KB DNSBlockedDomainStatEntry heap spikes"

    esp32_cfg = (ROOT / "sdkconfig.esp32-wifi").read_text()
    assert "CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU1=y" in esp32_cfg, "esp32-wifi must pin lwIP TCPIP to Core 1"


PREAMBLE = r'''
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

struct String : std::string {
    using std::string::string;
    String(const std::string& v) : std::string(v) {}
    bool isEmpty() const { return empty(); }
    bool equals(const char* other) const { return other && std::strcmp(c_str(), other) == 0; }
    bool equals(const String& other) const { return *this == other; }
    void trim() {
        while (!empty() && std::isspace(static_cast<unsigned char>(back()))) pop_back();
        erase(begin(), std::find_if_not(begin(), end(), [](unsigned char c) { return std::isspace(c); }));
    }
    void toCharArray(char* dest, size_t cap) const {
        size_t n = std::min(size(), cap - 1);
        std::memcpy(dest, c_str(), n);
        dest[n] = '\0';
    }
};

enum wl_status_t { WL_IDLE_STATUS = 0, WL_NO_SSID_AVAIL = 1, WL_CONNECTED = 3, WL_CONNECT_FAILED = 4, WL_CONNECTION_LOST = 5, WL_DISCONNECTED = 6 };
enum { WIFI_SCAN_RUNNING = -1, WIFI_SCAN_FAILED = -2 };
enum {
    WIFI_REASON_ASSOC_EXPIRE = 4,
    WIFI_REASON_ASSOC_LEAVE = 8,
    WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT = 15,
    WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT = 16,
    WIFI_REASON_BEACON_TIMEOUT = 200,
    WIFI_REASON_NO_AP_FOUND = 201,
    WIFI_REASON_AUTH_FAIL = 202,
    WIFI_REASON_ASSOC_FAIL = 203,
    WIFI_REASON_HANDSHAKE_TIMEOUT = 204,
    WIFI_REASON_CONNECTION_FAIL = 205,
    WIFI_REASON_AP_TSF_RESET = 206,
    WIFI_REASON_ROAMING = 207,
};
enum {
    ARDUINO_EVENT_WIFI_STA_GOT_IP = 1,
    ARDUINO_EVENT_WIFI_STA_DISCONNECTED = 2,
    ARDUINO_EVENT_WIFI_AP_STACONNECTED = 3,
    ARDUINO_EVENT_WIFI_AP_STADISCONNECTED = 4,
};
struct arduino_event_t {
    int event_id = 0;
    struct {
        struct { uint8_t reason = 0; } wifi_sta_disconnected;
    } event_info;
};
typedef enum { WIFI_SECOND_CHAN_NONE = 0 } wifi_second_chan_t;
#define ESP_OK 0
static uint8_t gFakeChannel = 6;
static int esp_wifi_get_channel(uint8_t* ch, wifi_second_chan_t* sec) {
    *ch = gFakeChannel;
    if (sec) *sec = WIFI_SECOND_CHAN_NONE;
    return ESP_OK;
}

static uint32_t gNowMs = 0;
static uint32_t millis() { return gNowMs; }
static bool gThermalProtect = false;
static bool gThermalThrottle = false;
static bool thermalProtectActive() { return gThermalProtect; }
static bool thermalThrottleActive() { return gThermalThrottle; }
static int gForceCh1Calls = 0;
void wifiBridgeForceApChannel1() { ++gForceCh1Calls; }

struct FakeDns {
    int clearCalls = 0;
    void clearCache() { ++clearCalls; }
} gDnsFilter;

struct Preferences {
    static int8_t lastPref;
    bool begin(const char*, bool) { return true; }
    void putChar(const char*, int8_t v) { lastPref = v; }
    void end() {}
};
int8_t Preferences::lastPref = -1;

struct BeginCall {
    std::string ssid;
    std::string pass;
    int32_t channel;
    uint32_t atMs;
};

struct FakeWiFi {
    wl_status_t st = WL_DISCONNECTED;
    std::string curSsid;
    int16_t scanState = WIFI_SCAN_FAILED;
    uint8_t apStations = 0;
    int disconnectCalls = 0;
    std::vector<BeginCall> begins;
    void (*eventCb)(arduino_event_t*) = nullptr;

    wl_status_t status() const { return st; }
    String SSID() const { return String(curSsid); }
    int16_t scanComplete() const { return scanState; }
    uint8_t softAPgetStationNum() const { return apStations; }
    void disconnect(bool = false, bool = false) {
        ++disconnectCalls;
        if (st == WL_CONNECTED) st = WL_DISCONNECTED;
    }
    void begin(const char* ssid, const char* pass = nullptr, int32_t ch = 0) {
        begins.push_back({ssid ? ssid : "", pass ? pass : "", ch, gNowMs});
    }
    void onEvent(void (*cb)(arduino_event_t*)) { eventCb = cb; }
} WiFi;

typedef int portMUX_TYPE;
static portMUX_TYPE gWifiBridgeMux = 0;
#define portENTER_CRITICAL(x) do { (void)(x); } while (0)
#define portEXIT_CRITICAL(x) do { (void)(x); } while (0)

static constexpr uint32_t UPSTREAM_RETRY_THROTTLED_MS = 60000;
static constexpr uint32_t UPSTREAM_CONNECT_TIMEOUT_MS = 12000;
static constexpr uint32_t UPSTREAM_FAST_RETRY_DELAY_MS = 2000;
static constexpr uint8_t  MAX_UPSTREAM_NETWORKS = 10;
static constexpr uint32_t kUpstreamBackoffMs[] = { 3000, 8000, 15000, 30000, 60000, 120000, 300000 };
static constexpr uint8_t  kUpstreamBackoffSteps = sizeof(kUpstreamBackoffMs) / sizeof(kUpstreamBackoffMs[0]);

struct SavedUpstreamNetwork {
    char ssid[33] = {};
    char pass[65] = {};
    uint8_t lastChannel = 0;
};
struct UpstreamWiFiConfig {
    bool enabled = false;
    SavedUpstreamNetwork networks[MAX_UPSTREAM_NETWORKS];
    uint8_t networkCount = 0;
    int8_t activeIndex = -1;
    uint8_t nextTryIndex = 0;
    volatile bool applyRequested = false;
    uint32_t lastAttemptMillis = 0;
    uint32_t connectStartMillis = 0;
    uint32_t nextRetryMillis = 0;
    uint8_t consecutiveFailures = 0;
    int8_t preferredIndex = -1;
    bool connecting = false;
    bool wasConnected = false;
    bool fastRetryPending = false;
    bool usedChannelHint = false;
    volatile bool pendingDisconnectEvent = false;
    volatile bool pendingGotIpEvent = false;
    volatile uint8_t lastDisconnectReason = 0;
    volatile uint32_t connectSuccessCount = 0;
    volatile uint32_t disconnectCount = 0;
    volatile uint32_t apClientJoinCount = 0;
    volatile uint32_t apClientLeaveCount = 0;
};
static UpstreamWiFiConfig gWifiBridgeCfg;
'''

TESTS = r'''
static void fireDisconnect(uint8_t reason) {
    arduino_event_t ev;
    ev.event_id = ARDUINO_EVENT_WIFI_STA_DISCONNECTED;
    ev.event_info.wifi_sta_disconnected.reason = reason;
    wifiBridgeOnEvent(&ev);
}

int main() {
    // 1. Add 3 networks and test preferredIndex maintenance on removal.
    assert(wifiBridgeAddOrUpdateNetwork("HomeA", "pass1111", true, "fsd-esp32"));
    assert(wifiBridgeAddOrUpdateNetwork("PhoneB", "pass2222", true, "fsd-esp32"));
    assert(wifiBridgeAddOrUpdateNetwork("OfficeC", "pass3333", true, "fsd-esp32"));
    gWifiBridgeCfg.preferredIndex = 2;  // OfficeC
    assert(wifiBridgeRemoveNetwork("HomeA"));
    assert(gWifiBridgeCfg.networkCount == 2);
    assert(gWifiBridgeCfg.preferredIndex == 1);  // shifted to OfficeC's new index
    assert(Preferences::lastPref == 1);
    assert(wifiBridgeRemoveNetwork("OfficeC"));
    assert(gWifiBridgeCfg.preferredIndex == -1);
    assert(Preferences::lastPref == -1);

    // Re-add second network for state-machine tests: [PhoneB, BackupD].
    assert(wifiBridgeAddOrUpdateNetwork("BackupD", "pass4444", true, "fsd-esp32"));
    gWifiBridgeCfg.enabled = true;
    gWifiBridgeCfg.preferredIndex = 0;

    // 2. Apply starts connecting to PhoneB; must NOT abort at 3 s..11.9 s (slow DHCP).
    gNowMs = 1000;
    wifiBridgeRequestApply();
    serviceUpstreamWiFi("fsd-esp32");
    assert(WiFi.begins.size() == 1);
    assert(WiFi.begins[0].ssid == "PhoneB" && WiFi.begins[0].channel == 0);
    assert(gWifiBridgeCfg.connecting);

    // Voluntary ASSOC_LEAVE while connecting must NOT abort the attempt.
    fireDisconnect(WIFI_REASON_ASSOC_LEAVE);
    gNowMs = 4500;  // 3.5 s elapsed — previously aborted at 3 s!
    serviceUpstreamWiFi("fsd-esp32");
    assert(WiFi.begins.size() == 1);
    assert(gWifiBridgeCfg.connecting);

    gNowMs = 12999;  // 11.999 s elapsed — still waiting for DHCP
    serviceUpstreamWiFi("fsd-esp32");
    assert(WiFi.begins.size() == 1);

    // 3. Connect succeeds at t=13000 ms (before timeout if within window -> test connect!).
    gNowMs = 12500;
    WiFi.st = WL_CONNECTED;
    WiFi.curSsid = "PhoneB";
    gFakeChannel = 11;
    serviceUpstreamWiFi("fsd-esp32");
    assert(!gWifiBridgeCfg.connecting && gWifiBridgeCfg.wasConnected);
    assert(gWifiBridgeCfg.networks[0].lastChannel == 11);
    assert(gWifiBridgeCfg.consecutiveFailures == 0);

    // 4. Transient BEACON_TIMEOUT while connected -> 2 s fast retry on SAME SSID + ch 11.
    WiFi.st = WL_DISCONNECTED;
    gNowMs = 20000;
    fireDisconnect(WIFI_REASON_BEACON_TIMEOUT);
    serviceUpstreamWiFi("fsd-esp32");
    assert(gDnsFilter.clearCalls >= 1);
    assert(gWifiBridgeCfg.fastRetryPending);
    assert(gWifiBridgeCfg.nextRetryMillis == 22000);
    gNowMs = 21999;
    serviceUpstreamWiFi("fsd-esp32");
    assert(WiFi.begins.size() == 1);
    gNowMs = 22000;
    serviceUpstreamWiFi("fsd-esp32");
    assert(WiFi.begins.size() == 2);
    assert(WiFi.begins[1].ssid == "PhoneB" && WiFi.begins[1].channel == 11);

    // 5. Fast retry fails with NO_AP_FOUND while AP client is attached -> clears ch hint,
    //    and after second failure enforces >= 60 s AP-client protection backoff.
    WiFi.apStations = 1;
    gNowMs = 23000;
    fireDisconnect(WIFI_REASON_NO_AP_FOUND);
    serviceUpstreamWiFi("fsd-esp32");
    assert(gWifiBridgeCfg.networks[0].lastChannel == 0);
    assert(gWifiBridgeCfg.consecutiveFailures == 1);
    assert(gWifiBridgeCfg.nextRetryMillis == 23000 + 3000);  // first failure = 3 s

    // Async scan running pauses retry even past deadline.
    WiFi.scanState = WIFI_SCAN_RUNNING;
    gNowMs = 30000;
    serviceUpstreamWiFi("fsd-esp32");
    assert(WiFi.begins.size() == 2);

    // Scan finishes -> retries BackupD; fails with AUTH_FAIL -> >= 60 s throttle with AP client.
    WiFi.scanState = WIFI_SCAN_FAILED;
    serviceUpstreamWiFi("fsd-esp32");
    assert(WiFi.begins.size() == 3);
    assert(WiFi.begins[2].ssid == "BackupD" && WiFi.begins[2].channel == 0);
    gNowMs = 31000;
    fireDisconnect(WIFI_REASON_AUTH_FAIL);
    serviceUpstreamWiFi("fsd-esp32");
    assert(gWifiBridgeCfg.consecutiveFailures == 2);
    assert(gWifiBridgeCfg.nextRetryMillis == 31000 + 60000);

    // 6. Connect timeout (12 s) cleanly aborts a stalled DHCP attempt.
    gNowMs = 91000;
    serviceUpstreamWiFi("fsd-esp32");
    assert(WiFi.begins.size() == 4);
    assert(gWifiBridgeCfg.connecting);
    const int discBefore = WiFi.disconnectCalls;
    gNowMs = 91000 + UPSTREAM_CONNECT_TIMEOUT_MS;
    serviceUpstreamWiFi("fsd-esp32");
    assert(!gWifiBridgeCfg.connecting);
    assert(WiFi.disconnectCalls == discBefore + 1);

    // 7. Backoff math checks.
    assert(wifiBridgeComputeBackoffMs(0, false, false) == 3000);
    assert(wifiBridgeComputeBackoffMs(1, false, false) == 8000);
    assert(wifiBridgeComputeBackoffMs(1, true, false) == 60000);
    assert(wifiBridgeComputeBackoffMs(0, false, true) == 60000);
    assert(wifiBridgeComputeBackoffMs(99, false, false) == 300000);

    std::puts("Wi-Fi bridge state machine: all checks passed");
    return 0;
}
'''


def main() -> None:
    check_static_contracts()
    signatures = [
        "static inline void wifiBridgeCopyStr(",
        "static inline bool wifiBridgeIsReservedSSID(",
        "static inline void wifiBridgeClearNetwork(",
        "static inline int wifiBridgeFindNetwork(",
        "static inline bool wifiBridgeAddOrUpdateNetwork(",
        "static inline bool wifiBridgeRemoveNetwork(",
        "static inline bool wifiBridgeHasCredentials(",
        "static inline bool wifiBridgeIsTransientDisconnect(",
        "static inline uint32_t wifiBridgeComputeBackoffMs(",
        "static inline void wifiBridgeOnEvent(",
        "static inline int wifiBridgeSelectNextNetworkIndex(",
        "static inline void wifiBridgeStartConnect(",
        "static inline void wifiBridgeRequestApply(",
        "static inline void wifiBridgeApplyConfig(",
        "static inline void serviceUpstreamWiFi(",
    ]
    extracted = "\n".join(extract_function(BRIDGE_SRC, s) for s in signatures)
    with tempfile.TemporaryDirectory(prefix="wifi-bridge-checks-") as tmp:
        src = Path(tmp) / "bridge.cpp"
        bin_path = Path(tmp) / "bridge"
        src.write_text(PREAMBLE + extracted + TESTS)
        subprocess.run(
            [os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
             "-fsanitize=address,undefined", "-fno-omit-frame-pointer", str(src), "-o", str(bin_path)],
            check=True,
        )
        subprocess.run([str(bin_path)], check=True)


if __name__ == "__main__":
    main()
