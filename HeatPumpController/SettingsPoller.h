#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "ConfigProfile.h"
#include "SystemData.h"
#include "ControlEngine.h"
#include "Logger.h"

// ============================================================
//  SETTINGS POLLER
//
//  Every 5 seconds (scheduled via Scheduler) this module:
//
//    1. GET  <settingsApiUrl>?device_id=<id>
//       → server returns the latest PENDING row as JSON (or empty)
//
//    2. If a pending row exists:
//       a. Parse each field and apply it to configManager.config
//       b. Apply runtime-only flags (heater_manually_on, hp_manually_on)
//       c. Save to NVS via configManager.save()
//       d. POST acknowledgement back to <settingsAckUrl>
//          with { "id": <row_id>, "status": 1, "feedback": "applied" }
//
//  Column ↔ ConfigProfile mapping:
//  ─────────────────────────────────────────────────────────
//  set_temp            → config.tempSetpoint
//  hysteresis          → config.tempHysteresis
//  heater_setpoint     → config.heaterSetpoint
//  heater_hysteresis   → config.heaterHysteresis
//  heater_manually_on  → REMOVED: relay control must only come from local dashboard
//  hp_manually_on      → REMOVED: relay control must only come from local dashboard
//  control_sensor_idx  → config.controlSensorIdx
//  voltage_max         → config.voltageMax
//  voltage_min         → config.voltageMin
//  current_max         → config.currentMax
//  temp_high_limit     → config.tempHighLimit
//  temp_low_limit      → config.tempLowLimit
//  empty_dist_cm       → config.emptyDistanceCm
//  full_dist_cm        → config.fullDistanceCm
//  water_shutoff_pct   → config.waterShutoffPercent
//  water_low_alarm_pct → config.waterLowAlarmPercent
//  cloud_interval_ms   → config.cloudIntervalMs
// ============================================================

// ---- Status codes (mirror the DB tinyint) ----
static constexpr uint8_t SETTINGS_PENDING = 0;
static constexpr uint8_t SETTINGS_APPLIED = 1;
static constexpr uint8_t SETTINGS_FAILED  = 2;

struct SettingsPollStatus {
    bool     lastPollOk      = false;   // true if GET succeeded (even "no pending")
    bool     lastApplyOk     = false;   // true if a settings row was successfully applied
    uint32_t pollCount       = 0;       // total GET requests sent
    uint32_t applyCount      = 0;       // total rows successfully applied
    uint32_t applyFailCount  = 0;       // rows that failed to parse/apply
    uint32_t lastRowId       = 0;       // DB id of the last processed row
    unsigned long lastPollMs = 0;       // millis() of last successful GET
    unsigned long lastApplyMs= 0;       // millis() of last successful apply
    char     lastFeedback[64]= "";      // "applied" | error reason
};

extern SettingsPollStatus settingsPollStatus;

class SettingsPoller {
public:
    // --------------------------------------------------------
    //  poll() — called by Scheduler every 5 000 ms
    //  Requires WiFi to be up; silently skips if offline.
    // --------------------------------------------------------
    void poll() {
        if (WiFi.status() != WL_CONNECTED) {
            logger.warning("[Settings] WiFi not connected — skip poll.");
            return;
        }

        const char* deviceId = hpSystem.device.deviceId;
        if (strlen(deviceId) < 3) {
            logger.warning("[Settings] Device ID not set — skip poll.");
            return;
        }

        settingsPollStatus.pollCount++;
        settingsPollStatus.lastPollMs = millis();

        // ---- Build GET URL ----
        // Expected GET endpoint:
        //   GET http://yourserver.com/iot_api/get_settings.php?device_id=HP-AABBCC
        //
        // Server must return ONE of:
        //   a) {}                   — no pending settings
        //   b) { "id": 7,
        //        "set_temp": "55",
        //        "hysteresis": "5",
        //        ... all columns ... }
        String url = _buildGetUrl(deviceId);

        logger.logf(LogLevel::INFO, "[Settings] Polling: %s", url.c_str());

        String responseBody;
        bool   getOk = _httpGet(url, responseBody);

        if (!getOk) {
            settingsPollStatus.lastPollOk = false;
            logger.error("[Settings] GET failed.");
            return;
        }

        settingsPollStatus.lastPollOk = true;

        // ---- Parse response ----
        if (responseBody.length() < 5 || responseBody == "{}") {
            // No pending settings — normal case
            logger.info("[Settings] No pending settings.");
            return;
        }

        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, responseBody);
        if (err) {
            logger.logf(LogLevel::ERROR,
                "[Settings] JSON parse error: %s  body=%.60s",
                err.c_str(), responseBody.c_str());
            settingsPollStatus.applyFailCount++;
            return;
        }

