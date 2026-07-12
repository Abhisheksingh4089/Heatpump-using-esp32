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
//  ── HP CONTROL MODES ──────────────────────────────────────
//  hpManualOn=true,  _hpForceOff=false  → MANUAL ON  (force relay ON)
//  hpManualOn=false, _hpForceOff=true   → MANUAL OFF (force relay OFF)
//  hpManualOn=false, _hpForceOff=false  → AUTO       (thermostat drives HP)
//
//  AUTO HP logic:
//    START when: temp < (setpoint - hysteresis)
//    STOP  when: temp >= setpoint
//
//  ── HEATER CONTROL MODES ──────────────────────────────────
//  heaterManualOn=true,  _heaterForceOff=false → MANUAL ON  (force relay ON, safety stop at setpoint)
//  heaterManualOn=false, _heaterForceOff=true  → MANUAL OFF (force relay OFF)
//  heaterManualOn=false, _heaterForceOff=false → AUTO       (thermostat drives Heater)
//
//  AUTO Heater logic:
//    RUNS  when: temp < heaterSetpoint
//    STOPS when: temp >= heaterSetpoint
//
//  ── BUTTON ACTIONS ────────────────────────────────────────
//  HP ON      → hpManualOn=true,  _hpForceOff=false
//  HP OFF     → hpManualOn=false, _hpForceOff=true  (holds OFF)
//  Heater ON  → heaterManualOn=true,  _heaterForceOff=false
//  Heater OFF → heaterManualOn=false, _heaterForceOff=true (holds OFF)
//  AUTO btn   → clears ALL manual flags → both devices return to AUTO thermostat
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

        hpSystem.relay          = RelayState::OFF;
        hpSystem.heaterRelay    = RelayState::OFF;
        hpSystem.heaterManualOn = false;
        hpSystem.hpManualOn     = false;
        _hpRunning              = false;
        _heaterRunning          = false;
        _hpForceOff             = false;
        _heaterForceOff         = false;
        _lastHpChange           = millis();

        logger.info("[Control] Init OK. HP=OFF Heater=OFF. Both in AUTO thermostat mode.");
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
            logger.info("[Control] Faults cleared. Resuming AUTO.");
            _transition(DeviceState::MONITORING);
        }

        if (hpSystem.state == DeviceState::READY)
            _transition(DeviceState::MONITORING);

        // ── NORMAL CONTROL ───────────────────────────────────
        _driveHP(tempValid, currentTemp, hpStartTemp, hpSetpoint);
        _driveHeater(tempValid, currentTemp, heaterSetpt);
    }

    // ── HP button: ON ─────────────────────────────────────────
    void setHPManual(bool on, const char* caller = "Web") {
        hpSystem.hpManualOn = on;
        if (on) {
            _hpForceOff = false;
            logger.logf(LogLevel::INFO, "[Control] HP MANUAL ON [src=%s]", caller);
        } else {
            _hpForceOff = true;
            _stopHP("Manual OFF");
            logger.logf(LogLevel::INFO, "[Control] HP MANUAL OFF [src=%s]", caller);
        }
    }

    // ── Heater button: ON / OFF ───────────────────────────────
    void setHeaterManual(bool on, const char* caller = "Web") {
        hpSystem.heaterManualOn = on;
        if (on) {
            _heaterForceOff = false;
            logger.logf(LogLevel::INFO, "[Control] Heater MANUAL ON [src=%s]", caller);
        } else {
            _heaterForceOff = true;
            _stopHeater("Manual OFF");
            logger.logf(LogLevel::INFO, "[Control] Heater MANUAL OFF [src=%s]", caller);
        }
    }

    // ── AUTO button: resume thermostat for both devices ───────
    void setManualMode(bool manual, const char* caller = "Web") {
        if (manual) {
            _transition(DeviceState::MANUAL_MODE);
            logger.logf(LogLevel::INFO, "[Control] MANUAL mode entered [src=%s]", caller);
        } else {
            // Clear ALL manual flags → both HP and Heater back to AUTO thermostat
            hpSystem.hpManualOn     = false;
            hpSystem.heaterManualOn = false;
            _hpForceOff             = false;
            _heaterForceOff         = false;
            _transition(DeviceState::MONITORING);
            logger.logf(LogLevel::INFO,
                "[Control] AUTO mode resumed [src=%s] — thermostat controls HP+Heater", caller);
        }
    }

    void clearLockout() {
        if (hpSystem.state == DeviceState::LOCKOUT) {
            hpSystem.alarms = AlarmData{};
            _hpRunning      = false;
            _heaterRunning  = false;
            _hpForceOff     = false;
            _heaterForceOff = false;
            _transition(DeviceState::MONITORING);
            logger.info("[Control] LOCKOUT cleared by operator.");
        }
    }

