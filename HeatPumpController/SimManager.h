#pragma once
#include <Arduino.h>
#include <Wire.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
#include "SystemData.h"
#include "ConfigProfile.h"
#include "Logger.h"
#include "Scheduler.h"
#include <WiFi.h>

// ============================================================
//  SIM MANAGER  —  SIM800L via TinyGSM on LILYGO T-Call V1.4
//
//  Hardware pins (T-Call V1.4 fixed wiring):
//    SIM800L TX → ESP32 RX = GPIO 26
//    SIM800L RX → ESP32 TX = GPIO 27
//    PWRKEY                = GPIO 4
//    RST                   = GPIO 5
//    IP5306 I2C SDA        = GPIO 21
//    IP5306 I2C SCL        = GPIO 22
//    IP5306 I2C Address    = 0x75
//
//  Library: TinyGSM by Volodymyr Shymanskyy
//           Install via Arduino Library Manager:
//           "TinyGSM"  (search and install v0.12.x)
//
//  APN: www  (Vodafone Idea / Vi India — no username/password required)
// ============================================================

// ---- TinyGSM must be configured BEFORE including it ----
// This is already set in HeatPumpController.ino.
// We only include the client header here.
#include <TinyGsmClient.h>

// ============================================================
//  HARDWARE CONSTANTS
// ============================================================
#define SIM_SERIAL_BAUD   115200
#define SIM_TX_PIN        27
#define SIM_RX_PIN        26
#define SIM_PWRKEY_PIN    4
#define SIM_RST_PIN       5
#define SIM_PWRON_PIN     23   // T-Call 1.4 power enable

#define IP5306_I2C_ADDR   0x75
#define IP5306_REG_SYS1   0x00
#define IP5306_REG_SYS2   0x01
#define IP5306_REG_CHG1   0x20
#define IP5306_REG_CHG2   0x21
#define IP5306_REG_DIS1   0x24
#define IP5306_REG_DIS2   0x26
#define IP5306_REG_BTN    0x30

#define GSM_APN           "www" // Vodafone Idea (Vi) 2G/GPRS APN
#define GSM_USER          ""          // No username required
#define GSM_PASS          ""          // No password required

#define HTTP_TIMEOUT_MS   10000
#define GPRS_RETRY_DELAY  30000UL   // 30 s between reconnect attempts

// ============================================================
//  CELLULAR STATUS (exposed in SystemData via hpSystem)
// ============================================================
struct CellularStatus {
    bool  modemOnline   = false;
    bool  gprsConnected = false;
    bool  simReady      = false;
    int8_t  csq         = 0;     // Signal quality 0-31 (99=unknown)
    char  iccid[21]     = "";
    char  operator_[20] = "";
    char  localIp[16]   = "0.0.0.0";
    unsigned long lastPostMs  = 0;
    uint16_t      postCount   = 0;
    uint16_t      postFails   = 0;
};

// Forward declare so WebServerManager can use it
extern CellularStatus simStatus;

class SimManager {
public:
    // -----------------------------------------------------------
    //  begin() — called once from setup()
    // -----------------------------------------------------------
    void begin() {
        logger.info("[SIM] Initialising SimManager (SIM800L + IP5306)...");

        // 1. Power up IP5306 to keep SIM800L alive
        _ip5306PowerOn();

        // 2. Enable modem power pin
        pinMode(SIM_PWRON_PIN, OUTPUT);
        digitalWrite(SIM_PWRON_PIN, HIGH);
        delay(100);

        // 3. Toggle PWRKEY to boot the modem
        _modemPowerOn();

        // 4. Begin hardware serial to SIM800L
        _simSerial.begin(SIM_SERIAL_BAUD, SERIAL_8N1, SIM_RX_PIN, SIM_TX_PIN);
        delay(300);

        // 5. Initialise TinyGSM modem object
        _modem = new TinyGsm(_simSerial);

        // 6. Connect to GPRS (only if network registered during boot)
        bool ok = _initModem();
        if (ok && _modem->isNetworkConnected()) {
            _connectGprs();
        }
    }

