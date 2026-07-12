#pragma once
#include <Arduino.h>
#include "SystemData.h"
#include "ConfigProfile.h"
#include "AlarmManager.h"
#include "Logger.h"

// ============================================================
//  CONTROL ENGINE  —  Dual NC Relay (Active-LOW module)
//
//  RELAY WIRING (active-LOW module, NC contacts):
//    GPIO LOW  → coil energized → NC OPEN  → equipment STOPPED
//    GPIO HIGH → coil released  → NC CLOSED → equipment RUNS
//
//  ── 2-STATE ENABLE LOGIC ──────────────────────────────────
//  Each device (HP, Heater) is either ENABLED or DISABLED.
//
//  HEATER ENABLED: Runs until temp >= heaterSetpoint. Stops if sensor lost.
//  HEATER DISABLED: Forced OFF.
//
//  HP ENABLED: Runs only in band: (setpoint - hysteresis) <= temp < setpoint.
//              Stops if temp >= setpoint, low water, or sensor lost.
//  HP DISABLED: Forced OFF.
// ============================================================

#define RELAY_HP_PIN      18
#define RELAY_HEATER_PIN  19

#define COIL_ON   LOW    // GPIO LOW  → coil energized → NC OPEN  → STOPPED
#define COIL_OFF  HIGH   // GPIO HIGH → coil released  → NC CLOSED → RUNS

class ControlEngine {
public:

    void begin() {
        pinMode(RELAY_HP_PIN,     OUTPUT);
        pinMode(RELAY_HEATER_PIN, OUTPUT);
        digitalWrite(RELAY_HP_PIN,     COIL_ON);
        digitalWrite(RELAY_HEATER_PIN, COIL_ON);

        hpSystem.relay         = RelayState::OFF;
        hpSystem.heaterRelay   = RelayState::OFF;
        hpSystem.heaterEnabled = true;
        hpSystem.hpEnabled     = true;
        _hpRunning             = false;
        _heaterRunning         = false;
        _lastHpChange          = millis();

        logger.info("[Control] Init OK. HP=AUTO Heater=AUTO.");
    }

    // ── Called every 1000 ms by Scheduler ────────────────────
    void evaluate() {
        uint8_t ctrlIdx = min((uint8_t)configManager.config.controlSensorIdx,
                              (uint8_t)(hpSystem.tempCount > 0 ? hpSystem.tempCount - 1 : 0));
        bool  tempValid   = (hpSystem.tempCount > 0 && hpSystem.temps[ctrlIdx].online);
        float currentTemp = tempValid ? hpSystem.temps[ctrlIdx].value : 0.0f;

        float hpSetpoint  = configManager.config.tempSetpoint;
        float hpHyst      = configManager.config.tempHysteresis;
        float hpStartTemp = hpSetpoint - hpHyst;   // HP starts below this
        float heaterSetpt = configManager.config.heaterSetpoint;

        bool hpPressureFault = hpSystem.alarms.highPressure || hpSystem.alarms.lowPressure;
        bool systemFault     = hpSystem.alarms.overvoltage   || hpSystem.alarms.overcurrent
                            || hpSystem.alarms.highTemp      || hpSystem.alarms.criticalWaterLevel;

        // ── LOCKOUT ──────────────────────────────────────────
        if (hpSystem.state == DeviceState::LOCKOUT) {
            _stopHP("Lockout");
            _stopHeater("Lockout");
            return;
        }

        // ── SYSTEM FAULT (stops everything) ─────────────────
        if (systemFault) {
            if (hpSystem.state != DeviceState::FAULT) _transition(DeviceState::FAULT);
            _stopHP("System fault");
            _stopHeater("System fault");
            if (millis() - _faultStart > 300000UL) {
                logger.critical("[Control] Fault >5min → LOCKOUT.");
                _transition(DeviceState::LOCKOUT);
            }
            return;
        }

        // ── HP PRESSURE FAULT (HP stops, heater continues) ──
        if (hpPressureFault) {
            if (hpSystem.state != DeviceState::FAULT) _transition(DeviceState::FAULT);
            _stopHP("Pressure fault");
            _driveHeater(tempValid, currentTemp, heaterSetpt);
            if (millis() - _faultStart > 300000UL) {
                logger.critical("[Control] HP pressure fault >5min → LOCKOUT.");
                _transition(DeviceState::LOCKOUT);
            }
            return;
        }

        // ── FAULT CLEARED ────────────────────────────────────
        if (hpSystem.state == DeviceState::FAULT) {
            logger.info("[Control] Faults cleared. Resuming normal operation.");
            _transition(DeviceState::MONITORING);
        }

        if (hpSystem.state == DeviceState::READY)
            _transition(DeviceState::MONITORING);

        // ── NORMAL CONTROL ───────────────────────────────────
        _driveHP(tempValid, currentTemp, hpStartTemp, hpSetpoint);
        _driveHeater(tempValid, currentTemp, heaterSetpt);
    }

    // ── Public Controls ───────────────────────────────────────
    void setHPEnabled(bool enabled, const char* caller = "Web") {
        hpSystem.hpEnabled = enabled;
        if (!enabled) _stopHP("Disabled by user");
        logger.logf(LogLevel::INFO, "[Control] HP %s [src=%s]", enabled ? "ENABLED" : "DISABLED", caller);
    }

