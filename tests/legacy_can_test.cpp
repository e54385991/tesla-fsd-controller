#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <thread>
#include <utility>
#include <vector>

static uint32_t nowMs = 1000;
uint32_t millis() { return nowMs; }
static volatile uint32_t bt_firstFsdMod = 0;
static void btMark(volatile uint32_t& value) { if (value == 0) value = millis(); }
static void addDiagLog(uint32_t, const char*) {}
#include "fsd_settings_update.h"
#include "mod_fsd.h"
#include "mod_lighting.h"
#include "mod_gps_speed.h"
#include "drivers/twai_driver.h"

FSDConfig cfg;
struct MockDriver : CanDriver {
    bool succeeds = true;
    uint32_t session = 0;
    std::vector<CanFrame> sent;
    bool init() override { return true; }
    bool send(const CanFrame& frame) override {
        // These single-threaded tests also guard against holding the config
        // critical section across a potentially blocking driver call.
        assert(gCanConfigMux.try_lock()); gCanConfigMux.unlock();
        sent.push_back(frame); return succeeds;
    }
    bool read(CanFrame&) override { return false; }
    uint32_t recoveryGeneration() const override { return session; }
    void setFilters(const uint32_t*, uint8_t) override {}
};

static void reset() {
    cfg = FSDConfig{}; gCanConfigGeneration = 0; gFsdRuntime = FsdRuntimeState{};
    nowMs = 1000; bt_firstFsdMod = 0; resetStalkGestureState(); fakeTwai::reset();
}
static size_t settingIndex(const char* key) {
    for (size_t i = 0; i < kFSDSettingCount; ++i)
        if (std::strcmp(key, kFSDSettingSpecs[i].key) == 0) return i;
    assert(false); return 0;
}
static FSDSettingsResult configure(std::initializer_list<std::pair<const char*, const char*>> values) {
    FSDSettingsUpdate update;
    for (const auto& value : values) assert(update.parse(settingIndex(value.first), value.second));
    return applyFSDSettingsUpdate(update);
}
static void legacy(bool enabled = true) {
    assert(!configure({{"hwMode", "0"}, {"fsdEnable", enabled ? "1" : "0"}}).error);
}
static CanFrame sample(uint32_t id, uint8_t mux = 0, bool selected = true) {
    CanFrame frame; frame.id = id;
    const uint8_t data[] = {0xA8, 0xD3, 0xFF, 0x92, 0xFF, 0xCB, 0xFF, 0xA5};
    std::memcpy(frame.data, data, 8);
    frame.data[0] = (frame.data[0] & 0xF8) | mux;
    if (!selected) frame.data[4] &= ~0x40;
    return frame;
}
static void receive(CanFrame frame, CanDriver& driver) {
    const auto control = prepareFsdControl(driver);
    if (frame.id == 760) handleGpsVehicleSpeed(frame);
    dispatchFsdMessage(frame, driver, control);
}
static void equal(const CanFrame& actual, const CanFrame& expected) {
    assert(actual.id == expected.id && actual.dlc == expected.dlc);
    assert(std::memcmp(actual.data, expected.data, 8) == 0);
}

static void test_defaults_and_atomic_updates() {
    reset();
    assert(cfg.legacyOffset == 0 && cfg.legacyByte7Enable && cfg.legacyByte7Value == 15);
    assert(cfg.removeVisionSpeedLimit);
    auto result = configure({{"hwMode", "0"}, {"speedProfile", "4"}, {"fsdEnable", "1"}});
    assert(result.error && !result.changed);
    assert(cfg.hwMode == 2 && !cfg.fsdEnable && cfg.speedProfile == 1 && gCanConfigGeneration == 0);
    cfg.rxCount = 123; cfg.gpsSpeedCount = 456;
    result = configure({{"hwMode", "0"}, {"fsdEnable", "1"}, {"legacyByte7Enable", "0"}, {"legacyByte7Value", "3"}});
    assert(!result.error && result.changed && cfg.legacyByte7Value == 3 && !cfg.legacyByte7Enable);
    assert(gCanConfigGeneration == 1 && cfg.rxCount == 123 && cfg.gpsSpeedCount == 456);
    assert(!configure({{"legacyByte7Value", "3"}}).changed && gCanConfigGeneration == 1);
    assert(configure({{"legacyOffset", "33"}}).changed && gCanConfigGeneration == 1);
    assert(configure({{"forceActivate", "1"}}).changed && gCanConfigGeneration == 2);
    configure({{"hwMode", "2"}, {"speedProfile", "4"}});
    configure({{"hwMode", "0"}}); assert(cfg.speedProfile == 2);
}