    // -----------------------------------------------------------
    //  update() — called by Scheduler every 30 s
    // -----------------------------------------------------------
    void update() {
        if (!simStatus.modemOnline) {
            // Try to reinitialise if modem died
            if (millis() - _lastRetry > GPRS_RETRY_DELAY) {
                logger.warning("[SIM] Modem offline. Retrying init...");
                _lastRetry = millis();
                bool ok = _initModem();
                if (ok) _connectGprs();
            }
            return;
        }

        // Check network registration first (non-blocking)
        if (!_modem->isNetworkConnected()) {
            logger.info("[SIM] Network not registered yet...");
            simStatus.gprsConnected = false;
            return;
        }

        // Check GPRS still connected
        if (!_modem->isGprsConnected()) {
            simStatus.gprsConnected = false;
            logger.warning("[SIM] GPRS disconnected. Reconnecting...");
            _connectGprs();
            return;
        }

        // Refresh signal quality
        simStatus.csq = _modem->getSignalQuality();

        logger.logf(LogLevel::INFO, "[SIM] GPRS OK | CSQ=%d | PostOK=%u Fail=%u",
                    simStatus.csq, simStatus.postCount, simStatus.postFails);
    }

    // -----------------------------------------------------------
    //  postTelemetry() — kept for backward compat (Scheduler still calls it
    //  but CloudManager is the preferred path now)
    // -----------------------------------------------------------
    void postTelemetry() {
        // CloudManager handles routing. This is now a no-op to avoid
        // duplicate posts. CloudManager's taskCloud handles everything.
    }

    // -----------------------------------------------------------
    //  postTelemetryPayload() — called by CloudManager for GPRS fallback
    //  Returns true on HTTP 2xx response.
    // -----------------------------------------------------------
    bool postTelemetryPayload(const String& payload, const char* url) {
        if (!simStatus.gprsConnected) return false;

        logger.logf(LogLevel::INFO, "[SIM] GPRS POST %u bytes → %s", payload.length(), url);

        TinyGsmClient client(*_modem);
        bool ok = _httpPost(client, url, payload);
        if (ok) {
            simStatus.postCount++;
            simStatus.lastPostMs = millis();
        } else {
            simStatus.postFails++;
        }
        return ok;
    }

    // Public getter for modem object (used by TinyGsmClientSecure if needed)
    TinyGsm* modem() { return _modem; }

    // -----------------------------------------------------------
    //  SMS Engine — checks for remote configuration commands
    // -----------------------------------------------------------
    void checkSMS() {
        if (!simStatus.modemOnline || !_modem) return;
        if (_isCheckingSMS) return;

        // Guard: if modem lost Vi registration, skip SMS read.
        // update() (runs every 30s) handles reconnection — don't trigger
        // _connectGprs() here or it will hammer the modem every 10s.
        if (!_modem->isNetworkConnected()) {
            logger.warning("[SIM] checkSMS: not registered on network — skipping SMS read.");
            return;
        }

        _isCheckingSMS = true;

        // Set text mode
        _modem->sendAT(GF("+CMGF=1"));
        _modem->waitResponse(1000);

        // List unread messages
        _modem->sendAT(GF("+CMGL=\"REC UNREAD\""));

        while (_modem->waitResponse(2000, GF("+CMGL:"), GF("OK"), GF("ERROR")) == 1) {
            // Read header line: index,"REC UNREAD","+91XXXXXXXXXX",,"timestamp"
            String header = _modem->stream.readStringUntil('\n');
            // Read body (actual SMS text)
            String body   = _modem->stream.readStringUntil('\n');

            // Aggressively strip all whitespace and control chars
            _cleanString(header);
            _cleanString(body);

            logger.logf(LogLevel::INFO, "[SIM] SMS Header: [%s]", header.c_str());
            logger.logf(LogLevel::INFO, "[SIM] SMS Body:   [%s]", body.c_str());

            // ── Extract message index (first token before first comma) ──
            int firstCommaH = header.indexOf(',');
            int index = (firstCommaH > 0)
                        ? header.substring(0, firstCommaH).toInt()
                        : header.toInt();

            // ── Extract phone number ──
            // Header format: idx,"REC UNREAD","phone",,"date"
            // We need the 3rd comma-delimited field, strip surrounding quotes
            String phoneStr = _extractField(header, 2);  // 0-indexed field 2
            phoneStr.replace("\"", "");
            _cleanString(phoneStr);
            logger.logf(LogLevel::INFO, "[SIM] Sender: [%s]", phoneStr.c_str());

            // ── Process command ──
            if (body.length() > 0) {
                _executeCommand(body, phoneStr);
            } else {
                logger.warning("[SIM] Empty SMS body — skipping.");
            }

            // ── Delete this message ──
            if (index > 0) {
                _modem->sendAT(GF("+CMGD="), index);
                _modem->waitResponse(1000);
                logger.logf(LogLevel::INFO, "[SIM] Deleted SMS index %d", index);
            }
        }
        _isCheckingSMS = false;
    }

private:
    HardwareSerial _simSerial{1};   // UART1
    TinyGsm*       _modem = nullptr;
    unsigned long  _lastRetry = 0;
    bool           _isCheckingSMS = false;

