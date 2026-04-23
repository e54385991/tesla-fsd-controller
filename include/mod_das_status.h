#pragma once
// ── Module: DAS status telemetry ─────────────────────────────────────────────
// Read-only parsers for DAS_status (0x39B/923) and DAS_status2 (0x389/905).
//
// Bit extraction: byte = bit/8, shift = bit%8, value = (data[byte]>>shift)&mask
//
// 0x39B (923) DAS_status — DLC >= 7:
//   DAS_fusedSpeedLimit         bit8|5   data[1] & 0x1F            ×5 kph  (0=SNA, 31=NONE)
//   DAS_visionOnlySpeedLimit    bit16|5  data[2] & 0x1F            ×5 kph  (0=SNA, 31=NONE)
//   DAS_forwardCollisionWarning bit22|2  (data[2]>>6) & 0x03
//   DAS_autoparkReady           bit24|1  data[3] & 0x01
//   DAS_sideCollisionAvoid      bit30|2  (data[3]>>6) & 0x03
//   DAS_sideCollisionWarning    bit32|2  data[4] & 0x03            0=none,1=left,2=right,3=both
//   DAS_laneDepartureWarning    bit37|3  (data[4]>>5) & 0x07
//   DAS_autopilotHandsOnState   bit42|4  (data[5]>>2) & 0x0F       nag level
//   DAS_autoLaneChangeState     bit46|5  ((data[5]>>6)&0x03)|((data[6]&0x07)<<2)
//
// 0x389 (905) DAS_status2 — DLC >= 5:
//   DAS_ACC_report              bit26|5  (data[3]>>2) & 0x1F       0=AP off

#include "can_frame_types.h"
#include "can_helpers.h"
#include "drivers/can_driver.h"
#include "fsd_config.h"
#include "mod_log.h"

extern FSDConfig cfg;

// ── DAS_status (0x39B / 923) ─────────────────────────────────────────────────
inline void handleDASStatus(const CanFrame& frame) {
    if (frame.dlc < 7) return;
    cfg.fusedSpeedLimit  = frame.data[1] & 0x1F;                                      // bit8|5  DAS_fusedSpeedLimit ×5=kph
    cfg.visionSpeedLimit = frame.data[2] & 0x1F;                                      // bit16|5 DAS_visionOnlySpeedLimit ×5=kph
    cfg.fcwLevel         = (frame.data[2] >> 6) & 0x03;                               // bit22|2
    cfg.sideCollision    = frame.data[4] & 0x03;                                       // bit32|2
    cfg.laneDeptWarning  = (frame.data[4] >> 5) & 0x07;                               // bit37|3
    cfg.nagLevel         = (frame.data[5] >> 2) & 0x0F;                               // bit42|4
    cfg.laneChangeState  = ((frame.data[5] >> 6) & 0x03) | ((frame.data[6] & 0x07) << 2); // bit46|5
    // Bring-up diag: log the first 923 frame carrying a valid fused limit so we
    // can cross-check with 921 which ID the car actually broadcasts.
    static bool logged923 = false;
    if (!logged923) {
        uint8_t fl = frame.data[1] & 0x1F;
        if (fl > 0 && fl < 31) {
            uint32_t up = (millis() - cfg.uptimeStart) / 1000;
            char msg[48];
            snprintf(msg, sizeof(msg), "DAS 923 fusedLim=%ukph raw=%u", (unsigned)(fl * 5), (unsigned)fl);
            addDiagLog(up, msg);
            logged923 = true;
        }
    }
}

// ── DAS_status (0x39B / 923) ISA override inject path ──────────────────────
// When cfg.isaOverride is ON, FSD is active, and we're on HW4, re-broadcast
// 0x39B with fusedSpeedLimit/visionOnlySpeedLimit forced to 31 ("NONE"). This
// prevents 2024+ DAS firmware from clamping the set speed to the nav limit.
// dlc>=8 required because we rewrite byte 7 checksum. Same re-send pattern
// as 0x399 ISA chime in mod_fsd.h — DAS-originated frame; our echo reaches
// the car bus after the original, and the car uses the newer arrival.
inline void handleDASStatusISAOverride(const CanFrame& frame, CanDriver& driver) {
    if (frame.dlc < 8) return;
    if (!cfg.isaOverride) return;
    if (!cfg.fsdTriggered || !cfg.fsdEnable) return;
    if (cfg.hwMode != 2) return;  // HW4 only
    CanFrame echo = frame;
    echo.data[1] |= 0x1F;  // fusedSpeedLimit = 31 (NONE)
    echo.data[2] |= 0x1F;  // visionOnlySpeedLimit = 31 (NONE)
    echo.data[7] = computeVehicleChecksum(echo);
    if (driver.send(echo)) cfg.modifiedCount++;
    else                   cfg.errorCount++;
}

// ── DAS_status_ISA (0x399 / 921) ─────────────────────────────────────────────
// Secondary source for fused speed limit — tesla-open-can-mod keys off this ID.
// Some vehicle variants broadcast fused limit on 921, others on 923. Listen on
// both; latest update wins. Diag logs the first occurrence on each so field data
// shows which ID the car uses.
inline void handleDASStatusISA(const CanFrame& frame) {
    if (frame.dlc < 2) return;
    uint8_t fl = frame.data[1] & 0x1F;
    if (fl > 0 && fl < 31) {
        cfg.fusedSpeedLimit = fl;
        static bool logged921 = false;
        if (!logged921) {
            uint32_t up = (millis() - cfg.uptimeStart) / 1000;
            char msg[48];
            snprintf(msg, sizeof(msg), "DAS 921 fusedLim=%ukph raw=%u", (unsigned)(fl * 5), (unsigned)fl);
            addDiagLog(up, msg);
            logged921 = true;
        }
    }
}

// ── DAS_status2 (0x389 / 905) ────────────────────────────────────────────────
inline void handleDASStatus2(const CanFrame& frame) {
    if (frame.dlc < 5) return;
    cfg.accState = (frame.data[3] >> 2) & 0x1F;   // bit26|5
}