    void setHeaterEnabled(bool enabled, const char* caller = "Web") {
        hpSystem.heaterEnabled = enabled;
        if (!enabled) _stopHeater("Disabled by user");
        logger.logf(LogLevel::INFO, "[Control] Heater %s [src=%s]", enabled ? "ENABLED" : "DISABLED", caller);
    }

    void clearLockout() {
        if (hpSystem.state == DeviceState::LOCKOUT) {
            hpSystem.alarms = AlarmData{};
            _hpRunning      = false;
            _heaterRunning  = false;
            _transition(DeviceState::MONITORING);
            logger.info("[Control] LOCKOUT cleared by operator.");
        }
    }

private:
    bool          _hpRunning      = false;
    bool          _heaterRunning  = false;
    unsigned long _lastHpChange   = 0;
    unsigned long _faultStart     = 0;

    // ── HP Control (Enabled / Disabled) ───────────────────────
    void _driveHP(bool tempValid, float temp, float startTemp, float stopTemp) {
        if (!hpSystem.hpEnabled) {
            _stopHP("HP is disabled");
            return;
        }

        bool waterSafe  = !hpSystem.alarms.criticalWaterLevel;
        uint32_t delay  = configManager.config.relayDelayMs;

        if (!_hpRunning) {
            // Keep driving off for safety
            digitalWrite(RELAY_HP_PIN, COIL_ON);
            hpSystem.relay = RelayState::OFF;

            // HP runs only in the band: startTemp (38°C) ≤ temp < setpoint (40°C)
            if (tempValid && temp >= startTemp && temp < stopTemp && waterSafe) {
                if (millis() - _lastHpChange >= delay) {
                    _hpRunning    = true;
                    _lastHpChange = millis();
                    digitalWrite(RELAY_HP_PIN, COIL_OFF);
                    hpSystem.relay = RelayState::ON;
                    logger.logf(LogLevel::INFO,
                        "[Control] HP ON — Thermostat: %.1fC >= Start %.1fC (band: %.1f-%.1fC)",
                        temp, startTemp, startTemp, stopTemp);
                }
            }
        } else {
            // Running — stop when setpoint reached, falls below band, sensor lost, or water low
            bool mustStop = !tempValid || (temp >= stopTemp) || (temp < startTemp) || !waterSafe;
            if (mustStop) {
                const char* reason = !tempValid ? "Sensor lost" :
                                     !waterSafe ? "Low water" :
                                     (temp >= stopTemp) ? "Setpoint reached" :
                                     "Temp below HP band";
                _stopHP(reason);
            } else {
                // Keep driving relay every cycle (glitch protection)
                digitalWrite(RELAY_HP_PIN, COIL_OFF);
                hpSystem.relay = RelayState::ON;
            }
        }
    }

    // ── Heater Control (Enabled / Disabled) ───────────────────
    void _driveHeater(bool tempValid, float temp, float setpt) {
        if (!hpSystem.heaterEnabled) {
            _stopHeater("Heater is disabled");
            return;
        }

        if (!tempValid) {
            _stopHeater("Sensor lost");
            return;
        }

        if (!_heaterRunning) {
            // Start if below setpoint
            if (temp < setpt) {
                _heaterRunning = true;
                logger.logf(LogLevel::INFO,
                    "[Control] Heater ON — Thermostat: %.1fC < Setpt %.1fC", temp, setpt);
            }
            bool run = _heaterRunning;
            digitalWrite(RELAY_HEATER_PIN, run ? COIL_OFF : COIL_ON);
            hpSystem.heaterRelay = run ? RelayState::ON : RelayState::OFF;
        } else {
            // Running — stop when temp reaches setpoint
            if (temp >= setpt) {
                _stopHeater("Setpoint reached");
            } else {
                // Keep driving relay every cycle (glitch protection)
                digitalWrite(RELAY_HEATER_PIN, COIL_OFF);
                hpSystem.heaterRelay = RelayState::ON;
            }
        }
    }

    void _stopHP(const char* reason) {
        digitalWrite(RELAY_HP_PIN, COIL_ON);
        if (_hpRunning || hpSystem.relay == RelayState::ON)
            logger.logf(LogLevel::INFO, "[Control] HP OFF — %s", reason);
        hpSystem.relay = RelayState::OFF;
        _hpRunning     = false;
        _lastHpChange  = millis();
    }

    void _stopHeater(const char* reason) {
        digitalWrite(RELAY_HEATER_PIN, COIL_ON);
        if (_heaterRunning || hpSystem.heaterRelay == RelayState::ON)
            logger.logf(LogLevel::INFO, "[Control] Heater OFF — %s", reason);
        hpSystem.heaterRelay = RelayState::OFF;
        _heaterRunning       = false;
    }

    void _transition(DeviceState next) {
        if (hpSystem.state == next) return;
        logger.logf(LogLevel::INFO, "[Control] State: %s -> %s",
                    stateToStr(hpSystem.state), stateToStr(next));
        if (next == DeviceState::FAULT) _faultStart = millis();
        hpSystem.state = next;
    }
};

extern ControlEngine controlEngine;
