#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include "can_control.h"
#include "service_vehicle_state.h"

// Service Mode here means the vehicle touchscreen mode, not a UDS diagnostic
// session. Public S/X telemetry is an observation, not a verified Raven control
// transport. Keep this module independent of CanDriver: a confirmed HTTP
// request must not turn into a guessed CAN write.
static ServiceVehicleState gServiceVehicleState;

inline void serviceModeOnFrame(const CanFrame& frame, uint32_t now, uint32_t session) {
    if (frame.id != 0x118) return;
    CanConfigGuard guard;
    gServiceVehicleState.onFrame(frame, now, session);
}

inline ServiceVehicleSnapshot serviceModeVehicleSnapshot(uint32_t session, bool canAvailable) {
    CanConfigGuard guard;
    // Sample time AFTER acquiring the lock. Otherwise a new CAN frame can have
    // a timestamp later than the reader's and appear billions of ms old.
    return gServiceVehicleState.snapshot(millis(), session, canAvailable);
}

inline bool serviceModeFormatStatus(char* body, size_t capacity,
        const ServiceVehicleSnapshot& s, bool safeMode) {
    char gear[8] = "null", gearRaw[8] = "null", speed[20] = "null", age[16] = "null";
    char frameHex[24] = "";
    if (s.gearValid) {
        std::snprintf(gear, sizeof(gear), "\"%c\"", "?PRND"[s.gear]);
        std::snprintf(gearRaw, sizeof(gearRaw), "%u", static_cast<unsigned>(s.gear));
    }
    if (s.speedValid) std::snprintf(speed, sizeof(speed), "%.3f", static_cast<double>(s.speedKph));
    if (s.seen) {
        std::snprintf(age, sizeof(age), "%lu", static_cast<unsigned long>(s.ageMs));
        const uint8_t count = s.frame.dlc > 8 ? 8 : s.frame.dlc;
        for (uint8_t i = 0; i < count; ++i)
            std::snprintf(frameHex + i * 3, sizeof(frameHex) - i * 3,
                "%02X%s", s.frame.data[i], i + 1 < count ? " " : "");
    }
    const int n = std::snprintf(body, capacity,
        "{\"targetVehicle\":\"model_x_raven\",\"targetYear\":2019,"
        "\"targetFirmware\":\"2026.2\",\"vehicleVerified\":false,"
        "\"protocolVerified\":false,\"entrySupported\":false,\"exitSupported\":false,"
        "\"state\":\"unknown\",\"gear\":%s,\"gearRaw\":%s,\"speedKph\":%s,\"parked\":%s,"
        "\"telemetryVerified\":false,\"signalSource\":\"legacy_sx_di_torque2\","
        "\"signalStatus\":\"%s\",\"signalFresh\":%s,\"signalAgeMs\":%s,"
        "\"frameCount\":%lu,\"frameDlc\":%u,\"frameHex\":\"%s\","
        "\"safeMode\":%s,\"error\":\"protocol_unverified\"}",
        gear, gearRaw, speed, s.parkedKnown() ? (s.parked() ? "true" : "false") : "null",
        s.reason, s.fresh ? "true" : "false", age,
        static_cast<unsigned long>(s.frameCount), s.seen ? s.frame.dlc : 0, frameHex,
        safeMode ? "true" : "false");
    return n >= 0 && static_cast<size_t>(n) < capacity;
}

struct ServiceModeDecision {
    uint16_t status;
    const char* error;
};

inline bool serviceModeFirmwareMatches(const char* version) {
    if (!version || std::strlen(version) > 32 ||
        std::strncmp(version, "2026.2", 6) != 0) return false;
    const char* p = version + 6;
    if (!*p) return true;
    // Accept patch versions of 2026.2, but not 2026.20/2026.26 or suffix text.
    while (*p) {
        if (*p++ != '.' || *p < '0' || *p > '9') return false;
        while (*p >= '0' && *p <= '9') ++p;
    }
    return true;
}

inline ServiceModeDecision serviceModeCheckRequest(
        const char* action, const char* vehicle, const char* year,
        const char* firmware, const char* confirmation, bool safeMode) {
    const bool enter = action && std::strcmp(action, "enter") == 0;
    const bool exit = action && std::strcmp(action, "exit") == 0;
    if (!enter && !exit) return {400, "invalid_action"};
    const char* expected = enter ? "ENTER_SERVICE_MODE" : "EXIT_SERVICE_MODE";
    if (!confirmation || std::strcmp(confirmation, expected) != 0)
        return {400, "confirmation_required"};
    if (!vehicle || std::strcmp(vehicle, "model_x_raven") != 0 ||
        !year || std::strcmp(year, "2019") != 0)
        return {422, "vehicle_out_of_scope"};
    if (!serviceModeFirmwareMatches(firmware))
        return {422, "firmware_out_of_scope"};
    if (safeMode) return {409, "device_safe_mode"};
    // Deliberately no success branch. Scope matching is a declaration from the
    // user, not proof of vehicle identity, fresh P/zero-speed data or a working
    // Service Mode transport. Do not substitute the Model 3 telemetry here.
    return {501, "protocol_unverified"};
}
