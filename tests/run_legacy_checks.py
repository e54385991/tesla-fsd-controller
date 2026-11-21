#!/usr/bin/env python3
"""Run sanitized CAN tests and the real API/NVS/status code with host adapters.

The extracted functions are compiled unchanged; generated sources/binaries
live in a temporary directory. No hardware, PlatformIO or third-party Python
packages are needed. Run from any directory: python3 tests/run_legacy_checks.py
"""
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/main.cpp").read_text()


def between(text, start, end):
    return text.split(start, 1)[1].split(end, 1)[0]


PREAMBLE = r'''
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <vector>
uint32_t millis() { return 100000; }
volatile uint32_t bt_firstFsdMod = 0;
void btMark(volatile uint32_t& n) { if (!n) n = millis(); }
#include "fsd_settings_update.h"
#include "mod_fsd.h"
FSDConfig cfg;
struct String : std::string {
    using std::string::string;
    String(const std::string& text) : std::string(text) {}
};
static bool authorized = true;
struct Param { String text; const String& value() { return text; } };
struct AsyncWebServerRequest {
    std::map<std::string, Param> params;
    int status = 0;
    std::string body;
    bool hasParam(const char* key) { return params.count(key); }
    Param* getParam(const char* key) { return &params.at(key); }
    void send(int code, const char*, const String& value) { status = code; body = value; }
};
bool checkToken(AsyncWebServerRequest*) { return authorized; }
size_t strlcpy(char* dst, const char* src, size_t cap) {
    size_t n = std::strlen(src);
    if (cap) { size_t count = std::min(n, cap - 1); std::memcpy(dst, src, count); dst[count] = 0; }
    return n;
}
static std::map<std::string, std::string> nvs;
static unsigned writes = 0;
struct Preferences {
    std::string ns;
    bool begin(const char* name, bool = false) { ns = name; return true; }
    void end() {}
    std::string key(const char* name) { return ns + "/" + name; }
    bool isKey(const char* name) { return nvs.count(key(name)); }
    bool remove(const char* name) { return nvs.erase(key(name)); }
    uint8_t getUChar(const char* name, uint8_t fallback = 0) {
        assert(gCanConfigMux.try_lock()); gCanConfigMux.unlock();
        return isKey(name) ? std::stoul(nvs[key(name)]) : fallback;
    }
    bool getBool(const char* name, bool fallback = false) { return getUChar(name, fallback) != 0; }
    String getString(const char* name, const char* fallback = "") { return isKey(name) ? nvs[key(name)] : fallback; }
    size_t getBytes(const char* name, void* dst, size_t size) {
        if (!isKey(name)) return 0;
        const auto& data = nvs[key(name)]; size_t copied = std::min(size, data.size());
        std::memcpy(dst, data.data(), copied); return copied;
    }
    void putUChar(const char* name, uint8_t value) {
        assert(gCanConfigMux.try_lock()); gCanConfigMux.unlock();
        nvs[key(name)] = std::to_string(value); ++writes;
    }
    void putBool(const char* name, bool value) { putUChar(name, value); }
    void putString(const char* name, const char* text) { nvs[key(name)] = text; ++writes; }
    void putBytes(const char* name, const void* data, size_t size) {
        nvs[key(name)] = std::string(static_cast<const char*>(data), size); ++writes;
    }
};
static Preferences prefs;
static char apSSID[33] = "FSD-Controller", apPass[64] = "12345678", staSSID[33] = "", staPass[64] = "", storedPin[17] = "";
static bool safeModeActive = false, timeSynced = false;
static const int WL_CONNECTED = 3;
struct FakeIP { String toString() { return "255.255.255.255"; } };
struct FakeWiFi { int status() { return WL_CONNECTED; } FakeIP localIP() { return {}; } } WiFi;
struct FakeThermal { double currentC = 125.1, averageC = -99.9, peakC = 128.0; unsigned level = 2; } gThermalStatus;
const char* thermalStatusText() { return "protect"; }
bool thermalProtectActive() { return true; }
unsigned esp_get_free_heap_size() { return 4294967295u; }
unsigned telemSpeedRaw() { return 65535; }
unsigned telemGear() { return 7; }
int telemTorqueFront() { return -32768; }
int telemTorqueRear() { return -32768; }
bool telemBrake() { return true; }
#include "fsd_version.h"
'''

