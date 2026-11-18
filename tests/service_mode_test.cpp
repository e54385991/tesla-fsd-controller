#include <cstdint>
static uint32_t nowMs = 1000;
uint32_t millis() { return nowMs; }
#include "mod_service_mode.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <thread>

static CanFrame parkedFrame() {
    // Golden vector for DBC DI_gear bit12=1, speed raw500 = 0 MPH.
    // The old Model 3 decoder would misread this frame's gear as SNA (7).
    CanFrame frame; frame.id = 0x118; frame.dlc = 6;
    const uint8_t bytes[] = {0x00, 0x10, 0xF4, 0x01, 0x00, 0x1E};
    std::memcpy(frame.data, bytes, 6);
    return frame;
}

static void fixChecksum(CanFrame& frame) {
    uint8_t checksum = 0x19;
    for (unsigned i = 0; i < 5; ++i) checksum += frame.data[i];
    frame.data[5] = checksum;
}

static void testVehicleObservation() {
    ServiceVehicleState state;
    assert(!state.snapshot(1000, 1, true).parkedKnown());
    assert(std::strcmp(state.snapshot(1000, 1, true).reason, "no_signal") == 0);
    auto frame = parkedFrame();
    state.onFrame(frame, 1000, 1);
    auto s = state.snapshot(1000, 1, true);
    assert(s.fresh && s.gear == 1 && s.speedValid && s.speedKph == 0 && s.parked());
    char json[896];
    assert(serviceModeFormatStatus(json, sizeof(json), s, false));
    assert(std::strstr(json, "\"gear\":\"P\""));
    assert(std::strstr(json, "\"frameHex\":\"00 10 F4 01 00 1E\""));
    assert(std::strstr(json, "\"entrySupported\":false"));
    assert(std::strstr(json, "\"telemetryVerified\":false"));
    assert(!serviceModeFormatStatus(json, 8, s, false));

    assert(state.snapshot(2000, 1, true).parked());
    assert(!state.snapshot(2001, 1, true).parkedKnown());
    assert(!state.snapshot(1001, 2, true).parkedKnown());
    assert(!state.snapshot(1001, 1, false).parkedKnown());
    // millis() wrap must neither expire a fresh sample nor revive an old one.
    state.onFrame(frame, UINT32_MAX - 100, 1);
    assert(state.snapshot(100, 1, true).parked());
    assert(!state.snapshot(1000, 1, true).parkedKnown());

    for (uint8_t gear = 0; gear < 8; ++gear) {
        frame = parkedFrame(); frame.data[1] = gear << 4; fixChecksum(frame);
        state.onFrame(frame, 1000, 1); s = state.snapshot(1000, 1, true);
        assert(s.gearValid == (gear >= 1 && gear <= 4));
        assert(s.parked() == (gear == 1));
    }
    // Forward AND reverse motion must prevent a standstill observation.
    for (int delta : {-10, 10, 1000}) {
        frame = parkedFrame(); const uint16_t raw = 500 + delta;
        frame.data[2] = raw & 0xFF; frame.data[3] = raw >> 8; fixChecksum(frame);
        state.onFrame(frame, 1000, 1); s = state.snapshot(1000, 1, true);
        assert(s.speedValid && !s.parked());
        assert(std::fabs(s.speedKph - delta * 0.0804672f) < 0.0001f);
    }
    frame = parkedFrame(); frame.data[2] = 0xFF; frame.data[3] = 0x0F; fixChecksum(frame);
    state.onFrame(frame, 1000, 1); s = state.snapshot(1000, 1, true);
    assert(s.gearValid && !s.speedValid && !s.parkedKnown());

    for (uint8_t dlc = 0; dlc <= 9; ++dlc) {
        if (dlc == 6) continue;
        frame = parkedFrame(); frame.dlc = dlc;
        state.onFrame(frame, 1000, 1); s = state.snapshot(1000, 1, true);
        assert(!s.parkedKnown());
        assert(std::strcmp(s.reason, "unsupported_frame_layout") == 0);
        assert(serviceModeFormatStatus(json, sizeof(json), s, false));
    }
    frame = parkedFrame(); frame.data[5] ^= 1;
    state.onFrame(frame, 1000, 1); s = state.snapshot(1000, 1, true);
    assert(!s.gearValid && !s.speedValid);
    assert(std::strcmp(s.reason, "checksum_mismatch") == 0);
    frame = parkedFrame(); frame.id = 0x257; state.onFrame(frame, 1001, 1);
    assert(std::strcmp(state.snapshot(1001, 1, true).reason, "checksum_mismatch") == 0);

    // The production wrappers must publish a consistent frame + decoded state
    // across the web and CAN cores, not independently sampled volatile fields.
    auto p = parkedFrame(), r = parkedFrame();
    r.data[1] = 0x20; r.data[2] = 0xEA; fixChecksum(r);
    serviceModeOnFrame(p, nowMs, 3);
    std::thread writer([&]() {
        for (unsigned i = 0; i < 10000; ++i) serviceModeOnFrame(i & 1 ? p : r, nowMs, 3);
    });
    for (unsigned i = 0; i < 10000; ++i) {
        s = serviceModeVehicleSnapshot(3, true);
        assert(s.gear == 1 || s.gear == 2);
        assert(s.parked() == (s.gear == 1));
        assert(s.frame.data[1] == s.gear << 4);
        assert(s.speedKph == (s.gear == 1 ? 0 : -10 * 0.0804672f));
    }
    writer.join();
}

