#pragma once
#include "can_driver.h"
#include <atomic>
#include <driver/twai.h>

#ifndef TWAI_TX_PIN
#define TWAI_TX_PIN GPIO_NUM_5
#endif
#ifndef TWAI_RX_PIN
#define TWAI_RX_PIN GPIO_NUM_4
#endif

struct TWAIDriver : public CanDriver {
    gpio_num_t txPin, rxPin;
    static constexpr bool kSupportsISR = false;

    TWAIDriver(gpio_num_t tx = (gpio_num_t)TWAI_TX_PIN,
               gpio_num_t rx = (gpio_num_t)TWAI_RX_PIN)
        : txPin(tx), rxPin(rx) {}

    bool init() override {
        return startController();
    }

    bool send(const CanFrame& frame) override {
        if (!driverOK_ || frame.id > 0x7FF || frame.dlc > 8) return false;
        twai_message_t msg = {};
        msg.identifier        = frame.id;
        msg.data_length_code  = frame.dlc;
        memcpy(msg.data, frame.data, 8);
        if (twai_transmit(&msg, pdMS_TO_TICKS(2)) != ESP_OK) {
            if (isBusOff()) recoverWithCooldown();
            return false;
        }
        return true;
    }

    bool read(CanFrame& frame) override {
        if (!driverOK_) {
            tryRecover();
            return false;
        }
        twai_message_t msg = {};
        while (twai_receive(&msg, 0) == ESP_OK) {
            // CanFrame intentionally represents standard Classical CAN data
            // only. Never reinterpret another frame format as a Tesla payload.
            if (msg.extd || msg.rtr || msg.dlc_non_comp ||
                msg.identifier > 0x7FF || msg.data_length_code > 8) continue;
            frame.id  = msg.identifier;
            frame.dlc = msg.data_length_code;
            memset(frame.data, 0, 8);
            memcpy(frame.data, msg.data, frame.dlc);
            return true;
        }
        if (isBusOff()) recoverWithCooldown();
        return false;
    }

    bool isDriverOK() const { return driverOK_; }
    uint32_t recoveryGeneration() const override {
        return recoveryGeneration_.load(std::memory_order_relaxed);
    }

    void setFilters(const uint32_t* /*ids*/, uint8_t /*count*/) override {
        // Software filtering via isFilteredId() in handlers.h
    }

private:
    static constexpr uint32_t BUSOFF_COOLDOWN_MS  = 1000;
    static constexpr uint32_t DRIVERFAIL_RETRY_MS = 10000;

    bool     driverOK_     = false;
    uint32_t lastRecovery_ = 0;
    // Web telemetry and the CAN core both read the current receive session.
    std::atomic<uint32_t> recoveryGeneration_{0};

    bool startController() {
        driverOK_ = false;
        twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(txPin, rxPin, TWAI_MODE_NORMAL);
        g.rx_queue_len = 128;
        g.tx_queue_len = 16;
        twai_timing_config_t t = TWAI_TIMING_CONFIG_500KBITS();
        twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();
        if (twai_driver_install(&g, &t, &f) != ESP_OK) return false;
        if (twai_start() != ESP_OK) {
            twai_driver_uninstall();
            return false;
        }
        driverOK_ = true;
        recoveryGeneration_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    bool isBusOff() {
        twai_status_info_t status;
        if (twai_get_status_info(&status) != ESP_OK) return false;
        return status.state == TWAI_STATE_BUS_OFF;
    }

    // stop → uninstall → reinstall → start.
    // Directly resets the TWAI peripheral and lets the SN65HVD230 transceiver
    // recover from dominant-timeout lockup (TX goes idle during uninstall).
    // Aligned with tesla-open-can-mod: skip twai_initiate_recovery() entirely
    // since it requires 128 recessive bits on the bus — useless when the
    // transceiver is locked and the bus is dead.
    void recoverWithCooldown() {
        uint32_t now = millis();
        if (now - lastRecovery_ < BUSOFF_COOLDOWN_MS) return;
        lastRecovery_ = now;
        twai_stop();
        twai_driver_uninstall();
        startController();
    }

    void tryRecover() {
        uint32_t now = millis();
        if (now - lastRecovery_ < DRIVERFAIL_RETRY_MS) return;
        lastRecovery_ = now;
        twai_stop();             // defensive: no-op if already stopped/uninstalled
        twai_driver_uninstall();
        startController();
    }
};