API_TESTS = r'''
AsyncWebServerRequest request(std::initializer_list<std::pair<const char*, const char*>> fields) {
    AsyncWebServerRequest req;
    for (const auto& field : fields) req.params[field.first].text = field.second;
    applyRequest(&req); return req;
}
template<class T> void maximize(volatile T& value) { value = std::numeric_limits<T>::max(); }
int main() {
    loadConfig();
    assert(cfg.legacyByte7Enable && cfg.legacyByte7Value == 15 && cfg.legacyOffset == 0);
    // Invalid late fields must preserve both RAM and the entire NVS namespace.
    const auto initialNvs = nvs;
    auto bad = request({{"fsdEnable", "1"}, {"legacyOffset", "34"}});
    assert(bad.status == 400 && !cfg.fsdEnable && nvs == initialNvs && writes == 0);
    for (const char* invalid : {"", "abc", "-1", "1x", "+1", " 1", "1 ", "1.1", "4294967296"}) {
        assert(request({{"fsdEnable", "1"}, {"legacyOffset", invalid}}).status == 400);
        assert(!cfg.fsdEnable && writes == 0 && nvs == initialNvs);
    }
    AsyncWebServerRequest nul;
    nul.params["fsdEnable"].text = "1";
    nul.params["legacyOffset"].text = String(std::string("1\0x", 3));
    applyRequest(&nul);
    assert(nul.status == 400 && !cfg.fsdEnable && writes == 0 && nvs == initialNvs);
    assert(request({{"hwMode", "0"}, {"speedProfile", "4"}, {"legacyOffset", "10"}}).status == 400);
    assert(cfg.hwMode == 2 && cfg.legacyOffset == 0 && writes == 0);
    assert(request({{"hw3CT0", "201"}, {"legacyByte7Value", "3"}}).status == 400);
    assert(cfg.legacyByte7Value == 15 && writes == 0);
    authorized = false;
    assert(request({{"legacyByte7Value", "3"}}).status == 403 && writes == 0);
    authorized = true;
    assert(request({{"hwMode", "0"}, {"fsdEnable", "1"}, {"legacyOffset", "33"},
                    {"legacyByte7Enable", "0"}, {"legacyByte7Value", "3"}}).status == 200);
    assert(cfg.legacyOffset == 33 && !cfg.legacyByte7Enable && cfg.legacyByte7Value == 3);
    assert(nvs["fsd/dg"] == "0" && nvs["fsd/dh"] == "3" && nvs["fsd/db"] == "33");
    const unsigned savedWrites = writes;
    assert(request({{"legacyByte7Enable", "0"}, {"legacyByte7Value", "3"}}).status == 200);
    assert(writes == savedWrites);
    cfg = FSDConfig{}; loadConfig();
    assert(cfg.legacyOffset == 33 && !cfg.legacyByte7Enable && cfg.legacyByte7Value == 3);
    // Old installations used dg/dh; corrupt stored bytes are bounded on load.
    nvs["fsd/dh"] = "255"; nvs["fsd/db"] = "255"; loadConfig();
    assert(cfg.legacyByte7Value == 15 && cfg.legacyOffset == 33);
    assert(request({{"legacyOffset", "0"}, {"legacyByte7Value", "0"}, {"legacyByte7Enable", "1"}}).status == 200);
    cfg = FSDConfig{}; loadConfig(); assert(cfg.legacyByte7Enable && cfg.legacyByte7Value == 0 && cfg.legacyOffset == 0);
    assert(request({{"legacyByte7Enable", "0"}, {"legacyByte7Value", "3"}}).status == 200);
    AsyncWebServerRequest status;
    statusRequest(&status); assert(status.status == 200);
    std::puts(("STATUS_JSON " + status.body).c_str());
    // Max-length escaped strings, counters and debug bytes exercise capacity.
    std::memset(apSSID, '"', 32); apSSID[32] = 0;
    std::memset(staSSID, '\\', 32); staSSID[32] = 0;
    std::memset(cfg.perfModel, '"', 32); cfg.perfModel[32] = 0;
    std::memset(cfg.carSwVer, '\\', 32); cfg.carSwVer[32] = 0;
    /* MAXIMIZE_RUNTIME */
    cfg.gpsUserOffsetUnits = 1; cfg.gpsMapLimitUnits = 1;
#ifdef DEBUG_MODE
    cfg.dbgFrameCaptured = true;
    for (auto& byte : cfg.dbgFrame) byte = 255;
#endif
    AsyncWebServerRequest maximum;
    statusRequest(&maximum); assert(maximum.status == 200);
    std::puts(("MAX_STATUS_JSON " + maximum.body).c_str());
    std::puts("API/NVS/status: transactional validation, persistence and capacity passed");
}
'''