        // If the server explicitly says success=false (e.g. "Device not found"), 
        // treat it as "no pending settings" and ignore silently.
        if (doc["success"].is<bool>() && doc["success"].as<bool>() == false) {
            return;
        }

        // Must have an 'id' field to acknowledge back
        if (!doc["id"].is<int>()) {
            logger.logf(LogLevel::ERROR,
                "[Settings] Missing 'id' in response. Body: %.60s",
                responseBody.c_str());
            settingsPollStatus.applyFailCount++;
            return;
        }

        uint32_t rowId = doc["id"].as<uint32_t>();

        // Avoid double-applying the same row (e.g. if server is slow to update status)
        if (rowId == settingsPollStatus.lastRowId) {
            logger.logf(LogLevel::INFO,
                "[Settings] Row #%u already applied — skip.", rowId);
            return;
        }

        // ---- Apply settings ----
        String feedback;
        bool applied = _applySettings(doc, feedback);

        settingsPollStatus.lastRowId = rowId;

        if (applied) {
            settingsPollStatus.applyCount++;
            settingsPollStatus.lastApplyOk = true;
            settingsPollStatus.lastApplyMs = millis();
            strlcpy(settingsPollStatus.lastFeedback, "applied",
                    sizeof(settingsPollStatus.lastFeedback));
            logger.logf(LogLevel::INFO,
                "[Settings] Row #%u applied & saved to NVS.", rowId);
        } else {
            settingsPollStatus.applyFailCount++;
            settingsPollStatus.lastApplyOk = false;
            strlcpy(settingsPollStatus.lastFeedback, feedback.c_str(),
                    sizeof(settingsPollStatus.lastFeedback));
            logger.logf(LogLevel::ERROR,
                "[Settings] Row #%u apply failed: %s", rowId, feedback.c_str());
        }

        // ---- Send acknowledgement (fire-and-forget) ----
        _sendAck(rowId, applied ? SETTINGS_APPLIED : SETTINGS_FAILED, feedback);
    }

