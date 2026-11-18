#pragma once
#include <cstdint>
#include <deque>
#include <vector>

using gpio_num_t = int;
static constexpr int GPIO_NUM_5 = 5, GPIO_NUM_4 = 4;
static constexpr int ESP_OK = 0, ESP_FAIL = -1;
static constexpr int TWAI_MODE_NORMAL = 0, TWAI_STATE_RUNNING = 0, TWAI_STATE_BUS_OFF = 1;
struct twai_general_config_t { int rx_queue_len = 0, tx_queue_len = 0; };
struct twai_timing_config_t {};
struct twai_filter_config_t {};
struct twai_status_info_t { int state = TWAI_STATE_RUNNING; };
struct twai_message_t {
    uint32_t identifier = 0;
    uint8_t data_length_code = 0;
    uint8_t data[8] = {};
    bool extd = false, rtr = false, dlc_non_comp = false;
};
#define TWAI_GENERAL_CONFIG_DEFAULT(tx, rx, mode) twai_general_config_t{}
#define TWAI_TIMING_CONFIG_500KBITS() twai_timing_config_t{}
#define TWAI_FILTER_CONFIG_ACCEPT_ALL() twai_filter_config_t{}
#define pdMS_TO_TICKS(ms) (ms)

namespace fakeTwai {
static std::deque<twai_message_t> incoming;
static std::vector<twai_message_t> outgoing;
static int state = TWAI_STATE_RUNNING;
static int installResult = ESP_OK, startResult = ESP_OK, transmitResult = ESP_OK;
static unsigned installs = 0, uninstalls = 0;
static void reset() {
    incoming.clear(); outgoing.clear();
    state = TWAI_STATE_RUNNING;
    installResult = startResult = transmitResult = ESP_OK;
    installs = uninstalls = 0;
}
}
inline int twai_driver_install(twai_general_config_t*, twai_timing_config_t*, twai_filter_config_t*) {
    ++fakeTwai::installs;
    return fakeTwai::installResult;
}
inline int twai_start() {
    if (fakeTwai::startResult == ESP_OK) fakeTwai::state = TWAI_STATE_RUNNING;
    return fakeTwai::startResult;
}
inline int twai_stop() { return ESP_OK; }
inline int twai_driver_uninstall() { ++fakeTwai::uninstalls; return ESP_OK; }
inline int twai_get_status_info(twai_status_info_t* info) { info->state = fakeTwai::state; return ESP_OK; }
inline int twai_transmit(twai_message_t* message, int) {
    if (fakeTwai::transmitResult == ESP_OK) fakeTwai::outgoing.push_back(*message);
    return fakeTwai::transmitResult;
}
inline int twai_receive(twai_message_t* message, int) {
    if (fakeTwai::incoming.empty()) return ESP_FAIL;
    *message = fakeTwai::incoming.front(); fakeTwai::incoming.pop_front(); return ESP_OK;
}
