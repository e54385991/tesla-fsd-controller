#pragma once
// ── Module: 0x2F8 (760) UI_gpsVehicleSpeed sniffer ───────────────────────────
// Read-only. No frames transmitted. Exists to answer three questions for the
// pending Legacy speed-breakout feature (see project memory
// project_legacy_speed_breakout_pending):
//   1. Is 0x2F8 present on the bus our ESP32 is wired to?
//   2. What's its send period? (tight period → our re-transmit gets clobbered)
//   3. What raw value is byte 5 (UI_userSpeedOffset) currently carrying?
//
// Bit layout (opendbc tesla_can.dbc — Intel/little-endian):
//   UI_userSpeedOffset      bit40|6  data[5] & 0x3F            actual = raw − 30 kph
//   UI_mppSpeedLimit        bit48|5  data[6] & 0x1F            actual = raw × 5  kph
//   UI_userSpeedOffsetUnits bit47|1  (data[5] >> 7) & 0x01     0=kph, 1=mph
//   UI_mapSpeedLimitUnits   bit46|1  (data[5] >> 6) & 0x01     0=kph, 1=mph
//
// Frame is from tesla_can.dbc (Legacy/GTW era), NOT tesla_model3_party.dbc.
// On Model 3/Y Party CAN it may be absent — that's what we're testing.

#include <Arduino.h>
#include "can_frame_types.h"
#include "fsd_config.h"

extern FSDConfig cfg;

inline void handleGpsVehicleSpeed(const CanFrame& frame) {
    if (frame.dlc < 7) return;
    cfg.gpsSpeedSeen     = true;
    cfg.gpsUserOffsetRaw = frame.data[5] & 0x3F;
    cfg.gpsMppLimitRaw   = frame.data[6] & 0x1F;
    uint32_t now = millis();
    if (cfg.gpsSpeedLastMs != 0) {
        cfg.gpsSpeedPeriodMs = now - cfg.gpsSpeedLastMs;
    }
    cfg.gpsSpeedLastMs = now;
    cfg.gpsSpeedCount++;
}
