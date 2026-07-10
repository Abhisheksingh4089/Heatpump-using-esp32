#pragma once
#include <Arduino.h>
#include "SystemData.h"
#include "ConfigProfile.h"
#include "AlarmManager.h"
#include "Logger.h"

// ⚠️  GPIO pin map for LILYGO T-Call V1.4 — ALL occupied pins:
//    2=WATER_ECHO  4=SIM_PWRKEY  5=SIM_RST  13=DS18B20  14=LP_SWITCH
//    18=WATER_TRIG 19=RELAY      21=I2C_SDA  22=I2C_SCL
//    23=SIM_PWRON  25=HP_SWITCH
//    26=SIM_RX    27=SIM_TX   32=PZEM_RX0 33=PZEM_TX
//    34=PZEM_RX1  35=PZEM_RX2
#define RELAY_PIN  19

class ControlEngine {
public:
    void begin() {
        pinMode(RELAY_PIN, OUTPUT);
        digitalWrite(RELAY_PIN, LOW);
        hpSystem.relay = RelayState::OFF;
        _lastRelayChange = millis();
        logger.info("[Control] ControlEngine initialised. Relay OFF.");
    }

    void evaluate() {
        if (_hasCriticalAlarm()) {
            _transitionTo(DeviceState::FAULT);
            _setRelay(RelayState::OFF);
            return;
        }

        if (hpSystem.state == DeviceState::LOCKOUT) {
            _setRelay(RelayState::OFF);
            return;
        }

        if (hpSystem.state == DeviceState::FAULT) {
            if (!hpSystem.alarms.anyActive()) {
                logger.info("[Control] Alarms cleared. Returning to MONITORING.");
                _transitionTo(DeviceState::MONITORING);
            } else if (millis() - _faultStart > 300000UL) {
                logger.critical("[Control] Persistent fault. Entering LOCKOUT.");
                _transitionTo(DeviceState::LOCKOUT);
            }
            return;
        }

        if (hpSystem.state == DeviceState::MANUAL_MODE) return;

        if (hpSystem.state == DeviceState::MONITORING || hpSystem.state == DeviceState::AUTO_MODE) {
            if (hpSystem.tempCount > 0 && hpSystem.temps[0].online) {
                float temp = hpSystem.temps[0].value;
                float setpt = configManager.config.tempSetpoint;
                float hyst = 1.5f;

                if (hpSystem.state == DeviceState::MONITORING && temp < (setpt - hyst)) {
                    logger.logf(LogLevel::INFO, "[Control] Temp %.1f < setpoint %.1f → AUTO_MODE", temp, setpt);
                    _transitionTo(DeviceState::AUTO_MODE);
                    _setRelayWithDelay(RelayState::ON);
                }

                if (hpSystem.state == DeviceState::AUTO_MODE && temp >= (setpt + hyst)) {
                    logger.logf(LogLevel::INFO, "[Control] Temp %.1f >= setpoint %.1f → MONITORING", temp, setpt);
                    _transitionTo(DeviceState::MONITORING);
                    _setRelayWithDelay(RelayState::OFF);
                }
            }
        }

        if (hpSystem.state == DeviceState::READY) {
            _transitionTo(DeviceState::MONITORING);
        }
    }

    void manualRelay(bool on) {
        if (hpSystem.state != DeviceState::MANUAL_MODE) return;
        _setRelayWithDelay(on ? RelayState::ON : RelayState::OFF);
    }

    void setManualMode(bool manual) {
        if (manual) _transitionTo(DeviceState::MANUAL_MODE);
        else {
            _transitionTo(DeviceState::MONITORING);
            _setRelay(RelayState::OFF);
        }
    }

    void clearLockout() {
        if (hpSystem.state == DeviceState::LOCKOUT) {
            logger.warning("[Control] LOCKOUT cleared by operator.");
            hpSystem.alarms = AlarmData{};
            _transitionTo(DeviceState::MONITORING);
            _setRelay(RelayState::OFF);
        }
    }

private:
    unsigned long _lastRelayChange = 0;
    unsigned long _faultStart      = 0;

    bool _hasCriticalAlarm() const {
        return hpSystem.alarms.overvoltage    ||
               hpSystem.alarms.overcurrent   ||
               hpSystem.alarms.highTemp      ||
               hpSystem.alarms.highPressure  ||
               hpSystem.alarms.lowPressure   ||
               hpSystem.alarms.criticalWaterLevel;  // low water → HP off
    }

    void _transitionTo(DeviceState next) {
        if (hpSystem.state == next) return;
        logger.logf(LogLevel::INFO, "[Control] State: %s → %s",
                    stateToStr(hpSystem.state), stateToStr(next));
        if (next == DeviceState::FAULT) _faultStart = millis();
        hpSystem.state = next;
    }

    void _setRelay(RelayState r) {
        hpSystem.relay = r;
        digitalWrite(RELAY_PIN, r == RelayState::ON ? HIGH : LOW);
    }

    void _setRelayWithDelay(RelayState r) {
        uint32_t minDelay = configManager.config.relayDelayMs;
        if (millis() - _lastRelayChange < minDelay) return;
        if (hpSystem.relay != r) {
            _setRelay(r);
            _lastRelayChange = millis();
            logger.logf(LogLevel::INFO, "[Control] Relay → %s", r == RelayState::ON ? "ON" : "OFF");
        }
    }
};

extern ControlEngine controlEngine;