    // -----------------------------------------------------------
    //  _executeCommand — scalable command parser
    //  Format: COMMAND:PIN,args...
    // -----------------------------------------------------------
    void _executeCommand(const String& rawCmd, const String& phone) {
        // Clean the raw command string completely
        String cmdLine = rawCmd;
        _cleanString(cmdLine);
        logger.logf(LogLevel::INFO, "[SIM] Parsing command: [%s] from [%s]",
                    cmdLine.c_str(), phone.c_str());

        // ── Split on ':' — format is COMMAND:PIN or COMMAND:PIN,args ──
        int colonIdx = cmdLine.indexOf(':');
        if (colonIdx < 0) {
            logger.warning("[SIM] No colon found in command. Format: COMMAND:PIN or COMMAND:PIN,args");
            if (phone.length() > 5) {
                _modem->sendSMS(phone, "Invalid format. Use: COMMAND:PIN or COMMAND:PIN,args");
            }
            return;
        }

        String cmd = cmdLine.substring(0, colonIdx);
        cmd.toUpperCase();
        _cleanString(cmd);

        String rest = cmdLine.substring(colonIdx + 1);
        _cleanString(rest);

        // ── Split rest on first comma → PIN + args ──
        int firstComma = rest.indexOf(',');
        String pin  = (firstComma > 0) ? rest.substring(0, firstComma) : rest;
        String args = (firstComma > 0) ? rest.substring(firstComma + 1) : "";
        _cleanString(pin);
        _cleanString(args);

        logger.logf(LogLevel::INFO, "[SIM] CMD=[%s] PIN=[%s] ARGS=[%s]",
                    cmd.c_str(), pin.c_str(), args.c_str());

        // ── Verify PIN ──
        String storedPin = String(configManager.config.devicePin);
        _cleanString(storedPin);
        if (pin != storedPin) {
            logger.logf(LogLevel::WARNING,
                "[SIM] PIN mismatch. Got=[%s] Expected=[%s]",
                pin.c_str(), storedPin.c_str());
            if (phone.length() > 5) {
                _modem->sendSMS(phone, "Access denied: wrong PIN.");
            }
            return;
        }

        // ── SET_WIFI:PIN,SSID,PASSWORD ──
        if (cmd == "SET_WIFI") {
            int sepComma = args.indexOf(',');
            if (sepComma < 0) {
                logger.warning("[SIM] SET_WIFI missing SSID,PASSWORD args.");
                if (phone.length() > 5)
                    _modem->sendSMS(phone, "Format: SET_WIFI:PIN,SSID,PASSWORD");
                return;
            }
            String ssid = args.substring(0, sepComma);
            String pass = args.substring(sepComma + 1);
            _cleanString(ssid);
            _cleanString(pass);

            logger.logf(LogLevel::INFO, "[SIM] SET_WIFI SSID=[%s] PASS=[%s]",
                        ssid.c_str(), pass.c_str());

            // ── Save credentials to NVS IMMEDIATELY ──────────────────────────
            // Do this BEFORE attempting connection so credentials are always
            // persisted even if the WiFi connection times out or fails.
            // On next reboot the NetworkManager will pick them up automatically.
            strlcpy(configManager.config.wifiSsid, ssid.c_str(),
                    sizeof(configManager.config.wifiSsid));
            strlcpy(configManager.config.wifiPass, pass.c_str(),
                    sizeof(configManager.config.wifiPass));
            configManager.saveWifiOnly();   // writes "wifi_ssid" + "wifi_pass" NVS keys
            logger.info("[SIM] WiFi credentials saved to NVS.");

            // ── Now attempt connection ────────────────────────────────────────
            WiFi.begin(ssid.c_str(), pass.c_str());

            unsigned long start = millis();
            bool connected = false;
            extern Scheduler scheduler;
            while (millis() - start < 25000) {
                if (WiFi.status() == WL_CONNECTED) { connected = true; break; }
                scheduler.tick();
                delay(300);
            }

            if (connected) {
                logger.info("[SIM] WiFi connected successfully.");
                if (phone.length() > 5) {
                    String reply = "WiFi OK! SSID: " + ssid
                                 + " IP: " + WiFi.localIP().toString()
                                 + " RSSI: " + String(WiFi.RSSI()) + "dBm";
                    _modem->sendSMS(phone, reply);
                }
            } else {
                // Credentials are ALREADY saved — device will retry on next boot.
                logger.warning("[SIM] WiFi connection timed out. Credentials saved for next boot.");
                if (phone.length() > 5)
                    _modem->sendSMS(phone, "WiFi SAVED but not connected yet (timeout). "
                                          "Device will retry on next reboot. SSID: " + ssid);
            }
        }

        // ── STATUS:PIN ──
        else if (cmd == "STATUS") {
            String reply = "State: " + String(stateToStr(hpSystem.state))
                         + "\nRelay: " + (hpSystem.relay == RelayState::ON ? "ON" : "OFF")
                         + "\nWiFi: " + (hpSystem.wifi.connected ? hpSystem.wifi.ip : "NO")
                         + "\nSIM CSQ: " + String(simStatus.csq)
                         + "\nWater: " + String(hpSystem.water.waterLevelPercent, 0) + "%"
                         + " (" + hpSystem.water.waterLevelState + ")";
            logger.info("[SIM] Sending STATUS reply.");
            if (phone.length() > 5) _modem->sendSMS(phone, reply);
        }

        // ── RESTART:PIN ──
        else if (cmd == "RESTART") {
            logger.info("[SIM] RESTART command. Rebooting...");
            if (phone.length() > 5) _modem->sendSMS(phone, "Rebooting now...");
            delay(1500);
            ESP.restart();
        }

        else {
            logger.logf(LogLevel::WARNING, "[SIM] Unknown command: [%s]", cmd.c_str());
            if (phone.length() > 5)
                _modem->sendSMS(phone, "Unknown command: " + cmd
                              + ". Valid: SET_WIFI, STATUS, RESTART");
        }
    }

