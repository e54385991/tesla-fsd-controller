#pragma once
#include "fsd_config.h"

#ifdef ARDUINO_ARCH_ESP32
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
static portMUX_TYPE gCanConfigMux = portMUX_INITIALIZER_UNLOCKED;
#else
#include <mutex>
static std::mutex gCanConfigMux;
#endif

extern FSDConfig cfg;

// Single-TU firmware: both cores use this one lock and generation. Never call
// a driver, allocate, perform I/O, or access a request while holding this lock.
class CanConfigGuard {
public:
    CanConfigGuard() {
#ifdef ARDUINO_ARCH_ESP32
        portENTER_CRITICAL(&gCanConfigMux);
#else
        gCanConfigMux.lock();
#endif
    }
    ~CanConfigGuard() {
#ifdef ARDUINO_ARCH_ESP32
        portEXIT_CRITICAL(&gCanConfigMux);
#else
        gCanConfigMux.unlock();
#endif
    }
    CanConfigGuard(const CanConfigGuard&) = delete;
    CanConfigGuard& operator=(const CanConfigGuard&) = delete;
};

static uint32_t gCanConfigGeneration = 0;  // protected by gCanConfigMux

struct CanControlSnapshot {
    FSDSettings settings;
    uint32_t generation;
};

inline CanControlSnapshot captureCanControl() {
    CanConfigGuard guard;
    return {static_cast<const FSDSettings&>(cfg), gCanConfigGeneration};
}

// A stalk frame in flight must not undo a newer web/manual-profile selection.
inline void publishStalkProfile(uint8_t profile, const CanControlSnapshot& control) {
    CanConfigGuard guard;
    if (gCanConfigGeneration == control.generation &&
        cfg.hwMode == control.settings.hwMode && cfg.profileModeAuto) {
        cfg.speedProfile = profile;
    }
}