static void test_strict_validation_and_bounds() {
    reset();
    for (const char* key : {"legacyOffset", "legacyByte7Value", "legacyByte7Enable", "removeVSL", "overrideSL", "fsdEnable", "forceActivate"}) {
        for (const char* bad : {"", "abc", "-1", "+1", " 1", "1 ", "1x", "1.0", "1\n", "4294967296", "99999999999999999999"}) {
            FSDSettingsUpdate update;
            assert(!update.parse(settingIndex(key), bad));
        }
    }
    FSDSettingsUpdate update;
    assert(!update.parse(settingIndex("legacyOffset"), "34"));
    assert(!update.parse(settingIndex("legacyByte7Value"), "16"));
    assert(!update.parse(settingIndex("legacyByte7Enable"), "2"));
    assert(!update.parse(kFSDSettingCount, "1"));
    assert(!update.parse(0, nullptr));
    assert(!update.parse(settingIndex("legacyOffset"), "1\0x", 3));
    legacy();
    for (const char* value : {"0", "33"}) assert(!configure({{"legacyOffset", value}}).error);
    for (const char* value : {"0", "15"}) assert(!configure({{"legacyByte7Value", value}}).error);
    configure({{"hw3SlewRate", "99"}, {"hw3WireEncoding", "99"}});
    assert(cfg.hw3SlewRatePctPerSec == 25 && cfg.hw3WireEncoding == 1);
}

static void test_master_switch() {
    reset(); legacy(false); MockDriver driver;
    configure({{"legacyOffset", "33"}, {"overrideSL", "1"}, {"forceActivate", "1"}});
    for (uint32_t id : {760u, 1080u, 1006u}) receive(sample(id), driver);
    receive(sample(1006, 1), driver);
    assert(driver.sent.empty() && !cfg.fsdTriggered && cfg.modifiedCount == 0);
    configure({{"fsdEnable", "1"}});
    receive(sample(760), driver); receive(sample(1080), driver);
    assert(driver.sent.size() == 2);  // These controls do not require a mux-0 latch.
    configure({{"fsdEnable", "0"}});
    receive(sample(760), driver); receive(sample(1080), driver); receive(sample(1006, 1), driver);
    assert(driver.sent.size() == 2 && !cfg.fsdTriggered);
}

static void test_offset_masks_and_units() {
    reset(); legacy(); MockDriver driver;
    for (unsigned offset = 1; offset <= 33; ++offset) {
        const auto text = std::to_string(offset);
        configure({{"legacyOffset", text.c_str()}});
        for (unsigned flags = 0; flags < 4; ++flags) {
            auto input = sample(760); input.data[5] = (flags << 6) | 17;
            auto expected = input; expected.data[5] = (flags << 6) | (offset + 30);
            const size_t sentBefore = driver.sent.size(); receive(input, driver);
            assert(driver.sent.size() == sentBefore + 1); equal(driver.sent.back(), expected);
            assert(cfg.gpsUserOffsetRaw == 17 && cfg.gpsUserOffsetUnits == (flags >> 1));
            assert(cfg.gpsMapLimitUnits == (flags & 1));
        }
    }
    configure({{"legacyOffset", "0"}}); const size_t count = driver.sent.size();
    receive(sample(760), driver); assert(driver.sent.size() == count);
}

static void test_slider_masks() {
    reset(); legacy(); MockDriver driver;
    receive(sample(1080), driver); assert(driver.sent.empty());
    configure({{"overrideSL", "1"}});
    for (unsigned byte = 0; byte < 256; ++byte) {
        auto input = sample(1080); input.data[7] = byte;
        auto expected = input; expected.data[7] = (byte & 0x80) | 100;
        receive(input, driver); equal(driver.sent.back(), expected);
    }
    assert(driver.sent.size() == 256 && cfg.modifiedCount == 256);
}

