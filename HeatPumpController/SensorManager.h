#pragma once
#include <Arduino.h>
#include "SystemData.h"
#include "Logger.h"

// ============================================================
//  SENSOR MANAGER  —  HP & LP Safety Switches
//
//  Wiring:
//    Common -> GND
//    HP     -> GPIO 25
//    LP     -> GPIO 14
//
//  Logic:
//    Normally Closed (NC) switches.
//    Safe  = Switch closed -> Pin is pulled to GND (LOW)
//    Fault = Switch open   -> Internal Pull-up pulls to 3.3V (HIGH)
// ============================================================

#define HP_PIN  25
#define LP_PIN  14

class SensorManager {
public:
    void begin() {
        logger.info("[Sensors] Initialising SensorManager (HP/LP)...");
        
        // Enable internal pull-up resistors
        pinMode(HP_PIN, INPUT_PULLUP);
        pinMode(LP_PIN, INPUT_PULLUP);
        
        logger.logf(LogLevel::INFO, "[Sensors] HP mapped to GPIO %d, LP mapped to GPIO %d", HP_PIN, LP_PIN);
    }

    // Called by Scheduler every 1000ms
    void update() {
        // HIGH means the switch opened (fault), LOW means switch is closed to GND (safe)
        bool hpFault = digitalRead(HP_PIN) == HIGH;
        bool lpFault = digitalRead(LP_PIN) == HIGH;

        // Log state changes only (no serial spam)
        if (hpFault != hpSystem.alarms.highPressure) {
            if (hpFault) logger.error("[Sensors] HIGH PRESSURE FAULT DETECTED (Switch Open)!");
            else         logger.info("[Sensors] High Pressure restored to Normal.");
            hpSystem.alarms.highPressure = hpFault;
        }
        if (lpFault != hpSystem.alarms.lowPressure) {
            if (lpFault) logger.error("[Sensors] LOW PRESSURE FAULT DETECTED (Switch Open)!");
            else         logger.info("[Sensors] Low Pressure restored to Normal.");
            hpSystem.alarms.lowPressure = lpFault;
        }
    }
};

extern SensorManager sensorManager;
