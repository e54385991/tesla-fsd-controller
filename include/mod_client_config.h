#pragma once

#include <stdint.h>

namespace client_config {

constexpr uint16_t kHttpsPort = 443;
bool start();
void serviceSerial();

}  // namespace client_config