static void test_mux0_masks_and_byte7() {
    reset(); legacy(); MockDriver driver;
    for (unsigned value = 0; value < 16; ++value) {
        const auto text = std::to_string(value); configure({{"legacyByte7Value", text.c_str()}});
        for (const char* profile : {"0", "1", "2"}) {
            configure({{"speedProfile", profile}, {"profileMode", "0"}});
            auto input = sample(1006); auto expected = input;
            expected.data[5] |= 0x40; expected.data[6] = (expected.data[6] & 0xF9) | ((profile[0] - '0') << 1);
            expected.data[7] = (expected.data[7] & 0xF0) | value;
            const size_t count = driver.sent.size(); receive(input, driver);
            assert(driver.sent.size() == count + 1); equal(driver.sent.back(), expected);
        }
    }
    configure({{"legacyByte7Enable", "0"}});
    auto input = sample(1006); receive(input, driver);
    assert(driver.sent.back().data[7] == input.data[7]);
}

static void test_mux_latch_transitions() {
    reset(); legacy(); MockDriver driver;
    for (uint8_t mux = 2; mux < 8; ++mux) receive(sample(1006, mux), driver);
    assert(driver.sent.empty() && !cfg.fsdTriggered);
    receive(sample(1006, 1), driver); assert(driver.sent.empty());
    receive(sample(1006, 0, false), driver); assert(driver.sent.empty());
    receive(sample(1006), driver); assert(cfg.fsdTriggered && driver.sent.size() == 1);
    auto nag = sample(1006, 1); auto expected = nag; expected.data[2] &= ~8; expected.data[6] &= ~1;
    receive(nag, driver); equal(driver.sent.back(), expected);
    assert(cfg.modifiedCount == 1 && driver.sent.size() == 2);
    configure({{"removeVSL", "0"}}); expected = nag; expected.data[2] &= ~8;
    receive(nag, driver); equal(driver.sent.back(), expected);
    receive(sample(1006, 0, false), driver);
    const auto count = driver.sent.size(); receive(nag, driver);
    assert(driver.sent.size() == count && !cfg.fsdTriggered);
    configure({{"forceActivate", "1"}});
    receive(nag, driver); assert(driver.sent.size() == count);
    receive(sample(1006, 0, false), driver); assert(cfg.fsdTriggered);
    configure({{"forceActivate", "0"}});
    const auto count2 = driver.sent.size(); receive(nag, driver); assert(driver.sent.size() == count2);
}

static void test_disable_reenable_and_mode_roundtrip() {
    reset(); legacy(); MockDriver driver; receive(sample(1006), driver);
    configure({{"fsdEnable", "0"}}); configure({{"fsdEnable", "1"}});
    receive(sample(1006, 1), driver); assert(driver.sent.size() == 1 && !cfg.fsdTriggered);
    receive(sample(1006), driver); assert(driver.sent.size() == 2);
    configure({{"hwMode", "2"}}); receive(sample(1021), driver);
    configure({{"hwMode", "0"}}); receive(sample(1006, 1), driver);
    assert(driver.sent.size() == 3 && !cfg.fsdTriggered);
    receive(sample(1006), driver);
    configure({{"hwMode", "1"}}); configure({{"hwMode", "0"}});
    const auto count = driver.sent.size(); receive(sample(1006, 1), driver);
    assert(driver.sent.size() == count); // Even a roundtrip between CAN frames is observed.
}

static void test_recovery_and_send_failures() {
    reset(); legacy(); MockDriver driver; driver.succeeds = false;
    receive(sample(1006), driver);
    assert(cfg.errorCount == 1 && cfg.modifiedCount == 0 && bt_firstFsdMod == 0);
    receive(sample(1006, 1), driver); assert(cfg.errorCount == 2);
    driver.succeeds = true; nowMs = 2345; receive(sample(1006), driver);
    assert(bt_firstFsdMod == 2345 && cfg.modifiedCount == 1);
    ++driver.session; const auto count = driver.sent.size();
    receive(sample(1006, 1), driver); assert(driver.sent.size() == count && !cfg.fsdTriggered);
    receive(sample(1006), driver); assert(driver.sent.size() == count + 1);
    configure({{"legacyOffset", "1"}, {"overrideSL", "1"}}); driver.succeeds = false;
    receive(sample(760), driver); receive(sample(1080), driver); assert(cfg.errorCount == 4);
}

