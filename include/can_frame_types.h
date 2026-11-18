#pragma once
#include <cstdint>
#include <cstring>

// Standard (11-bit) Classical CAN data frame. Drivers reject unsupported
// formats before constructing one; DLC is always bounded by data[8].
struct CanFrame {
    uint32_t id = 0;
    uint8_t  dlc = 8;
    uint8_t  data[8] = {};
};