private:
    // --------------------------------------------------------
    //  Build the GET URL
    //  Server: GET get_settings.php?device_id=<id>
    //  Returns latest PENDING row as JSON, or {} if none.
    // --------------------------------------------------------
    String _buildGetUrl(const char* deviceId) {
        String base      = String(configManager.config.apiUrl);
        int    lastSlash = base.lastIndexOf('/');
        String host      = (lastSlash >= 0) ? base.substring(0, lastSlash + 1)
                                            : base + "/";
        return host + "get_settings.php?device_id=" + String(deviceId);
    }

    // --------------------------------------------------------
    //  Build the ACK POST URL
    //  Server: POST update_settings_feedback.php?id=<rowId>
    //  The row ID goes in the query string, not the body.
    // --------------------------------------------------------
    String _buildAckUrl(uint32_t rowId) {
        String base      = String(configManager.config.apiUrl);
        int    lastSlash = base.lastIndexOf('/');
        String host      = (lastSlash >= 0) ? base.substring(0, lastSlash + 1)
                                            : base + "/";
        return host + "update_settings_feedback.php?id=" + String(rowId);
    }

    // --------------------------------------------------------
    //  HTTP GET helper — returns response body in `out`
    // --------------------------------------------------------
    bool _httpGet(const String& url, String& out) {
        HTTPClient http;
        http.begin(url);
        http.setTimeout(8000);
        if (strlen(configManager.config.apiKey) > 0) {
            http.addHeader("X-API-Key", configManager.config.apiKey);
        }
        int code = http.GET();
        if (code >= 200 && code < 300) {
            out = http.getString();
            http.end();
            return true;
        }
        logger.logf(LogLevel::ERROR, "[Settings] GET HTTP %d", code);
        http.end();
        return false;
    }

    // --------------------------------------------------------
    //  HTTP POST helper — sends JSON body
    // --------------------------------------------------------
    bool _httpPost(const String& url, const String& body) {
        HTTPClient http;
        http.begin(url);
        http.addHeader("Content-Type", "application/json");
        http.setTimeout(8000);
        if (strlen(configManager.config.apiKey) > 0) {
            http.addHeader("X-API-Key", configManager.config.apiKey);
        }
        int code = http.POST((uint8_t*)body.c_str(), body.length());
        String resp = http.getString();
        http.end();
        if (code >= 200 && code < 300) {
            logger.logf(LogLevel::INFO,
                "[Settings] ACK POST OK  resp=%.60s", resp.c_str());
            return true;
        }
        logger.logf(LogLevel::ERROR,
            "[Settings] ACK POST HTTP %d  resp=%.60s", code, resp.c_str());
        return false;
    }

    // --------------------------------------------------------
    //  Apply a parsed JSON settings row to ConfigProfile + NVS
    //
    //  All numeric fields arrive as strings (varchar in DB) OR
    //  as JSON numbers — we handle both safely.
    //
    //  Returns true on success; populates `feedback` with status.
    // --------------------------------------------------------
    bool _applySettings(JsonDocument& doc, String& feedback) {
        ConfigProfile& c = configManager.config;

        // ── Helper lambdas ────────────────────────────────────
        // Safely read a float from either a JSON string or number.
        auto getFloat = [&](const char* key, float fallback) -> float {
            if (doc[key].is<float>())       return doc[key].as<float>();
            if (doc[key].is<const char*>()) {
                String s = doc[key].as<String>();
                if (s.length() > 0) return s.toFloat();
            }
            return fallback;
        };
        auto getInt = [&](const char* key, int fallback) -> int {
            if (doc[key].is<int>())         return doc[key].as<int>();
            if (doc[key].is<const char*>()) {
                String s = doc[key].as<String>();
                if (s.length() > 0) return s.toInt();
            }
            return fallback;
        };
        auto getBool = [&](const char* key, bool fallback) -> bool {
            if (doc[key].is<bool>())        return doc[key].as<bool>();
            if (doc[key].is<int>())         return doc[key].as<int>() != 0;
            if (doc[key].is<const char*>()) {
                String s = doc[key].as<String>();
                return (s == "1" || s.equalsIgnoreCase("true") ||
                        s.equalsIgnoreCase("yes") || s.equalsIgnoreCase("on"));
            }
            return fallback;
        };

        // ── Temperature Control ───────────────────────────────
        if (doc.containsKey("set_temp")) {
            float v = getFloat("set_temp", c.tempSetpoint);
            v = constrain(v, 5.0f, 90.0f);           // sanity: 5–90 °C
            c.tempSetpoint = v;
        }
        if (doc.containsKey("hysteresis")) {
            float v = getFloat("hysteresis", c.tempHysteresis);
            v = constrain(v, 0.5f, 20.0f);
            c.tempHysteresis = v;
        }
        if (doc.containsKey("heater_setpoint")) {
            float v = getFloat("heater_setpoint", c.heaterSetpoint);
            v = constrain(v, 5.0f, 90.0f);
            c.heaterSetpoint = v;
        }
        if (doc.containsKey("heater_hysteresis")) {
            float v = getFloat("heater_hysteresis", c.heaterHysteresis);
            v = constrain(v, 0.5f, 20.0f);
            c.heaterHysteresis = v;
        }

        // ── Device Enable Toggles (from remote server) ────────
        if (doc.containsKey("hp_manually_on")) {
            bool enabled = getBool("hp_manually_on", hpSystem.hpEnabled);
            controlEngine.setHPEnabled(enabled, "Cloud");
        }
        if (doc.containsKey("heater_manually_on")) {
            bool enabled = getBool("heater_manually_on", hpSystem.heaterEnabled);
            controlEngine.setHeaterEnabled(enabled, "Cloud");
        }

        // ── Control Sensor Selection ──────────────────────────
        if (doc.containsKey("control_sensor_idx")) {
            int idx = getInt("control_sensor_idx", 0);
            c.controlSensorIdx = (uint8_t)constrain(idx, 0, 7);
        }

        // ── PZEM Protection Thresholds ────────────────────────
        if (doc.containsKey("voltage_max")) {
            float v = getFloat("voltage_max", c.voltageMax);
            c.voltageMax = constrain(v, 100.0f, 280.0f);
        }
        if (doc.containsKey("voltage_min")) {
            float v = getFloat("voltage_min", c.voltageMin);
            c.voltageMin = constrain(v, 100.0f, 260.0f);
        }
        if (doc.containsKey("current_max")) {
            float v = getFloat("current_max", c.currentMax);
            c.currentMax = constrain(v, 1.0f, 100.0f);
        }

        // ── Temperature Alarm Limits ──────────────────────────
        if (doc.containsKey("temp_high_limit")) {
            float v = getFloat("temp_high_limit", c.tempHighLimit);
            c.tempHighLimit = constrain(v, 30.0f, 120.0f);
        }
        if (doc.containsKey("temp_low_limit")) {
            float v = getFloat("temp_low_limit", c.tempLowLimit);
            c.tempLowLimit = constrain(v, -30.0f, 30.0f);
        }

        // ── Water Tank Parameters ─────────────────────────────
        if (doc.containsKey("empty_dist_cm")) {
            float v = getFloat("empty_dist_cm", c.emptyDistanceCm);
            c.emptyDistanceCm = constrain(v, 1.0f, 500.0f);
        }
        if (doc.containsKey("full_dist_cm")) {
            float v = getFloat("full_dist_cm", c.fullDistanceCm);
            c.fullDistanceCm = constrain(v, 1.0f, 500.0f);
        }
        if (doc.containsKey("water_shutoff_pct")) {
            float v = getFloat("water_shutoff_pct", c.waterShutoffPercent);
            c.waterShutoffPercent = constrain(v, 0.0f, 100.0f);
        }
        if (doc.containsKey("water_low_alarm_pct")) {
            float v = getFloat("water_low_alarm_pct", c.waterLowAlarmPercent);
            c.waterLowAlarmPercent = constrain(v, 0.0f, 100.0f);
        }

        // ── Sampling Intervals ────────────────────────────────
        if (doc.containsKey("cloud_interval_ms")) {
            int ms = getInt("cloud_interval_ms", (int)c.cloudIntervalMs);
            c.cloudIntervalMs = (uint32_t)constrain(ms, 1000, 300000);
        }

        // ── Persist everything to NVS ─────────────────────────
        configManager.save();

        feedback = "applied: set_temp=" + String(c.tempSetpoint, 1)
                 + " hyst=" + String(c.tempHysteresis, 1)
                 + " htr_set=" + String(c.heaterSetpoint, 1)
                 + " sensor_idx=" + String(c.controlSensorIdx);

        return true;
    }

    // --------------------------------------------------------
    //  Send acknowledgement back to server
    //  POST https://…/update_settings_feedback.php?id=<rowId>
    //  Body: { "device_id": "...", "status": 1,
    //          "feedback": "applied: ..." }
    // --------------------------------------------------------
    void _sendAck(uint32_t rowId, uint8_t status, const String& feedback) {
        // Row ID goes in the query string as the server requires
        String ackUrl = _buildAckUrl(rowId);

        JsonDocument ack;
        ack["device_id"] = hpSystem.device.deviceId;
        ack["status"]    = status;
        ack["feedback"]  = feedback;

        String ackBody;
        serializeJson(ack, ackBody);

        bool ok = _httpPost(ackUrl, ackBody);
        if (!ok) {
            logger.logf(LogLevel::WARNING,
                "[Settings] ACK POST failed for row #%u — server may still show PENDING.", rowId);
        }
    }
};

// ---- Global singleton — defined in HeatPumpController.ino ----
SettingsPollStatus settingsPollStatus;
extern SettingsPoller settingsPoller;
