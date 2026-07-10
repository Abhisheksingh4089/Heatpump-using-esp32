#pragma once
#include <Arduino.h>
#include "SystemData.h"
#include "ConfigProfile.h"
#include "AlarmManager.h"
#include "Logger.h"

// ============================================================
//  CONTROL ENGINE — Dual NC Relay Logic
//
//  LILYGO T-Call V1.4 — Pin reassignment:
//    GPIO 18 → RELAY_HP     (Heat Pump compressor)  [was WATER_TRIG]
//    GPIO 19 → RELAY_HEATER (Heater element)         [was single relay]
//    GPIO 2  → freed                                  [was WATER_ECHO]
//
//  NC RELAY WIRING (Normally Closed / Fail-Safe):
//    Coil OFF (LOW)  → NC contacts CLOSED → Equipment RUNS
//    Coil ON  (HIGH) → NC contacts OPEN   → Equipment STOPS
//    No ESP32 power  → Coil dies → NC closes → manual operation works ✅
//
//  HEAT PUMP LOGIC (hysteresis thermostat + safety):
//    startTemp = setpoint - hysteresis
//    START  when: currentTemp < startTemp AND HP_OK AND LP_OK AND no faults
//    STOP   when: currentTemp >= setpoint OR HP fault OR LP fault OR critical alarm
//
//  HEATER LOGIC (manual start, temp-guarded auto stop):
//    START  when: heaterManualOn == true
//    STOP   when: heaterManualOn == false OR currentTemp >= setpoint
// ============================================================

#define RELAY_HP_PIN      18   // Heat Pump compressor — NC relay (was WATER_TRIG)
#define RELAY_HEATER_PIN  19   // Heater element       — NC relay (was single relay pin)

// NC relay coil helpers — makes intent clear in code
#define COIL_ON   HIGH   // coil energized  → contacts OPEN  → equipment STOPPED
#define COIL_OFF  LOW    // coil released   → contacts CLOSED → equipment RUNS

class ControlEngine {
public:

    // ── begin() — call once in setup() ───────────────────────
    void begin() {
        pinMode(RELAY_HP_PIN,     OUTPUT);
        pinMode(RELAY_HEATER_PIN, OUTPUT);

        // NC relay: energize coil at startup → contacts OPEN → both OFF (safe default)
        digitalWrite(RELAY_HP_PIN,     COIL_ON);
        digitalWrite(RELAY_HEATER_PIN, COIL_ON);

        hpSystem.relay         = RelayState::OFF;
        hpSystem.heaterRelay   = RelayState::OFF;
        hpSystem.heaterManualOn = false;
        _hpRunning             = false;
        _lastHpChange          = millis();

        logger.info("[Control] ControlEngine init. HP=STOPPED, Heater=STOPPED (NC safe).");
    }

    // ── evaluate() — call every cycle (e.g. 1s) ──────────────
    void evaluate() {

        // ── State machine guards ──────────────────────────────
        if (_hasCriticalAlarm()) {
            _transitionTo(DeviceState::FAULT);
            _stopHP("Critical alarm");
            _stopHeater("Critical alarm");
            return;
        }

        if (hpSystem.state == DeviceState::LOCKOUT) {
            _stopHP("Lockout");
            _stopHeater("Lockout");
            return;
        }

        if (hpSystem.state == DeviceState::FAULT) {
            if (!hpSystem.alarms.anyActive()) {
                logger.info("[Control] Alarms cleared. Returning to MONITORING.");
                _transitionTo(DeviceState::MONITORING);
            } else if (millis() - _faultStart > 300000UL) {
                logger.critical("[Control] Persistent fault → LOCKOUT.");
                _transitionTo(DeviceState::LOCKOUT);
            }
            return;
        }

        if (hpSystem.state == DeviceState::READY)
            _transitionTo(DeviceState::MONITORING);

        if (hpSystem.state == DeviceState::MANUAL_MODE) return;

        // ── Get shared values ─────────────────────────────────
        bool tempValid  = (hpSystem.tempCount > 0 && hpSystem.temps[0].online);
        float currentTemp = tempValid ? hpSystem.temps[0].value : 0.0f;
        float setpt     = configManager.config.tempSetpoint;
        float hyst      = configManager.config.tempHysteresis;   // e.g. 2.0°C
        float startTemp = setpt - hyst;   // e.g. 40 - 2 = 38°C  → HP starts
        float stopTemp  = setpt;          // e.g. 40°C            → HP stops

        // ── HEAT PUMP RELAY (GPIO 18, NC) ────────────────────
        _evaluateHeatPump(tempValid, currentTemp, startTemp, stopTemp);

        // ── HEATER RELAY (GPIO 19, NC) ───────────────────────
        _evaluateHeater(tempValid, currentTemp, setpt);
    }

    // ── Public controls — called from WebServer / SimManager ─

    void setHeaterManual(bool on) {
        hpSystem.heaterManualOn = on;
        logger.logf(LogLevel::INFO, "[Control] Heater manual → %s", on ? "ON" : "OFF");
    }