static void test_short_frames_and_stalk_values() {
    reset(); legacy(); MockDriver driver; configure({{"legacyOffset", "1"}, {"overrideSL", "1"}});
    for (uint32_t id : {69u, 760u, 1006u, 1080u}) {
        for (unsigned length = 0; length <= 9; ++length) {
            if (length == 8) continue;
            auto input = sample(id); input.dlc = length; receive(input, driver);
        }
    }
    assert(driver.sent.empty() && !cfg.fsdTriggered && !cfg.gpsSpeedSeen && cfg.speedProfile == 1);
    for (unsigned value = 0; value <= 255; ++value) {
        cfg.speedProfile = 1; auto input = sample(69); input.data[1] = value; receive(input, driver);
        unsigned pos = value >> 5;
        assert(cfg.speedProfile == (value > 200 ? 1 : pos <= 1 ? 2 : pos == 2 ? 1 : 0));
    }
    configure({{"profileMode", "0"}, {"speedProfile", "1"}});
    auto input = sample(69); input.data[1] = 0; receive(input, driver); assert(cfg.speedProfile == 1);
    configure({{"profileMode", "1"}}); const auto old = captureCanControl();
    configure({{"profileMode", "0"}}); publishStalkProfile(2, old); assert(cfg.speedProfile == 1);
}

static void test_other_modes_and_filtering() {
    reset(); MockDriver driver;
    for (uint8_t mode = 0; mode < 3; ++mode) {
        const auto filter = getFilterIds(mode); assert(filter.count == (mode == 0 ? 4u : mode == 1 ? 2u : 3u));
        for (uint32_t id = 0; id < 2048; ++id) {
            const bool expected = mode == 0 ? (id == 69 || id == 760 || id == 1006 || id == 1080)
                : mode == 1 ? (id == 1016 || id == 1021) : (id == 921 || id == 1016 || id == 1021);
            assert(isFilteredId(id, mode) == expected);
        }
    }
    assert(!isFilteredId(1006, 255));
    configure({{"hwMode", "1"}, {"fsdEnable", "1"}, {"hw3AutoSpeed", "0"}});
    auto hw3 = sample(1021); auto expected = hw3; expected.data[5] |= 0x40;
    expected.data[6] = (expected.data[6] & 0xF9) | 2;
    receive(hw3, driver); equal(driver.sent.back(), expected);
    receive(sample(1021, 1), driver); assert(driver.sent.size() == 2 && cfg.modifiedCount == 1);
    receive(sample(1021, 2), driver); assert(driver.sent.size() == 3);
    configure({{"hwMode", "2"}, {"speedProfile", "4"}, {"hw4Offset", "21"}, {"tlsscBypass", "1"}});
    auto hw4 = sample(1021); expected = hw4;
    expected.data[5] |= 0x40; expected.data[4] |= 0x40; expected.data[7] |= 0x18;
    receive(hw4, driver); equal(driver.sent.back(), expected);
    auto mux2 = sample(1021, 2); expected = mux2;
    expected.data[7] = (expected.data[7] & 0x8F) | 0x40;
    expected.data[1] = (expected.data[1] & 0xC0) | 21;
    receive(mux2, driver); equal(driver.sent.back(), expected);
    const auto count = driver.sent.size(); receive(sample(1006), driver); assert(driver.sent.size() == count);
}

static void test_concurrent_mode_snapshots() {
    reset(); std::atomic<bool> running{true};
    std::thread writer([&]() {
        for (unsigned n = 0; n < 30000; ++n) {
            configure({{"hwMode", "0"}, {"legacyOffset", "1"}, {"legacyByte7Value", "2"}});
            configure({{"hwMode", "1"}, {"legacyOffset", "3"}, {"legacyByte7Value", "4"}});
            configure({{"hwMode", "2"}, {"legacyOffset", "5"}, {"legacyByte7Value", "6"}});
        }
        running = false;
    });
    do {
        const auto control = captureCanControl(); const auto& s = control.settings;
        if (control.generation > 0) {
            assert(s.legacyOffset == s.hwMode * 2 + 1 && s.legacyByte7Value == s.hwMode * 2 + 2);
        }
        assert(!isFilteredId(0x7A0, s.hwMode));
    } while (running.load());
    writer.join();
}