def integration_source():
    keys = between(SOURCE, '#define NV_FSD_EN', '// ═══════════════════════════════════════════')
    keys = '#define NV_FSD_EN' + keys
    persistence = 'void loadConfig() {' + between(SOURCE, 'void loadConfig() {', '// ═══════════════════════════════════════════\n//  SPIFFS')
    api = between(SOURCE, 'server.on("/api/set", HTTP_GET, [](AsyncWebServerRequest* req) {', '\n    });')
    # The FSD-mock-only build registers its own compact /api/status first in
    # source order; anchor on the legacy body so the integration harness keeps
    # exercising the full CAN status payload.
    legacy_status = 'server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* req) {\n        const auto settings = captureCanControl().settings;'
    status_offset = SOURCE.index(legacy_status)
    status = '        const auto settings = captureCanControl().settings;' + between(
        SOURCE[status_offset:], legacy_status, '\n    });')
    escape = ('#include "json_escape.h"\n'
              'static uint32_t gBootMillis = 0;\n'
              'static inline uint32_t bootUptimeSeconds() { return (millis() - gBootMillis) / 1000; }\n')
    runtime = sorted(set(re.findall(r'cfg\.(\w+)', status)) - {'perfModel', 'carSwVer', 'dbgFrame', 'dbgFrameCaptured'})
    maximum = '\n'.join(f'    maximize(cfg.{field});' for field in runtime)
    return (PREAMBLE + keys + persistence + escape + '\nvoid applyRequest(AsyncWebServerRequest* req) {' + api +
            '\n}\nvoid statusRequest(AsyncWebServerRequest* req) {' + status + '\n}\n' + API_TESTS.replace('/* MAXIMIZE_RUNTIME */', maximum))


def main():
    compiler = os.environ.get('CXX', 'c++')
    flags = ['-std=c++11', '-Wall', '-Wextra', '-Wno-unused-parameter',
             '-Wno-unused-function', '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-pthread',
             '-I' + str(ROOT / 'tests/native_stubs'), '-I' + str(ROOT / 'include')]
    with tempfile.TemporaryDirectory(prefix='legacy-can-checks-') as directory:
        binary = str(Path(directory) / 'can')
        subprocess.run([compiler, *flags, str(ROOT / 'tests/legacy_can_test.cpp'), '-o', binary], check=True)
        subprocess.run([binary], check=True)
        for debug in (False, True):
            binary = str(Path(directory) / ('api-debug' if debug else 'api'))
            debug_flag = ['-DDEBUG_MODE'] if debug else []
            subprocess.run([compiler, *flags, *debug_flag, '-x', 'c++', '-', '-o', binary],
                           input=integration_source(), text=True, check=True)
            result = subprocess.run([binary], text=True, capture_output=True, check=True)
            for line in result.stdout.splitlines():
                if line.startswith(('STATUS_JSON ', 'MAX_STATUS_JSON ')):
                    tag, body = line.split(' ', 1)
                    payload = json.loads(body)
                    assert payload['legacyByte7Enable'] == 0 and payload['legacyByte7Value'] == 3
                    assert payload['legacyOffset'] == 0
                    if tag == 'MAX_STATUS_JSON':
                        assert payload['gps2F8UserUnits'] == 1 and payload['gps2F8MapUnits'] == 1
                        if debug:
                            assert payload['dbg']['bytes'] == ' '.join(['FF'] * 8)
                        print(f'Status JSON ({"debug" if debug else "release"}): {len(body.encode())}/3072 bytes')
                else:
                    print(line)


if __name__ == '__main__':
    main()
