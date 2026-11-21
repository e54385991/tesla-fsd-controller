#pragma once

#include <stdint.h>

namespace client_config {

// 443 is reserved for the FSD TLS interception service (feature_configuration
// reply + reverse-mode passthrough). The optional self-service client-config
// HTTPS API lives on a separate port so it cannot collide with FSD traffic.
constexpr uint16_t kFsdPort = 443;
constexpr uint16_t kClientConfigPort = 10011;
constexpr uint16_t kHttpsPort = kFsdPort;  // backwards-compatible alias

bool start();
bool configStarted();
void serviceSerial();

}  // namespace client_config