    // ── Helper: remove all CR, LF, non-printable chars, and trim ──
    void _cleanString(String& s) {
        String out;
        out.reserve(s.length());
        for (size_t i = 0; i < s.length(); i++) {
            char c = s[i];
            if (c >= 32 && c < 127) out += c;   // keep only printable ASCII
        }
        out.trim();
        s = out;
    }

    // ── Helper: extract Nth comma-delimited field (0-indexed) ──
    String _extractField(const String& str, uint8_t fieldIndex) {
        uint8_t currentField = 0;
        int start = 0;
        for (int i = 0; i <= (int)str.length(); i++) {
            if (i == (int)str.length() || str[i] == ',') {
                if (currentField == fieldIndex)
                    return str.substring(start, i);
                currentField++;
                start = i + 1;
            }
        }
        return "";
    }

    // -----------------------------------------------------------
    //  IP5306 — turn on "Boost Output" so SIM800L gets power

    // -----------------------------------------------------------
    void _ip5306PowerOn() {
        Wire.begin(21, 22);   // SDA=21, SCL=22

        // Enable boost output always-on mode
        _ip5306SetBits(IP5306_REG_SYS1, 0x80, 0x80);
        // Boost output on
        _ip5306SetBits(IP5306_REG_DIS1, 0x20, 0x20);

        logger.info("[SIM] IP5306 boost output enabled.");
    }

    void _ip5306SetBits(uint8_t reg, uint8_t mask, uint8_t val) {
        Wire.beginTransmission(IP5306_I2C_ADDR);
        Wire.write(reg);
        Wire.endTransmission();
        Wire.requestFrom((uint8_t)IP5306_I2C_ADDR, (uint8_t)1);
        uint8_t cur = Wire.available() ? Wire.read() : 0;
        cur = (cur & ~mask) | (val & mask);
        Wire.beginTransmission(IP5306_I2C_ADDR);
        Wire.write(reg);
        Wire.write(cur);
        Wire.endTransmission();
    }