private:
    bool          _hpRunning      = false;
    bool          _heaterRunning  = false;
    bool          _hpForceOff     = false;
    bool          _heaterForceOff = false;
    unsigned long _lastHpChange   = 0;
    unsigned long _faultStart     = 0;

    // ── HP: Manual ON / Manual OFF / AUTO thermostat ─────────
    void _driveHP(bool tempValid, float temp, float startTemp, float stopTemp) {
        bool waterSafe  = !hpSystem.alarms.criticalWaterLevel;
        uint32_t delay  = configManager.config.relayDelayMs;

        // ── MANUAL ON ──
        if (hpSystem.hpManualOn) {
            if (!waterSafe) { _stopHP("Low water — manual blocked"); return; }
            bool delayOk = (millis() - _lastHpChange >= delay);
            if (_hpRunning || delayOk) {
                if (!_hpRunning) {
                    _hpRunning    = true;
                    _lastHpChange = millis();
                    logger.info("[Control] HP ON — Manual");
                    _transition(DeviceState::MANUAL_MODE);
                }
                digitalWrite(RELAY_HP_PIN, COIL_OFF);
                hpSystem.relay = RelayState::ON;
            } else {
                // Waiting for anti-short-cycle delay
                digitalWrite(RELAY_HP_PIN, COIL_ON);
                hpSystem.relay = RelayState::OFF;
            }
            return;
        }

        // ── MANUAL OFF ──
        if (_hpForceOff) {
            digitalWrite(RELAY_HP_PIN, COIL_ON);
            hpSystem.relay = RelayState::OFF;
            return;
        }

        // ── AUTO thermostat ──
        // HP runs only in the band: startTemp (38°C) ≤ temp < setpoint (40°C)
        // Below startTemp (e.g. 30°C) → HP is OFF (Heater handles cold water)
        // Reaches setpoint (40°C)     → HP stops
        if (!_hpRunning) {
            digitalWrite(RELAY_HP_PIN, COIL_ON);
            hpSystem.relay = RelayState::OFF;
            if (tempValid && temp >= startTemp && temp < stopTemp && waterSafe) {
                if (millis() - _lastHpChange >= delay) {
                    _hpRunning    = true;
                    _lastHpChange = millis();
                    digitalWrite(RELAY_HP_PIN, COIL_OFF);
                    hpSystem.relay = RelayState::ON;
                    logger.logf(LogLevel::INFO,
                        "[Control] HP ON — Auto: %.1fC >= Start %.1fC (band: %.1f-%.1fC)",
                        temp, startTemp, startTemp, stopTemp);
                    _transition(DeviceState::AUTO_MODE);
                }
            }
        } else {
            // HP is running — stop when setpoint reached, sensor lost, or water low
            bool stop = !tempValid || (temp >= stopTemp) || !waterSafe;
            if (stop) {
                _stopHP(!tempValid ? "Sensor lost" : !waterSafe ? "Low water" : "Setpoint reached");
                _transition(DeviceState::MONITORING);
            } else {
                // Keep driving relay every cycle (glitch protection)
                digitalWrite(RELAY_HP_PIN, COIL_OFF);
                hpSystem.relay = RelayState::ON;
            }
        }
    }

    // ── Heater: Manual ON / Manual OFF / AUTO thermostat ─────
    void _driveHeater(bool tempValid, float temp, float setpt) {
        // ── MANUAL ON ──
        if (hpSystem.heaterManualOn) {
            bool safetyStop = (tempValid && temp >= setpt);
            if (safetyStop) {
                // Safety: don't run heater above setpoint even in manual
                if (_heaterRunning) {
                    _stopHeater("Safety: temp above setpoint");
                } else {
                    digitalWrite(RELAY_HEATER_PIN, COIL_ON);
                    hpSystem.heaterRelay = RelayState::OFF;
                }
            } else {
                if (!_heaterRunning) {
                    _heaterRunning = true;
                    logger.info("[Control] Heater ON — Manual");
                }
                digitalWrite(RELAY_HEATER_PIN, COIL_OFF);
                hpSystem.heaterRelay = RelayState::ON;
            }
            return;
        }

        // ── MANUAL OFF ──
        if (_heaterForceOff) {
            if (_heaterRunning) _stopHeater("Manual OFF");
            else {
                digitalWrite(RELAY_HEATER_PIN, COIL_ON);
                hpSystem.heaterRelay = RelayState::OFF;
            }
            return;
        }

        // ── AUTO thermostat ──
        // Run heater when temp < heaterSetpoint; stop when temp >= heaterSetpoint
        if (!tempValid) {
            // No sensor — stop heater for safety
            if (_heaterRunning) _stopHeater("Sensor lost");
            else {
                digitalWrite(RELAY_HEATER_PIN, COIL_ON);
                hpSystem.heaterRelay = RelayState::OFF;
            }
            return;
        }

        if (!_heaterRunning) {
            // Not running — start if below setpoint
            if (temp < setpt) {
                _heaterRunning = true;
                logger.logf(LogLevel::INFO,
                    "[Control] Heater ON — Auto: %.1fC < Setpt %.1fC", temp, setpt);
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
        logger.logf(LogLevel::INFO, "[Control] %s -> %s",
                    stateToStr(hpSystem.state), stateToStr(next));
        if (next == DeviceState::FAULT) _faultStart = millis();
        hpSystem.state = next;
    }
};

extern ControlEngine controlEngine;
