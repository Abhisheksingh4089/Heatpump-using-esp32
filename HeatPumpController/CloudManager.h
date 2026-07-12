#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "SystemData.h"
#include "ConfigProfile.h"
#include "Logger.h"
#include "SimManager.h"

// ============================================================
//  CLOUD MANAGER
//
//  Sends telemetry JSON to the configured API endpoint.
//
//  Path priority:
//    1. WiFi  (fast — Arduino HTTPClient)
//    2. GPRS  (SIM800L fallback when WiFi is down)
//
//  JSON schema matches telemetry_payload.json v1.0
// ============================================================

struct CloudTxStatus {
    bool     lastPostOk     = false;
    uint16_t wifiPosts      = 0;
    uint16_t wifiPostFails  = 0;
    uint16_t gprsPosts      = 0;
    uint16_t gprsPostFails  = 0;
    unsigned long lastPostMs = 0;
    char     lastPath[8]    = "---";  // "WiFi", "GPRS", "NONE", "FAIL"
};

extern CloudTxStatus cloudTxStatus;

class CloudManager {
public:
    // -----------------------------------------------------------
    //  send() — called by Scheduler every cloudIntervalMs
    // -----------------------------------------------------------
    void send() {
        const char* url = configManager.config.apiUrl;

        // --- DEBUG (remove after confirmed working) ---
        Serial.printf("[Cloud] send() called | WiFi=%s | URL='%s'\n",
                      WiFi.status() == WL_CONNECTED ? "UP" : "DOWN", url);
        // ----------------------------------------------

        if (strlen(url) < 10) {
            Serial.println("[Cloud] ERROR: API URL is empty in NVS! Resetting config defaults...");
            configManager.resetDefaults();  // wipe bad NVS data, restore URL default
            url = configManager.config.apiUrl;
        }

        String payload = _buildPayload();

        // ---- Path 1: WiFi ----
        if (WiFi.status() == WL_CONNECTED) {
            bool ok = _postViaWiFi(url, payload);
            if (ok) {
                cloudTxStatus.wifiPosts++;
                cloudTxStatus.lastPostOk = true;
                cloudTxStatus.lastPostMs = millis();
                strlcpy(cloudTxStatus.lastPath, "WiFi", sizeof(cloudTxStatus.lastPath));
                logger.logf(LogLevel::INFO, "[Cloud] WiFi POST OK (#%u)", cloudTxStatus.wifiPosts);
                return;
            }
            cloudTxStatus.wifiPostFails++;
            logger.warning("[Cloud] WiFi POST failed. Trying GPRS fallback...");
        } else {
            logger.warning("[Cloud] WiFi not connected. Trying GPRS...");
        }

        // ---- Path 2: GPRS fallback ----
        if (simStatus.gprsConnected) {
            bool ok = simManager.postTelemetryPayload(payload, url);
            if (ok) {
                cloudTxStatus.gprsPosts++;
                cloudTxStatus.lastPostOk = true;
                cloudTxStatus.lastPostMs = millis();
                strlcpy(cloudTxStatus.lastPath, "GPRS", sizeof(cloudTxStatus.lastPath));
                logger.logf(LogLevel::INFO, "[Cloud] GPRS POST OK (#%u)", cloudTxStatus.gprsPosts);
            } else {
                cloudTxStatus.gprsPostFails++;
                cloudTxStatus.lastPostOk = false;
                strlcpy(cloudTxStatus.lastPath, "FAIL", sizeof(cloudTxStatus.lastPath));
                logger.error("[Cloud] GPRS POST also failed.");
            }
        } else {
            cloudTxStatus.lastPostOk = false;
            strlcpy(cloudTxStatus.lastPath, "NONE", sizeof(cloudTxStatus.lastPath));
            logger.error("[Cloud] No network path available.");
        }
    }

private:
    // -----------------------------------------------------------
    //  WiFi POST via Arduino HTTPClient
    // -----------------------------------------------------------
    bool _postViaWiFi(const char* url, const String& payload) {
        HTTPClient http;
        http.begin(url);
        http.addHeader("Content-Type", "application/json");
        http.setTimeout(10000);
        if (strlen(configManager.config.apiKey) > 0) {
            http.addHeader("X-API-Key", configManager.config.apiKey);
        }

        int code = http.POST((uint8_t*)payload.c_str(), payload.length());
        String resp = http.getString();
        http.end();

        if (code >= 200 && code < 300) {
            logger.logf(LogLevel::INFO, "[Cloud] HTTP %d  resp: %s",
                        code, resp.substring(0, 80).c_str());
            return true;
        }
        logger.logf(LogLevel::ERROR, "[Cloud] HTTP error %d  resp: %s",
                    code, resp.substring(0, 80).c_str());
        return false;
    }

