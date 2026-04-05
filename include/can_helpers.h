#pragma once
#include "can_frame_types.h"

/*
 * Verified against upstream:
 * https://gitlab.com/Tesla-OPEN-CAN-MOD/tesla-open-can-mod
 */

inline uint8_t readMuxID(const CanFrame& frame) {
    return frame.data[0] & 0x07;
}

inline bool isFSDSelectedInUI(const CanFrame& frame) {
    return (frame.data[4] >> 6) & 0x01;
}

inline void setSpeedProfileV12V13(CanFrame& frame, int profile) {
    // V12/V13 speed field is bits 1-2 of byte 6 (2 bits, max value 3).
    // Clamp to [0,2] — profiles 3/4 are HW4-only; writing them here
    // would shift bits outside the cleared mask and corrupt the frame.
    if (profile > 2) profile = 2;
    frame.data[6] &= ~0x06;
    frame.data[6] |= (uint8_t)((profile & 0x03) << 1);
}

inline void setBit(CanFrame& frame, int bit, bool value) {
    if (bit < 0 || bit >= 64) return;
    int byteIndex = bit / 8;
    int bitIndex = bit % 8;
    uint8_t mask = static_cast<uint8_t>(1U << bitIndex);
    if (value) {
        frame.data[byteIndex] |= mask;
    } else {
        frame.data[byteIndex] &= static_cast<uint8_t>(~mask);
    }
}