    // -----------------------------------------------------------
    //  Modem power on via PWRKEY
    // -----------------------------------------------------------
    void _modemPowerOn() {
        pinMode(SIM_PWRKEY_PIN, OUTPUT);
        pinMode(SIM_RST_PIN,    OUTPUT);
        
        // ⚠️ CRITICAL: RST must be HIGH for the modem to run. 
        // If left LOW, the modem is held in permanent hardware reset!
        digitalWrite(SIM_RST_PIN, HIGH);
        delay(100);

        // Toggle PWRKEY to boot SIM800L (pull LOW for >1s, then HIGH)
        digitalWrite(SIM_PWRKEY_PIN, LOW);
        delay(1500);
        digitalWrite(SIM_PWRKEY_PIN, HIGH);
        delay(2500);  // Wait for modem to stabilise

        logger.info("[SIM] PWRKEY toggled. Modem booting...");
    }

    // -----------------------------------------------------------
    //  Initialise the TinyGSM modem
    // -----------------------------------------------------------
    bool _initModem() {
        logger.info("[SIM] Initialising modem (AT)...");
        // We use init() instead of restart() because we already hard-reset it with PWRKEY!
        // restart() takes 10+ seconds and triggers the ESP32 v3 Core WDT crash.
        if (!_modem->init()) {
            logger.error("[SIM] Modem init failed!");
            simStatus.modemOnline = false;
            return false;
        }

        simStatus.modemOnline = true;
        logger.info("[SIM] Modem online.");

        // Read ICCID
        String iccid = _modem->getSimCCID();
        iccid.toCharArray(simStatus.iccid, sizeof(simStatus.iccid));
        logger.logf(LogLevel::INFO, "[SIM] ICCID: %s", simStatus.iccid);

        // Fast check for network registration
        logger.info("[SIM] Checking network registration...");
        bool registered = _modem->waitForNetwork(3000); // 3 seconds max during boot
        
        if (registered) {
            simStatus.simReady = true;
            simStatus.csq = _modem->getSignalQuality();
            String op = _modem->getOperator();
            op.toCharArray(simStatus.operator_, sizeof(simStatus.operator_));
            logger.logf(LogLevel::INFO, "[SIM] Network: %s | CSQ: %d", simStatus.operator_, simStatus.csq);
        } else {
            logger.warning("[SIM] Network not registered yet. Will retry in background.");
        }

        return true; // Return true because modem is online, even if not registered yet
    }

    // -----------------------------------------------------------
    //  Connect GPRS
    // -----------------------------------------------------------
    void _connectGprs() {
        logger.info("[SIM] Connecting GPRS (APN: " GSM_APN ")...");
        if (!_modem->gprsConnect(GSM_APN, GSM_USER, GSM_PASS)) {
            logger.error("[SIM] GPRS connection failed!");
            simStatus.gprsConnected = false;
            return;
        }

        simStatus.gprsConnected = true;
        String ip = _modem->localIP().toString();
        ip.toCharArray(simStatus.localIp, sizeof(simStatus.localIp));
        logger.logf(LogLevel::INFO, "[SIM] GPRS connected. IP: %s", simStatus.localIp);
    }

    // -----------------------------------------------------------
    //  HTTP POST over TinyGsmClient
    // -----------------------------------------------------------
    bool _httpPost(TinyGsmClient& client, const char* fullUrl, const String& body) {
        // Parse host and path from URL
        // Format: http://host:port/path  or  http://host/path
        String url = String(fullUrl);
        if (!url.startsWith("http://")) {
            logger.error("[SIM] Only http:// URLs are supported for SIM800L.");
            return false;
        }

        String hostAndPath = url.substring(7);  // strip "http://"
        int portStart = hostAndPath.indexOf(':');
        int pathStart = hostAndPath.indexOf('/');
        if (pathStart < 0) { hostAndPath += "/"; pathStart = hostAndPath.length() - 1; }

        String host;
        uint16_t port = 80;
        String path;

        if (portStart >= 0 && portStart < pathStart) {
            host = hostAndPath.substring(0, portStart);
            port = hostAndPath.substring(portStart + 1, pathStart).toInt();
            path = hostAndPath.substring(pathStart);
        } else {
            host = hostAndPath.substring(0, pathStart);
            path = hostAndPath.substring(pathStart);
        }

        logger.logf(LogLevel::INFO, "[SIM] Connecting to %s:%d%s", host.c_str(), port, path.c_str());

        if (!client.connect(host.c_str(), port)) {
            logger.error("[SIM] TCP connect failed.");
            return false;
        }

        // Send HTTP request
        client.print("POST " + path + " HTTP/1.1\r\n");
        client.print("Host: " + host + "\r\n");
        client.print("Content-Type: application/json\r\n");
        client.print("Content-Length: " + String(body.length()) + "\r\n");

        // Send API key header if configured
        if (strlen(configManager.config.apiKey) > 0) {
            client.print("X-API-Key: " + String(configManager.config.apiKey) + "\r\n");
        }
        client.print("Connection: close\r\n\r\n");
        client.print(body);

        // Wait for response
        unsigned long start = millis();
        while (client.connected() && millis() - start < HTTP_TIMEOUT_MS) {
            if (client.available()) {
                String responseLine = client.readStringUntil('\n');
                logger.logf(LogLevel::INFO, "[SIM] HTTP resp: %s", responseLine.substring(0, 60).c_str());
                // We only need the first line (status)
                client.stop();
                return responseLine.startsWith("HTTP/1.1 2") || responseLine.startsWith("HTTP/1.0 2");
            }
            delay(10);
        }

        client.stop();
        logger.error("[SIM] HTTP response timeout.");
        return false;
    }