int main() {
    testVehicleObservation();
    for (const char* version : {"2026.2", "2026.2.1", "2026.2.1.3"})
        assert(serviceModeFirmwareMatches(version));
    for (const char* version : {"", "2026", "2026.20", "2026.26.1", "2026.2.",
            "2026.2..1", "2026.2.beta", "2026.2.1x", "2026.2\n", " 2026.2", "2026.2.1.",
            "2026.2.123456789012345678901234567890"})
        assert(!serviceModeFirmwareMatches(version));
    assert(!serviceModeFirmwareMatches(nullptr));

    auto check = [](const char* action, const char* confirmation, bool safe) {
        return serviceModeCheckRequest(action, "model_x_raven", "2019", "2026.2", confirmation, safe);
    };
    // A correctly confirmed request must still never be reported as executed.
    for (const char* action : {"enter", "exit"}) {
        const char* confirmation = action[1] == 'n' ? "ENTER_SERVICE_MODE" : "EXIT_SERVICE_MODE";
        auto result = check(action, confirmation, false);
        assert(result.status == 501);
        assert(std::strcmp(result.error, "protocol_unverified") == 0);
        assert(check(action, confirmation, true).status == 409);
        assert(check(action, nullptr, false).status == 400);
        assert(check(action, "", false).status == 400);
    }
    assert(check("enter", "EXIT_SERVICE_MODE", false).status == 400);
    assert(check("exit", "ENTER_SERVICE_MODE", false).status == 400);
    assert(check(nullptr, "ENTER_SERVICE_MODE", false).status == 400);
    assert(check("toggle", "ENTER_SERVICE_MODE", false).status == 400);
    assert(serviceModeCheckRequest("enter", "model_3", "2019", "2026.2", "ENTER_SERVICE_MODE", false).status == 422);
    assert(serviceModeCheckRequest("enter", "model_x_raven", "2020", "2026.2", "ENTER_SERVICE_MODE", false).status == 422);
    assert(serviceModeCheckRequest("enter", "model_x_raven", "2019", "2026.20", "ENTER_SERVICE_MODE", false).status == 422);
    assert(serviceModeCheckRequest("enter", nullptr, nullptr, nullptr, "ENTER_SERVICE_MODE", false).status == 422);
    std::puts("Service Mode: S/X decoding, validity, freshness, recovery, snapshots and request checks passed");
}
