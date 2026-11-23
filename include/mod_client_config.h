#pragma once

#include <Arduino.h>
#include <stdint.h>

namespace client_config {

// 443 is reserved for the FSD TLS interception service (feature_configuration
// reply + reverse-mode passthrough). The self-service client-config API is
// served by the management web server on port 10010, so no second HTTPS
// server (and no extra ~40 KB TLS socket/task) is needed.
constexpr uint16_t kFsdPort = 443;
constexpr uint32_t kAutoOffDelayMs = 120000;  // 2 minutes non-blocking grace window after successful delivery

enum class Change : uint8_t { Publish, Rollback, Acknowledge };

bool start();
bool fsdHttpsStarted();
bool mothershipHttpStarted();
uint32_t recoveryCount();
const char* lastListenerError();
void requestListenerSync();
void scheduleAutoOff(uint32_t delayMs = kAutoOffDelayMs);
void cancelAutoOff();
bool autoOffPending();
uint32_t autoOffRemainingSec();
void serviceListeners();
bool apiGet(const char* authorization, String& response, int& status);
bool apiChange(const char* authorization, const char* body, size_t length,
               Change change, String& response, int& status);
const char* dashboardHtml();
void serviceSerial();

}  // namespace client_config
