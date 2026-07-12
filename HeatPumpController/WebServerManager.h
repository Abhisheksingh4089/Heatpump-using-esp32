#pragma once
#include <Arduino.h>
#include <WebServer.h>        // Built-in ESP32 library — no extra install needed!
#include <ArduinoJson.h>
#include "SystemData.h"
#include "ConfigProfile.h"
#include "AlarmManager.h"
#include "ControlEngine.h"
#include "Logger.h"
#include "WebUI.h"
#include "SimManager.h"
#include "SettingsPoller.h"

// ============================================================
//  WEB SERVER MANAGER
//  Uses the built-in ESP32 WebServer (fully compatible with
//  ESP32 Arduino Core 3.x — no threading/TCP lock issues).
//
//  NOTE: Unlike ESPAsyncWebServer, this server is synchronous.
//  It requires server.handleClient() to be called regularly.
//  This is done via the "WebServer" task in the Scheduler.
// ============================================================

class WebServerManager {
public:
    void begin() {
        _server = new WebServer(80);
        _registerRoutes();
        _server->begin();
        logger.info("[Web] WebServer started on port 80.");
        logger.logf(LogLevel::INFO, "[Web] Dashboard: http://%s", hpSystem.wifi.ip);
    }

    // Must be called frequently — registered as a Scheduler task
    void handle() {
        if (_server) _server->handleClient();
    }

private:
    WebServer* _server = nullptr;

