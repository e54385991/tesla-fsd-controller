#pragma once
// ── Module: 0x2F8 (760) UI_gpsVehicleSpeed sniffer ───────────────────────────
// Observes the OEM frame before optional Legacy rewriting in mod_fsd.h.
// This parser never sends. Its sample remains the original vehicle value.
//
// Bit layout (opendbc tesla_can.dbc — Intel/little-endian):
//   UI_userSpeedOffset      bit40|6  data[5] & 0x3F            value = raw − 30, user units
//   UI_mppSpeedLimit        bit48|5  data[6] & 0x1F            value = raw × 5, map units
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
    if (frame.dlc != 8) return;
    cfg.gpsSpeedSeen     = true;
    cfg.gpsUserOffsetRaw = frame.data[5] & 0x3F;
    cfg.gpsMppLimitRaw   = frame.data[6] & 0x1F;
    cfg.gpsUserOffsetUnits = (frame.data[5] >> 7) & 1;
    cfg.gpsMapLimitUnits   = (frame.data[5] >> 6) & 1;
    uint32_t now = millis();
    if (cfg.gpsSpeedLastMs != 0) {
        cfg.gpsSpeedPeriodMs = now - cfg.gpsSpeedLastMs;
    }
    cfg.gpsSpeedLastMs = now;
    cfg.gpsSpeedCount++;
}