    // -----------------------------------------------------------
    //  Build the telemetry payload — matches telemetry_payload.json v1.0
    // -----------------------------------------------------------
    String _buildPayload() {
        JsonDocument doc;

        doc["schemaVersion"] = "1.0";
        doc["deviceId"]      = hpSystem.device.deviceId;
        doc["timestamp"]     = _isoTimestamp();

        // ── system ───────────────────────────────────────────
        // No sensor deps — always safe.
        JsonObject sys = doc["system"].to<JsonObject>();
        bool relayOn = (hpSystem.relay == RelayState::ON);
        bool heaterRelayOn = (hpSystem.heaterRelay == RelayState::ON);
        sys["heatPumpRunning"]  = relayOn;
        sys["heaterOn"]         = heaterRelayOn;   // Fixed: now reading actual heater relay
        sys["operatingMode"]    = relayOn ? "HEATING" : "STANDBY";
        sys["relayOn"]          = relayOn;
        sys["deviceState"]      = stateToStr(hpSystem.state);
        sys["phaseType"]        = (int)hpSystem.pzemCount;
        sys["firmwareVersion"]  = hpSystem.device.firmwareVersion;
        sys["uptimeSeconds"]    = hpSystem.health.uptime;

        // ── temperature ──────────────────────────────────────
        // Guards: check tempCount AND online flag before using value.
        // If sensor absent or offline → send null so backend can distinguish
        // "sensor not fitted" from a real 0°C reading.
        JsonObject temp = doc["temperature"].to<JsonObject>();
        temp["targetTemperature"] = (double)configManager.config.tempSetpoint;

        // Each sensor: use real value if wired+online, JSON null otherwise.
        // nullptr = proper JSON null in ArduinoJson v7.
        if (hpSystem.tempCount > 0 && hpSystem.temps[0].online)
            temp["inletTemperature"] = hpSystem.temps[0].value;
        else
            temp["inletTemperature"] = nullptr;

        if (hpSystem.tempCount > 1 && hpSystem.temps[1].online)
            temp["outletTemperature"] = hpSystem.temps[1].value;
        else
            temp["outletTemperature"] = nullptr;

        if (hpSystem.tempCount > 2 && hpSystem.temps[2].online)
            temp["ambientTemperature"] = hpSystem.temps[2].value;
        else
            temp["ambientTemperature"] = nullptr;

        // ── power (named phases) ─────────────────────────────
        // Guard: loop only up to pzemCount, cap at 3.
        // Each phase also includes its 'online' flag so backend knows
        // if the PZEM is actually responding.
        JsonObject power = doc["power"].to<JsonObject>();
        const char* phaseNames[] = {"phase1", "phase2", "phase3"};
        uint8_t phaseCount = min((uint8_t)hpSystem.pzemCount, (uint8_t)3);
        for (uint8_t i = 0; i < phaseCount; i++) {
            const PZEMData& p = hpSystem.pzem[i];
            JsonObject ph = power[phaseNames[i]].to<JsonObject>();
            ph["online"] = p.online;
            if (p.online) {
                ph["voltageVolts"] = p.voltage;
                ph["currentAmps"]  = p.current;
                ph["powerWatts"]   = p.power;
                ph["energyKwh"]    = p.energy;
                ph["frequencyHz"]  = p.frequency;
                ph["powerFactor"]  = p.powerFactor;
            } else {
                ph["voltageVolts"] = nullptr;
                ph["currentAmps"]  = nullptr;
                ph["powerWatts"]   = nullptr;
                ph["energyKwh"]    = nullptr;
                ph["frequencyHz"]  = nullptr;
                ph["powerFactor"]  = nullptr;
            }
        }

        // ── summary ───────────────────────────────────────────
        // Guard: only sum phases that are online. If none → all zeros.
        // Divide by onlineCount (never 0 thanks to max(...,1) guard).
        float totalPower = 0, totalEnergy = 0;
        float totalVoltage = 0, totalCurrent = 0, totalPF = 0;
        float maxPower = 0, minPower = 0;
        uint8_t onlineCount = 0;
        for (uint8_t i = 0; i < phaseCount; i++) {
            if (!hpSystem.pzem[i].online) continue;
            const PZEMData& p = hpSystem.pzem[i];
            totalPower   += p.power;
            totalEnergy  += p.energy;
            totalVoltage += p.voltage;
            totalCurrent += p.current;
            totalPF      += p.powerFactor;
            if (onlineCount == 0 || p.power > maxPower) maxPower = p.power;
            if (onlineCount == 0 || p.power < minPower) minPower = p.power;
            onlineCount++;
        }
        uint8_t div = max(onlineCount, (uint8_t)1);  // never divide by 0
        float avgVoltage = totalVoltage / div;
        float avgCurrent = totalCurrent / div;
        float avgPF      = totalPF / div;
        float avgPower   = totalPower / div;
        float imbalance  = (avgPower > 0.0f)
                           ? ((maxPower - minPower) / avgPower) * 100.0f : 0.0f;

        JsonObject summary = doc["summary"].to<JsonObject>();
        summary["totalPowerWatts"]       = serialized(String(totalPower,  1));
        summary["totalEnergyKwh"]        = serialized(String(totalEnergy, 3));
        summary["averageVoltageVolts"]   = serialized(String(avgVoltage,  1));
        summary["averageCurrentAmps"]    = serialized(String(avgCurrent,  2));
        summary["averagePowerFactor"]    = serialized(String(avgPF,       3));
        summary["phaseImbalancePercent"] = serialized(String(imbalance,   1));

        // ── waterTank ─────────────────────────────────────────
        // Send real values from WaterManager when sensor is online,
        // null when offline so backend can distinguish "no sensor" vs 0%.
        JsonObject water = doc["waterTank"].to<JsonObject>();
        if (hpSystem.water.sensorOnline) {
            water["sensorDistanceCm"]  = hpSystem.water.sensorDistanceCm;
            water["waterLevelPercent"] = hpSystem.water.waterLevelPercent;
        } else {
            water["sensorDistanceCm"]  = nullptr;
            water["waterLevelPercent"] = nullptr;
        }
        water["waterLevelState"]      = hpSystem.water.waterLevelState;
        water["flowRateLitersPerMin"] = nullptr;   // flow meter not wired

        // ── compressor ────────────────────────────────────────
        JsonObject comp = doc["compressor"].to<JsonObject>();
        comp["compressorRunning"]  = relayOn;
        comp["totalRuntimeHours"]  = (float)hpSystem.health.uptime / 3600.0f;

        // ── safety ────────────────────────────────────────────
        // HP/LP switches are wired — use real values.
        // Flow / water level not wired yet — default safe (true).
        JsonObject safety = doc["safety"].to<JsonObject>();
        safety["highPressureOk"]    = !hpSystem.alarms.highPressure;
        safety["lowPressureOk"]     = !hpSystem.alarms.lowPressure;
        safety["phaseLossDetected"] = hpSystem.alarms.pzemOffline;
        safety["waterFlowOk"]       = true;   // not wired
        safety["waterLevelSafe"]    = true;   // not wired

        // ── alarms ────────────────────────────────────────────
        JsonObject alarms = doc["alarms"].to<JsonObject>();
        alarms["lowWaterAlarm"]    = false;                         // not wired
        alarms["highPressureTrip"] = hpSystem.alarms.highPressure;
        alarms["lowPressureTrip"]  = hpSystem.alarms.lowPressure;
        alarms["waterFlowFailure"] = false;                         // not wired
        alarms["phaseFailure"]     = hpSystem.alarms.pzemOffline;
        alarms["overvoltage"]      = hpSystem.alarms.overvoltage;
        alarms["undervoltage"]     = hpSystem.alarms.undervoltage;
        alarms["overcurrent"]      = hpSystem.alarms.overcurrent;
        alarms["highTemperature"]  = hpSystem.alarms.highTemp;
        alarms["lowTemperature"]   = hpSystem.alarms.lowTemp;

        // ── network ───────────────────────────────────────────
        JsonObject net = doc["network"].to<JsonObject>();
        net["wifiSignalStrength"] = hpSystem.wifi.rssi;
        net["ipAddress"]          = hpSystem.wifi.connected
                                    ? hpSystem.wifi.ip : simStatus.localIp;
        net["timeSynced"]         = hpSystem.wifi.ntpSynced;

        String out;
        serializeJson(doc, out);
        return out;
    }

    // -----------------------------------------------------------
    //  Returns ISO 8601 timestamp if NTP is synced, else uptime
    // -----------------------------------------------------------
    String _isoTimestamp() {
        extern HPNetworkManager networkManager;
        unsigned long epoch = networkManager.epochTime();
        if (epoch > 0) {
            // Format: 2026-06-30T16:30:00Z
            time_t t = (time_t)epoch;
            struct tm* tm_info = gmtime(&t);
            char buf[25];
            strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", tm_info);
            return String(buf);
        }
        // Fallback: uptime in seconds
        return String(millis() / 1000) + "s_uptime";
    }
};

// Global singletons — defined in HeatPumpController.ino
CloudTxStatus cloudTxStatus;
extern CloudManager cloudManager;