    void _registerRoutes() {

        // ---- Dashboard HTML ----
        _server->on("/", HTTP_GET, [this]() {
            _server->send_P(200, "text/html", DASHBOARD_HTML);
        });

        // ---- GET /api/info ----
        _server->on("/api/info", HTTP_GET, [this]() {
            JsonDocument doc;
            doc["device_id"]    = hpSystem.device.deviceId;
            doc["serial"]       = configManager.config.serialNumber;
            doc["mac"]          = hpSystem.device.macAddress;
            doc["firmware"]     = hpSystem.device.firmwareVersion;
            doc["hardware"]     = hpSystem.device.hardwareVersion;
            doc["protocol"]     = hpSystem.device.protocolVersion;
            doc["customer"]     = configManager.config.customerName;
            doc["alias"]        = configManager.config.deviceAlias;
            doc["address"]      = configManager.config.installAddress;
            doc["installer"]    = configManager.config.installerName;
            String body;
            serializeJson(doc, body);
            _server->send(200, "application/json", body);
        });

        // ---- GET /api/live ----
        _server->on("/api/live", HTTP_GET, [this]() {
            _server->send(200, "application/json", _buildLiveJson());
        });

        // ---- GET /api/health ----
        _server->on("/api/health", HTTP_GET, [this]() {
            hpSystem.health.freeHeap    = ESP.getFreeHeap();
            hpSystem.health.minFreeHeap = ESP.getMinFreeHeap();
            hpSystem.health.cpuFreqMHz  = (float)ESP.getCpuFreqMHz();

            JsonDocument doc;
            doc["uptime_sec"]     = hpSystem.health.uptime;
            doc["free_heap"]      = hpSystem.health.freeHeap;
            doc["min_free_heap"]  = hpSystem.health.minFreeHeap;
            doc["cpu_mhz"]        = hpSystem.health.cpuFreqMHz;
            doc["restart_count"]  = hpSystem.health.restartCount;
            doc["wifi_rssi"]       = hpSystem.wifi.rssi;
            doc["wifi_connected"]  = hpSystem.wifi.connected;
            doc["ntp_synced"]      = hpSystem.wifi.ntpSynced;
            doc["cloud_queue"]     = hpSystem.cloud.queueDepth;
            doc["gsm_connected"]   = simStatus.gprsConnected;
            doc["gsm_csq"]         = simStatus.csq;
            doc["gsm_operator"]    = simStatus.operator_;
            doc["gsm_ip"]          = simStatus.localIp;
            doc["post_ok"]         = simStatus.postCount;
            doc["post_fail"]       = simStatus.postFails;
            // ── Remote Settings Poller ────────────────────────
            doc["settings_poll_ok"]      = settingsPollStatus.lastPollOk;
            doc["settings_apply_count"]  = settingsPollStatus.applyCount;
            doc["settings_fail_count"]   = settingsPollStatus.applyFailCount;
            doc["settings_last_row_id"]  = settingsPollStatus.lastRowId;
            doc["settings_last_feedback"]= settingsPollStatus.lastFeedback;
            doc["settings_poll_count"]   = settingsPollStatus.pollCount;
            String body;
            serializeJson(doc, body);
            _server->send(200, "application/json", body);
        });

        // ---- GET /api/logs ----
        _server->on("/api/logs", HTTP_GET, [this]() {
            JsonDocument doc;
            JsonArray arr = doc["logs"].to<JsonArray>();
            logger.forEach([&arr](const LogEntry& e) {
                JsonObject entry = arr.add<JsonObject>();
                entry["ts"]    = e.timestamp;
                entry["level"] = logLevelStr(e.level);
                entry["msg"]   = e.message;
            });
            String body;
            serializeJson(doc, body);
            _server->send(200, "application/json", body);
        });

        // ---- POST /api/control ----
        _server->on("/api/control", HTTP_POST, [this]() {
            if (!_server->hasArg("plain")) {
                _server->send(400, "application/json", "{\"error\":\"No body\"}");
                return;
            }
            JsonDocument doc;
            if (deserializeJson(doc, _server->arg("plain")) != DeserializationError::Ok) {
                _server->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
                return;
            }
            if (doc["mode"].is<const char*>()) {
                String mode = doc["mode"].as<String>();
                if (mode == "manual")     controlEngine.setManualMode(true,  "WebDash");
                else if (mode == "auto")  controlEngine.setManualMode(false, "WebDash");
                else if (mode == "reset") controlEngine.clearLockout();
            }
            if (doc["heater"].is<bool>()) {
                controlEngine.setHeaterManual(doc["heater"].as<bool>(), "WebDash");
            }
            if (doc["hp_manual"].is<bool>()) {
                controlEngine.setHPManual(doc["hp_manual"].as<bool>(), "WebDash");
            }
            logger.info("[Web] Control command received.");
            _server->send(200, "application/json", "{\"status\":\"ok\"}");
        });

        // ---- POST /api/config ----
        _server->on("/api/config", HTTP_POST, [this]() {
            if (!_server->hasArg("plain")) {
                _server->send(400, "application/json", "{\"error\":\"No body\"}");
                return;
            }
            JsonDocument doc;
            if (deserializeJson(doc, _server->arg("plain")) != DeserializationError::Ok) {
                _server->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
                return;
            }
            ConfigProfile& c = configManager.config;
            if (doc["voltage_max"].is<float>())    c.voltageMax    = doc["voltage_max"];
            if (doc["voltage_min"].is<float>())    c.voltageMin    = doc["voltage_min"];
            if (doc["current_max"].is<float>())    c.currentMax    = doc["current_max"];
            if (doc["temp_high"].is<float>())      c.tempHighLimit = doc["temp_high"];
            if (doc["temp_low"].is<float>())       c.tempLowLimit  = doc["temp_low"];
            if (doc["temp_setpoint"].is<float>())    c.tempSetpoint    = doc["temp_setpoint"];
            if (doc["temp_hysteresis"].is<float>())  c.tempHysteresis  = doc["temp_hysteresis"];
            if (doc["heater_setpoint"].is<float>())  c.heaterSetpoint  = doc["heater_setpoint"];
            if (doc["control_sensor_idx"].is<int>()) c.controlSensorIdx = (uint8_t)constrain((int)doc["control_sensor_idx"], 0, 7);
            if (doc["api_url"].is<const char*>())
                strlcpy(c.apiUrl, doc["api_url"], sizeof(c.apiUrl));
            // Water tank parameters
            if (doc["empty_dist_cm"].is<float>())       c.emptyDistanceCm      = doc["empty_dist_cm"];
            if (doc["full_dist_cm"].is<float>())        c.fullDistanceCm       = doc["full_dist_cm"];
            if (doc["water_shutoff_pct"].is<float>())   c.waterShutoffPercent  = doc["water_shutoff_pct"];
            if (doc["water_low_alarm_pct"].is<float>()) c.waterLowAlarmPercent = doc["water_low_alarm_pct"];
            configManager.save();
            logger.info("[Web] Config updated via API.");
            _server->send(200, "application/json", "{\"status\":\"saved\"}");
        });
        // ---- POST /api/wifi ----
        _server->on("/api/wifi", HTTP_POST, [this]() {
            if (!_server->hasArg("plain")) {
                _server->send(400, "application/json", "{\"error\":\"No body\"}");
                return;
            }
            JsonDocument doc;
            if (deserializeJson(doc, _server->arg("plain")) != DeserializationError::Ok) {
                _server->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
                return;
            }
            if (doc["ssid"].is<const char*>()) {
                strlcpy(configManager.config.wifiSsid, doc["ssid"], sizeof(configManager.config.wifiSsid));
                // Trim trailing/leading spaces — forms often add them
                String s = String(configManager.config.wifiSsid); s.trim();
                strlcpy(configManager.config.wifiSsid, s.c_str(), sizeof(configManager.config.wifiSsid));
            }
            if (doc["pass"].is<const char*>()) {
                strlcpy(configManager.config.wifiPass, doc["pass"], sizeof(configManager.config.wifiPass));
                String p = String(configManager.config.wifiPass); p.trim();
                strlcpy(configManager.config.wifiPass, p.c_str(), sizeof(configManager.config.wifiPass));
            }
            // saveWifiOnly() writes dedicated NVS string keys that survive
            // future struct-size changes — password will never be lost again.
            configManager.saveWifiOnly();
            logger.info("[Web] WiFi credentials saved to NVS. Connecting...");

            // Give the HTTP response time to send before WiFi reconnects
            _server->send(200, "application/json", "{\"status\":\"saved\"}");
            delay(100);

            WiFi.disconnect(false);   // drop current connection, keep settings
            WiFi.begin(configManager.config.wifiSsid, configManager.config.wifiPass);
        });

        // ---- POST /api/calibrate/empty  (tank is empty right now) ----
        _server->on("/api/calibrate/empty", HTTP_POST, [this]() {
            float dist = hpSystem.water.sensorDistanceCm;
            if (!hpSystem.water.sensorOnline || dist <= 0) {
                _server->send(400, "application/json",
                    "{\"error\":\"Sensor offline or no valid reading.\"}");
                return;
            }
            configManager.config.emptyDistanceCm = dist;
            configManager.save();
            logger.logf(LogLevel::INFO,
                "[Web] Calibration: EMPTY set to %.1f cm", dist);
            String body = "{\"status\":\"saved\",\"empty_dist_cm\":" + String(dist, 1) + "}";
            _server->send(200, "application/json", body);
        });

        // ---- POST /api/calibrate/full  (tank is full right now) ----
        _server->on("/api/calibrate/full", HTTP_POST, [this]() {
            float dist = hpSystem.water.sensorDistanceCm;
            if (!hpSystem.water.sensorOnline || dist <= 0) {
                _server->send(400, "application/json",
                    "{\"error\":\"Sensor offline or no valid reading.\"}");
                return;
            }
            configManager.config.fullDistanceCm = dist;
            configManager.save();
            logger.logf(LogLevel::INFO,
                "[Web] Calibration: FULL set to %.1f cm", dist);
            String body = "{\"status\":\"saved\",\"full_dist_cm\":" + String(dist, 1) + "}";
            _server->send(200, "application/json", body);
        });

        // ---- 404 ----
        _server->onNotFound([this]() {
            _server->send(404, "application/json", "{\"error\":\"Not found\"}");
        });
    }