    // -----------------------------------------------------------
    //  Build the telemetry JSON payload
    // -----------------------------------------------------------
    String _buildPayload() {
        JsonDocument doc;

        doc["schemaVersion"] = "1.0";
        doc["deviceId"]      = hpSystem.device.deviceId;
        doc["timestamp"]     = millis() / 1000;  // seconds uptime (replace with NTP if available)

        // System
        JsonObject sys = doc["system"].to<JsonObject>();
        sys["state"]           = stateToStr(hpSystem.state);
        sys["relayOn"]         = (hpSystem.relay == RelayState::ON);
        sys["firmwareVersion"] = hpSystem.device.firmwareVersion;
        sys["uptimeSeconds"]   = hpSystem.health.uptime;

        // Safety
        JsonObject safety = doc["safety"].to<JsonObject>();
        safety["highPressureOk"]  = !hpSystem.alarms.highPressure;
        safety["lowPressureOk"]   = !hpSystem.alarms.lowPressure;

        // Alarms
        JsonObject alarms = doc["alarms"].to<JsonObject>();
        alarms["highPressureTrip"] = hpSystem.alarms.highPressure;
        alarms["lowPressureTrip"]  = hpSystem.alarms.lowPressure;
        alarms["overvoltage"]      = hpSystem.alarms.overvoltage;
        alarms["undervoltage"]     = hpSystem.alarms.undervoltage;
        alarms["overcurrent"]      = hpSystem.alarms.overcurrent;
        alarms["highTemperature"]  = hpSystem.alarms.highTemp;
        alarms["lowTemperature"]   = hpSystem.alarms.lowTemp;

        // PZEM power data
        JsonArray pzemArr = doc["power"].to<JsonArray>();
        for (uint8_t i = 0; i < hpSystem.pzemCount; i++) {
            const PZEMData& p = hpSystem.pzem[i];
            JsonObject phase = pzemArr.add<JsonObject>();
            phase["phase"]       = i + 1;
            phase["online"]      = p.online;
            phase["voltage"]     = serialized(String(p.voltage,     2));
            phase["current"]     = serialized(String(p.current,     3));
            phase["power"]       = serialized(String(p.power,       2));
            phase["energy"]      = serialized(String(p.energy,      3));
            phase["frequency"]   = serialized(String(p.frequency,   1));
            phase["powerFactor"] = serialized(String(p.powerFactor, 3));
        }

        // Temperature sensors
        JsonArray tempArr = doc["temperatures"].to<JsonArray>();
        for (uint8_t i = 0; i < hpSystem.tempCount; i++) {
            const TempSensor& s = hpSystem.temps[i];
            JsonObject t = tempArr.add<JsonObject>();
            t["name"]   = s.name;
            t["online"] = s.online;
            t["value"]  = serialized(String(s.value, 2));
        }

        // Network / connectivity
        JsonObject net = doc["network"].to<JsonObject>();
        net["wifiConnected"]   = hpSystem.wifi.connected;
        net["wifiRssi"]        = hpSystem.wifi.rssi;
        net["wifiIp"]          = hpSystem.wifi.ip;
        net["gprsConnected"]   = simStatus.gprsConnected;
        net["simCsq"]          = simStatus.csq;
        net["simOperator"]     = simStatus.operator_;
        net["simIp"]           = simStatus.localIp;

        String out;
        serializeJson(doc, out);
        return out;
    }
};

// Global singleton — defined in HeatPumpController.ino
CellularStatus simStatus;
extern SimManager simManager;