static void test_twai_formats_and_recovery() {
    reset(); TWAIDriver driver; assert(driver.init()); assert(driver.recoveryGeneration() == 1);
    for (int type = 0; type < 5; ++type) {
        twai_message_t invalid; invalid.identifier = 1006; invalid.data_length_code = 8;
        if (type == 0) invalid.extd = true;
        if (type == 1) invalid.rtr = true;
        if (type == 2) invalid.data_length_code = 12;
        if (type == 3) invalid.dlc_non_comp = true;
        if (type == 4) invalid.identifier = 0x800;
        fakeTwai::incoming.push_back(invalid);
        twai_message_t valid; valid.identifier = 760; valid.data_length_code = 2; valid.data[0] = 17;
        fakeTwai::incoming.push_back(valid);
        CanFrame actual; assert(driver.read(actual)); assert(actual.id == 760 && actual.dlc == 2 && actual.data[0] == 17);
        for (unsigned i = 2; i < 8; ++i) assert(actual.data[i] == 0);
        fakeTwai::incoming.push_back(invalid); actual.id = 0x123;
        assert(!driver.read(actual) && actual.id == 0x123);
    }
    CanFrame invalid; invalid.dlc = 9; assert(!driver.send(invalid));
    invalid.dlc = 8; invalid.id = 0x800; assert(!driver.send(invalid));
    auto valid = sample(1006); assert(driver.send(valid)); assert(fakeTwai::outgoing.size() == 1);
    legacy(); receive(sample(1006), driver); assert(cfg.fsdTriggered);
    fakeTwai::state = TWAI_STATE_BUS_OFF; CanFrame next;
    assert(!driver.read(next)); assert(driver.recoveryGeneration() == 2);
    receive(sample(1006, 1), driver); assert(!cfg.fsdTriggered);
    receive(sample(1006), driver); assert(cfg.fsdTriggered);
    nowMs += 1000; fakeTwai::state = TWAI_STATE_BUS_OFF; fakeTwai::transmitResult = ESP_FAIL;
    assert(!driver.send(valid) && driver.recoveryGeneration() == 3);
    receive(sample(1006, 1), driver); assert(!cfg.fsdTriggered);
    // Failed recovery and failed start do not leak the driver installation.
    nowMs += 1000; fakeTwai::state = TWAI_STATE_BUS_OFF; fakeTwai::startResult = ESP_FAIL;
    const auto uninstalls = fakeTwai::uninstalls;
    assert(!driver.read(next) && !driver.isDriverOK() && fakeTwai::uninstalls == uninstalls + 2);
    fakeTwai::startResult = ESP_OK; nowMs += 10000;
    assert(!driver.read(next) && driver.isDriverOK() && driver.recoveryGeneration() == 4);
}

static void tap(MockDriver& driver) {
    CanFrame frame; frame.id = 585; frame.dlc = 3; frame.data[1] = 0xD7; frame.data[2] = 0xAB;
    nowMs += 100; handleSCCMStalk(frame, driver);
    frame.data[1] = 0xC7; handleSCCMStalk(frame, driver);
}
static void test_lighting_guard_and_masks() {
    reset(); MockDriver driver;
    for (int i = 0; i < 4; ++i) tap(driver);
    assert(!cfg.highBeamForce && driver.sent.empty());
    cfg.adaptiveLighting = true; tap(driver); tap(driver);
    CanFrame settings; settings.id = 659; handleDASSettings(settings);
    assert(hbTapCount == 0 && !cfg.highBeamForce);
    cfg.adaptiveLighting = true; tap(driver); assert(!cfg.highBeamForce);
    tap(driver); tap(driver); assert(cfg.highBeamForce && driver.sent.size() == 2);
    for (const auto& frame : driver.sent) {
        assert(frame.dlc == 3 && (frame.data[1] & 0xCF) == 0xC7 && (frame.data[1] & 0x30) == 0x20);
        assert(frame.data[2] == 0xAB && frame.data[0] == static_cast<uint8_t>(0x4B + frame.data[1] + frame.data[2]));
    }
    tap(driver); assert(!cfg.highBeamForce && driver.sent.size() == 2);
    cfg.highBeamForce = true; settings.data[2] = 0; handleDASSettings(settings);
    assert(!cfg.highBeamForce && hbTapCount == 0);
}

int main() {
    unsigned count = 0;
#define RUN(test) do { test(); ++count; std::puts(#test " passed"); } while (0)
    RUN(test_defaults_and_atomic_updates);
    RUN(test_strict_validation_and_bounds);
    RUN(test_master_switch);
    RUN(test_offset_masks_and_units);
    RUN(test_slider_masks);
    RUN(test_mux0_masks_and_byte7);
    RUN(test_mux_latch_transitions);
    RUN(test_disable_reenable_and_mode_roundtrip);
    RUN(test_recovery_and_send_failures);
    RUN(test_short_frames_and_stalk_values);
    RUN(test_other_modes_and_filtering);
    RUN(test_concurrent_mode_snapshots);
    RUN(test_twai_formats_and_recovery);
    RUN(test_lighting_guard_and_masks);
    std::printf("LEGACY/CAN: %u regression groups passed\n", count);
}
