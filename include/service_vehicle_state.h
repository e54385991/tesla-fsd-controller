#pragma once

#include <cstdint>
#include "can_frame_types.h"

// Read-only observation using opendbc tesla_can.dbc, DI_torque2 (0x118,
// DLC 6). This is the older S/X layout, NOT Model 3's 8-byte DI_systemStatus.
// Sources and the remaining Raven validation limits: docs/raven-service-mode.md.
struct ServiceVehicleSnapshot {
    bool seen = false;
    bool fresh = false;
    bool gearValid = false;
    bool speedValid = false;
    uint8_t gear = 0;
    float speedKph = 0;
    uint32_t ageMs = 0;
    uint32_t frameCount = 0;
    CanFrame frame;
    const char* reason = "no_signal";

    bool parkedKnown() const { return gearValid && speedValid; }
    bool parked() const {
        return parkedKnown() && gear == 1 && speedKph >= -0.1f && speedKph <= 0.1f;
    }
};

class ServiceVehicleState {
public:
    static constexpr uint32_t kMaxAgeMs = 1000;

    void onFrame(const CanFrame& frame, uint32_t now, uint32_t session) {
        if (frame.id != 0x118) return;
        frame_ = frame;
        seen_ = true;
        receivedMs_ = now;
        session_ = session;
        ++frameCount_;
    }

    ServiceVehicleSnapshot snapshot(uint32_t now, uint32_t session, bool canAvailable) const {
        ServiceVehicleSnapshot s;
        s.seen = seen_;
        s.frame = frame_;
        s.frameCount = frameCount_;
        if (!seen_) return s;
        s.ageMs = now - receivedMs_;  // unsigned subtraction also handles millis() wrap
        if (!canAvailable) { s.reason = "can_unavailable"; return s; }
        if (session != session_) { s.reason = "can_session_changed"; return s; }
        if (s.ageMs > kMaxAgeMs) { s.reason = "stale_signal"; return s; }
        s.fresh = true;
        // Do not try both layouts and choose whichever happens to say Park.
        if (frame_.dlc != 6) { s.reason = "unsupported_frame_layout"; return s; }
        // Tesla additive checksum: address bytes + payload excluding checksum.
        // See BogGyver/panda board/safety/safety_tesla.h tesla_compute_checksum.
        uint8_t checksum = 0x18 + 0x01;
        for (uint8_t i = 0; i < 5; ++i) checksum += frame_.data[i];
        if (checksum != frame_.data[5]) { s.reason = "checksum_mismatch"; return s; }
        s.gear = (frame_.data[1] >> 4) & 0x07;  // DI_gear: bit 12, length 3
        s.gearValid = s.gear >= 1 && s.gear <= 4;
        const uint16_t speedRaw = frame_.data[2] | ((frame_.data[3] & 0x0F) << 8);
        s.speedValid = speedRaw != 0x0FFF;
        // DI_vehicleSpeed: bit 16, length 12, (0.05, -25) MPH. Keep reverse
        // motion signed; clamping it to zero would falsely indicate standstill.
        if (s.speedValid) s.speedKph = (static_cast<int>(speedRaw) - 500) * 0.0804672f;
        s.reason = !s.gearValid ? "invalid_gear" : !s.speedValid ? "invalid_speed" : "observed";
        return s;
    }

private:
    bool seen_ = false;
    uint32_t receivedMs_ = 0;
    uint32_t session_ = 0;
    uint32_t frameCount_ = 0;
    CanFrame frame_;
};
