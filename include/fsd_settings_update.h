#pragma once
#include <cstddef>
#include <cstring>
#include "can_control.h"

struct FSDSettingSpec {
    const char* key;
    uint8_t minimum, maximum;
    bool clamp;  // preserve existing HW3 slew/encoding clamping
    uint8_t (*read)(const FSDSettings&);
    void (*write)(FSDSettings&, uint8_t);
};

// One registry drives parsing and applying. Arrays use the same validation as
// scalar settings, so no late request error can leave a partial configuration.
#define FSD_SETTING(key, field, lo, hi, clamp) \
    {key, lo, hi, clamp, \
     [](const FSDSettings& s) -> uint8_t { return s.field; }, \
     [](FSDSettings& s, uint8_t value) { s.field = value; }}
static const FSDSettingSpec kFSDSettingSpecs[] = {
    FSD_SETTING("hwMode", hwMode, 0, 2, false),
    FSD_SETTING("speedProfile", speedProfile, 0, 4, false),
    FSD_SETTING("fsdEnable", fsdEnable, 0, 1, false),
    FSD_SETTING("profileMode", profileModeAuto, 0, 1, false),
    FSD_SETTING("forceActivate", forceActivate, 0, 1, false),
    FSD_SETTING("overrideSL", overrideSpeedLimit, 0, 1, false),
    FSD_SETTING("legacyOffset", legacyOffset, 0, 33, false),
    FSD_SETTING("removeVSL", removeVisionSpeedLimit, 0, 1, false),
    FSD_SETTING("legacyByte7Enable", legacyByte7Enable, 0, 1, false),
    FSD_SETTING("legacyByte7Value", legacyByte7Value, 0, 15, false),
    FSD_SETTING("isaChime", isaChimeSuppress, 0, 1, false),
    FSD_SETTING("emergencyDet", emergencyDetection, 0, 1, false),
    FSD_SETTING("hw4Offset", hw4OffsetRaw, 0, 21, false),
    FSD_SETTING("hw3AutoSpeed", hw3AutoSpeed, 0, 1, false),
    FSD_SETTING("hw3CustomSpeed", hw3CustomSpeed, 0, 1, false),
    FSD_SETTING("hw3OffsetSlew", hw3OffsetSlew, 0, 1, false),
    FSD_SETTING("hw3SlewRate", hw3SlewRatePctPerSec, 1, 25, true),
    FSD_SETTING("hw3HighSpeedEnable", hw3HighSpeedEnable, 0, 1, false),
    FSD_SETTING("hw3WireEncoding", hw3WireEncoding, 0, 1, true),
    FSD_SETTING("ipBlockerEnabled", ipBlockerEnabled, 0, 1, false),
    FSD_SETTING("isaOverride", isaOverride, 0, 1, false),
    FSD_SETTING("tlsscBypass", tlsscBypass, 0, 1, false),
    FSD_SETTING("hw3CT0", hw3CustomTarget[0], 0, 200, false),
    FSD_SETTING("hw3CT1", hw3CustomTarget[1], 0, 200, false),
    FSD_SETTING("hw3CT2", hw3CustomTarget[2], 0, 200, false),
    FSD_SETTING("hw3CT3", hw3CustomTarget[3], 0, 200, false),
    FSD_SETTING("hw3CT4", hw3CustomTarget[4], 0, 200, false),
    FSD_SETTING("hw3HsPct0", hw3HighSpeedTargetPct[0], 0, 50, false),
    FSD_SETTING("hw3HsPct1", hw3HighSpeedTargetPct[1], 0, 50, false),
    FSD_SETTING("hw3HsPct2", hw3HighSpeedTargetPct[2], 0, 50, false),
    FSD_SETTING("hw3HsPct3", hw3HighSpeedTargetPct[3], 0, 50, false),
    FSD_SETTING("hw3HsPct4", hw3HighSpeedTargetPct[4], 0, 50, false),
};
#undef FSD_SETTING
static constexpr size_t kFSDSettingCount = sizeof(kFSDSettingSpecs) / sizeof(kFSDSettingSpecs[0]);

struct FSDSettingsUpdate {
    bool present[kFSDSettingCount] = {};
    uint8_t values[kFSDSettingCount] = {};

    bool contains(const char* key) const {
        for (size_t i = 0; i < kFSDSettingCount; ++i) {
            if (present[i] && std::strcmp(key, kFSDSettingSpecs[i].key) == 0) return true;
        }
        return false;
    }

    bool parse(size_t index, const char* text) {
        return parse(index, text, text ? std::strlen(text) : 0);
    }

    bool parse(size_t index, const char* text, size_t length) {
        if (index >= kFSDSettingCount || !text || length == 0) return false;
        uint32_t value = 0;
        for (size_t i = 0; i < length; ++i) {
            if (text[i] < '0' || text[i] > '9') return false;
            uint8_t digit = text[i] - '0';
            if (value > (UINT32_MAX - digit) / 10) return false;
            value = value * 10 + digit;
        }
        const auto& spec = kFSDSettingSpecs[index];
        if (value < spec.minimum || value > spec.maximum) {
            if (!spec.clamp) return false;
            value = value < spec.minimum ? spec.minimum : spec.maximum;
        }
        values[index] = static_cast<uint8_t>(value);
        present[index] = true;
        return true;
    }
};

struct FSDSettingsResult {
    const char* error;
    bool changed;
};

inline FSDSettingsResult applyFSDSettingsUpdate(const FSDSettingsUpdate& update) {
    const bool profileSpecified = update.contains("speedProfile");
    CanConfigGuard guard;
    FSDSettings next = static_cast<const FSDSettings&>(cfg);
    for (size_t i = 0; i < kFSDSettingCount; ++i) {
        if (update.present[i]) kFSDSettingSpecs[i].write(next, update.values[i]);
    }
    if (next.hwMode != 2 && next.speedProfile > 2) {
        if (profileSpecified) return {"speedProfile out of range for non-HW4", false};
        next.speedProfile = 2;
    }
    const bool invalidate = next.hwMode != cfg.hwMode || next.fsdEnable != cfg.fsdEnable ||
                            next.forceActivate != cfg.forceActivate;
    bool changed = false;
    for (const auto& spec : kFSDSettingSpecs) {
        if (spec.read(cfg) != spec.read(next)) {
            spec.write(cfg, spec.read(next));
            changed = true;
        }
    }
    if (invalidate) ++gCanConfigGeneration;
    return {nullptr, changed};
}
