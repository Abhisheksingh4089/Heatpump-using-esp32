#pragma once
#include <Arduino.h>
#include "SystemData.h"
#include "ConfigProfile.h"
#include "Logger.h"

class AlarmManager {
public:
    void begin() { logger.info("[Alarm] AlarmManager initialised."); }

    void evaluate() {
        _checkPZEM();
        _checkTemperature();
        _checkWaterLevel();
    }

    String activeAlarmsJson() const {
        String result = "[";
        bool first = true;
        auto append = [&](const char* name) {
            if (!first) result += ",";
            result += "\""; result += name; result += "\"";
            first = false;
        };
        if (hpSystem.alarms.pzemOffline)      append("PZEM_OFFLINE");
        if (hpSystem.alarms.tempSensorLost)   append("TEMP_SENSOR_LOST");
        if (hpSystem.alarms.overvoltage)      append("OVERVOLTAGE");
        if (hpSystem.alarms.undervoltage)     append("UNDERVOLTAGE");
        if (hpSystem.alarms.overcurrent)      append("OVERCURRENT");
        if (hpSystem.alarms.highTemp)         append("HIGH_TEMP");
        if (hpSystem.alarms.lowTemp)          append("LOW_TEMP");
        if (hpSystem.alarms.highPressure)     append("HIGH_PRESSURE");
        if (hpSystem.alarms.lowPressure)      append("LOW_PRESSURE");
        if (hpSystem.alarms.wifiLost)         append("WIFI_LOST");
        if (hpSystem.alarms.cloudUnreachable) append("CLOUD_UNREACHABLE");
        if (hpSystem.alarms.lowWaterLevel)    append("LOW_WATER");
        if (hpSystem.alarms.criticalWaterLevel) append("CRITICAL_WATER");
        result += "]";
        return result;
    }

private:
    void _checkPZEM() {
        const ConfigProfile& cfg = configManager.config;
        for (uint8_t i = 0; i < hpSystem.pzemCount; i++) {
            const PZEMData& p = hpSystem.pzem[i];
            if (!p.online) continue;

            hpSystem.alarms.overvoltage  = (p.voltage > cfg.voltageMax);
            if (hpSystem.alarms.overvoltage)
                logger.logf(LogLevel::WARNING,"[Alarm] OVERVOLTAGE: %.1fV", p.voltage);

            hpSystem.alarms.undervoltage = (p.voltage < cfg.voltageMin && p.voltage > 10.0f);
            if (hpSystem.alarms.undervoltage)
                logger.logf(LogLevel::WARNING,"[Alarm] UNDERVOLTAGE: %.1fV", p.voltage);

            hpSystem.alarms.overcurrent  = (p.current > cfg.currentMax);
            if (hpSystem.alarms.overcurrent)
                logger.logf(LogLevel::WARNING,"[Alarm] OVERCURRENT: %.2fA", p.current);
        }
    }

    void _checkTemperature() {
        const ConfigProfile& cfg = configManager.config;
        for (uint8_t i = 0; i < hpSystem.tempCount; i++) {
            const TempSensor& s = hpSystem.temps[i];
            if (!s.online) continue;

            hpSystem.alarms.highTemp = (s.value > cfg.tempHighLimit);
            if (hpSystem.alarms.highTemp)
                logger.logf(LogLevel::ERROR,"[Alarm] HIGH TEMP: %.1f°C", s.value);

            hpSystem.alarms.lowTemp  = (s.value < cfg.tempLowLimit);
            if (hpSystem.alarms.lowTemp)
                logger.logf(LogLevel::WARNING,"[Alarm] LOW TEMP: %.1f°C", s.value);
        }
    }

    void _checkWaterLevel() {
        const WaterTankData& w   = hpSystem.water;
        const ConfigProfile& cfg = configManager.config;

        // If sensor hasn't had its first reading yet, skip
        if (!w.sensorOnline && w.lastUpdate == 0) return;

        float pct = w.waterLevelPercent;

        bool prevLow      = hpSystem.alarms.lowWaterLevel;
        bool prevCritical = hpSystem.alarms.criticalWaterLevel;

        // Sensor error is already handled in WaterManager — criticalWaterLevel
        // is set there. Only update normal threshold alarms here.
        if (w.sensorOnline) {
            hpSystem.alarms.lowWaterLevel      = (pct < cfg.waterLowAlarmPercent);
            hpSystem.alarms.criticalWaterLevel = (pct < cfg.waterShutoffPercent);
        }

        // Log only on state change (avoid serial spam every second)
        if (hpSystem.alarms.lowWaterLevel && !prevLow)
            logger.logf(LogLevel::WARNING,
                "[Alarm] LOW WATER: %.1f%% (threshold: %.0f%%)",
                pct, cfg.waterLowAlarmPercent);

        if (!hpSystem.alarms.lowWaterLevel && prevLow)
            logger.logf(LogLevel::INFO,
                "[Alarm] Low water alarm cleared. Level now %.1f%%", pct);

        if (hpSystem.alarms.criticalWaterLevel && !prevCritical)
            logger.logf(LogLevel::ERROR,
                "[Alarm] 🚨 CRITICAL WATER: %.1f%% — Heat pump shutoff triggered!",
                pct);

        if (!hpSystem.alarms.criticalWaterLevel && prevCritical)
            logger.logf(LogLevel::INFO,
                "[Alarm] Critical water alarm cleared. Level now %.1f%%", pct);
    }
};

extern AlarmManager alarmManager;