    String _buildLiveJson() const {
        JsonDocument doc;
        doc["state"]    = stateToStr(hpSystem.state);
        doc["uptime"]   = hpSystem.health.uptime;
        doc["device_id"]= hpSystem.device.deviceId;
        doc["firmware"] = hpSystem.device.firmwareVersion;
        doc["relay_hp"]       = (hpSystem.relay       == RelayState::ON) ? "ON" : "OFF";
        doc["relay_heater"]   = (hpSystem.heaterRelay  == RelayState::ON) ? "ON" : "OFF";
        doc["heater_manual"]  = hpSystem.heaterManualOn;
        doc["hp_manual"]      = hpSystem.hpManualOn;
        doc["setpoint"]            = configManager.config.tempSetpoint;
        doc["hysteresis"]           = configManager.config.tempHysteresis;
        doc["heater_setpoint"]      = configManager.config.heaterSetpoint;
        doc["control_sensor_idx"]   = configManager.config.controlSensorIdx;
        doc["temp_count"]           = hpSystem.tempCount;
        doc["timestamp"]= millis();

        JsonDocument alarmsDoc;
        deserializeJson(alarmsDoc, alarmManager.activeAlarmsJson());
        doc["alarms"]   = alarmsDoc.as<JsonArray>();

        JsonObject safety = doc["safety"].to<JsonObject>();
        safety["hp_fault"] = hpSystem.alarms.highPressure;
        safety["lp_fault"] = hpSystem.alarms.lowPressure;

        JsonArray pzemArr = doc["pzem"].to<JsonArray>();
        for (uint8_t i = 0; i < hpSystem.pzemCount; i++) {
            const PZEMData& p = hpSystem.pzem[i];
            JsonObject o = pzemArr.add<JsonObject>();
            o["index"]        = i;
            o["online"]       = p.online;
            o["voltage"]      = serialized(String(p.voltage,     4));
            o["current"]      = serialized(String(p.current,     4));
            o["power"]        = serialized(String(p.power,       4));
            o["energy"]       = serialized(String(p.energy,      4));
            o["frequency"]    = serialized(String(p.frequency,   4));
            o["power_factor"] = serialized(String(p.powerFactor, 4));
        }

        JsonArray tempArr = doc["temps"].to<JsonArray>();
        for (uint8_t i = 0; i < hpSystem.tempCount; i++) {
            const TempSensor& s = hpSystem.temps[i];
            JsonObject o = tempArr.add<JsonObject>();
            o["index"]  = i;
            o["name"]   = s.name;
            o["online"] = s.online;
            o["value"]  = serialized(String(s.value, 2));
        }

        JsonObject net    = doc["wifi"].to<JsonObject>();
        net["connected"]  = hpSystem.wifi.connected;
        net["rssi"]       = hpSystem.wifi.rssi;
        net["ip"]         = hpSystem.wifi.ip;

        // Water tank (AJ-SR04M)
        JsonObject wt        = doc["water"].to<JsonObject>();
        wt["sensor_online"]  = hpSystem.water.sensorOnline;
        wt["distance_cm"]    = hpSystem.water.sensorOnline ? (float)((int)(hpSystem.water.sensorDistanceCm * 10 + 0.5f)) / 10.0f : 0;
        wt["level_pct"]      = hpSystem.water.sensorOnline ? (float)((int)(hpSystem.water.waterLevelPercent * 10 + 0.5f)) / 10.0f : 0;
        wt["level_state"]    = hpSystem.water.waterLevelState;
        wt["shutoff_active"] = hpSystem.water.shutoffActive;
        wt["empty_dist_cm"]      = configManager.config.emptyDistanceCm;
        wt["full_dist_cm"]       = configManager.config.fullDistanceCm;
        wt["water_shutoff_pct"]  = configManager.config.waterShutoffPercent;
        wt["water_low_alarm_pct"]= configManager.config.waterLowAlarmPercent;

        JsonObject gsm    = doc["cellular"].to<JsonObject>();
        gsm["connected"]  = simStatus.gprsConnected;
        gsm["csq"]        = simStatus.csq;
        gsm["operator"]   = simStatus.operator_;
        gsm["ip"]         = simStatus.localIp;
        gsm["post_ok"]    = simStatus.postCount;
        gsm["post_fail"]  = simStatus.postFails;

        // ── Remote Settings Poller status ─────────────────────
        JsonObject sp           = doc["settings_poll"].to<JsonObject>();
        sp["poll_ok"]           = settingsPollStatus.lastPollOk;
        sp["poll_count"]        = settingsPollStatus.pollCount;
        sp["apply_count"]       = settingsPollStatus.applyCount;
        sp["fail_count"]        = settingsPollStatus.applyFailCount;
        sp["last_row_id"]       = settingsPollStatus.lastRowId;
        sp["last_feedback"]     = settingsPollStatus.lastFeedback;
        sp["last_apply_ms"]     = settingsPollStatus.lastApplyMs;

        String out;
        serializeJson(doc, out);
        return out;
    }
};

extern WebServerManager webServer;