    void setManualMode(bool manual) {
        if (manual) _transitionTo(DeviceState::MANUAL_MODE);
        else {
            _transitionTo(DeviceState::MONITORING);
            _stopHP("Manual mode exit");
            _stopHeater("Manual mode exit");
        }
    }

    void clearLockout() {
        if (hpSystem.state == DeviceState::LOCKOUT) {
            logger.warning("[Control] LOCKOUT cleared by operator.");
            hpSystem.alarms = AlarmData{};
            _transitionTo(DeviceState::MONITORING);
            _stopHP("Lockout cleared");
            _stopHeater("Lockout cleared");
        }
    }

private:
    bool          _hpRunning    = false;
    unsigned long _lastHpChange = 0;
    unsigned long _faultStart   = 0;

    // ── Heat Pump hysteresis control ──────────────────────────
    //    NC relay: COIL_OFF = HP runs, COIL_ON = HP stopped
    void _evaluateHeatPump(bool tempValid, float currentTemp,
                           float startTemp, float stopTemp) {
        bool hpFault    = hpSystem.alarms.highPressure || hpSystem.alarms.lowPressure;
        bool waterSafe  = !hpSystem.alarms.criticalWaterLevel;
        uint32_t minDelay = configManager.config.relayDelayMs;

        if (!_hpRunning) {
            // ── Currently stopped: start if all conditions met ──
            if (tempValid && currentTemp < startTemp && !hpFault && waterSafe) {
                if (millis() - _lastHpChange >= minDelay) {
                    _hpRunning = true;
                    digitalWrite(RELAY_HP_PIN, COIL_OFF);   // NC closes → HP runs
                    hpSystem.relay = RelayState::ON;
                    _lastHpChange  = millis();
                    logger.logf(LogLevel::INFO,
                        "[Control] HP START — Temp=%.1f°C < Start=%.1f°C",
                        currentTemp, startTemp);
                    _transitionTo(DeviceState::AUTO_MODE);
                }
            }
        } else {
            // ── Currently running: stop if target reached or fault ──
            bool mustStop = !tempValid
                         || (currentTemp >= stopTemp)
                         || hpFault
                         || !waterSafe;

            if (mustStop) {
                const char* reason = !tempValid        ? "Sensor lost"
                                   : hpFault           ? "HP/LP fault"
                                   : !waterSafe        ? "Low water"
                                                       : "Setpoint reached";
                _stopHP(reason);
                _transitionTo(DeviceState::MONITORING);
            }
        }
    }

    // ── Heater manual + temp-guarded auto stop ────────────────
    //    NC relay: COIL_OFF = heater runs, COIL_ON = heater stopped
    void _evaluateHeater(bool tempValid, float currentTemp, float setpt) {
        bool shouldStop = !hpSystem.heaterManualOn
                       || (tempValid && currentTemp >= setpt);

        RelayState target = shouldStop ? RelayState::OFF : RelayState::ON;

        if (hpSystem.heaterRelay != target) {
            digitalWrite(RELAY_HEATER_PIN, shouldStop ? COIL_ON : COIL_OFF);
            hpSystem.heaterRelay = target;
            logger.logf(LogLevel::INFO,
                "[Control] Heater %s — Temp=%.1f°C Setpt=%.1f°C Manual=%s",
                shouldStop ? "STOPPED" : "RUNNING",
                currentTemp, setpt,
                hpSystem.heaterManualOn ? "ON" : "OFF");
        }
    }

    // ── Helpers ───────────────────────────────────────────────
    void _stopHP(const char* reason) {
        if (_hpRunning || hpSystem.relay == RelayState::ON) {
            digitalWrite(RELAY_HP_PIN, COIL_ON);   // NC opens → HP stops
            hpSystem.relay = RelayState::OFF;
            _hpRunning     = false;
            _lastHpChange  = millis();
            logger.logf(LogLevel::INFO, "[Control] HP STOP — %s", reason);
        }
    }

    void _stopHeater(const char* reason) {
        if (hpSystem.heaterRelay == RelayState::ON) {
            digitalWrite(RELAY_HEATER_PIN, COIL_ON);
            hpSystem.heaterRelay    = RelayState::OFF;
            hpSystem.heaterManualOn = false;
            logger.logf(LogLevel::INFO, "[Control] Heater STOP — %s", reason);
        }
    }

    bool _hasCriticalAlarm() const {
        return hpSystem.alarms.overvoltage
            || hpSystem.alarms.overcurrent
            || hpSystem.alarms.highTemp
            || hpSystem.alarms.highPressure
            || hpSystem.alarms.lowPressure
            || hpSystem.alarms.criticalWaterLevel;
    }

    void _transitionTo(DeviceState next) {
        if (hpSystem.state == next) return;
        logger.logf(LogLevel::INFO, "[Control] State: %s → %s",
                    stateToStr(hpSystem.state), stateToStr(next));
        if (next == DeviceState::FAULT) _faultStart = millis();
        hpSystem.state = next;
    }
};

extern ControlEngine controlEngine;


