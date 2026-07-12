#pragma once
#include <Arduino.h>
#include <Preferences.h>

// ============================================================
//  CONFIGURATION & INSTALLATION PROFILE
//
//  Stored in ESP32 NVS (non-volatile flash).
//  Field technician sets this on installation via dashboard.
//  No reflashing needed.
// ============================================================
struct ConfigProfile {
    // --- Installation identity ---
    char customerName[48]       = "Unknown Customer";
    char installAddress[96]     = "Unknown Address";
    char installerName[32]      = "Unknown";
    char installDate[12]        = "2025-01-01";
    char deviceAlias[32]        = "HeatPump-01";
    char timezone[32]           = "Asia/Kolkata";
    char country[32]            = "India";

    // --- Device identity ---
    char serialNumber[32]       = "SN000001";

    // --- Network ---
    char apiUrl[96]             = "http://iotsystems.co.in/iot_api/telemetry.php";
    char apiKey[64]             = "";
    char wifiSsid[32]           = "";
    char wifiPass[64]           = "";
    char devicePin[8]           = "864219"; // 6-digit SMS config PIN

    // --- Sampling rates (milliseconds) ---
    uint32_t pzemIntervalMs     = 1000;
    uint32_t tempIntervalMs     = 2000;
    uint32_t cloudIntervalMs    = 5000;
    uint32_t rssiIntervalMs     = 10000;
    uint32_t ntpIntervalMs      = 3600000;  // 1 hour
    uint32_t dashboardIntervalMs= 1000;

    // --- PZEM Protection thresholds ---
    float voltageMax            = 253.0f;   // V  (230V +10%)
    float voltageMin            = 196.0f;   // V  (230V -15%)
    float currentMax            = 20.0f;    // A
    float powerMax              = 5000.0f;  // W

    // --- Temperature thresholds ---
    float tempHighLimit         = 70.0f;    // °C
    float tempLowLimit          = 0.0f;     // °C

    // --- Control ---
    float    tempSetpoint       = 40.0f;    // °C  Heat Pump stop temperature
    float    tempHysteresis     = 2.0f;     // °C  HP starts at (setpoint - hyst), stops at setpoint
    float    heaterSetpoint     = 40.0f;    // °C  Heater stop temperature
    float    heaterHysteresis   = 2.0f;     // °C  Heater starts at (setpoint - hyst), stops at setpoint
    uint8_t  controlSensorIdx   = 0;        // Which DS18B20 index drives HP+Heater control
    uint32_t relayDelayMs       = 5000;     // Anti-short-cycle delay (ms)

    // --- Water Tank (AJ-SR04M) — Two-point calibration ---
    // Step 1: Empty tank fully → note the distance reading → set emptyDistanceCm
    // Step 2: Fill tank fully → note the distance reading → set fullDistanceCm
    // No geometry or sensor offset needed.
    float    emptyDistanceCm      = 35.0f;   // sensor reading when tank is EMPTY (cm)
    float    fullDistanceCm       = 5.0f;    // sensor reading when tank is FULL  (cm)
    float    waterShutoffPercent  = 15.0f;   // shut off heat pump below this %
    float    waterLowAlarmPercent = 25.0f;   // trigger low-water alarm below this %
};

// ============================================================
//  CONFIG MANAGER
//  Saves / loads ConfigProfile from NVS.
//
//  WiFi Safety Rule:
//  wifiSsid + wifiPass are ALSO stored as dedicated NVS string
//  keys ("wifi_ssid", "wifi_pass") independently of the profile
//  blob. This means firmware updates that add new struct fields
//  (causing a blob size mismatch and a reset-to-defaults) will
//  NEVER erase the saved WiFi password. Credentials always survive.
// ============================================================
class ConfigManager {
public:
    ConfigProfile config;

    void begin() {
        _prefs.begin("hpconfig", false);
        _load();
        Serial.println("[Config] Profile loaded from NVS.");
    }

    // Full save — writes the profile blob AND refreshes the WiFi keys.
    void save() {
        _prefs.putBytes("profile", &config, sizeof(ConfigProfile));
        // Always mirror WiFi to dedicated keys so they survive future
        // struct size changes caused by adding new ConfigProfile fields.
        _prefs.putString("wifi_ssid", config.wifiSsid);
        _prefs.putString("wifi_pass", config.wifiPass);
        Serial.println("[Config] Profile saved to NVS.");
    }

    // Fast WiFi-only save — skips the full blob write.
    // Call this from /api/wifi and SET_WIFI SMS handler for speed.
    void saveWifiOnly() {
        _prefs.putString("wifi_ssid", config.wifiSsid);
        _prefs.putString("wifi_pass", config.wifiPass);
        Serial.printf("[Config] WiFi credentials saved to NVS: SSID='%s'\n",
                      config.wifiSsid);
    }

    void resetDefaults() {
        config = ConfigProfile{};
        save();
        Serial.println("[Config] Defaults restored.");
    }

private:
    Preferences _prefs;

    void _load() {
        bool needResave = false;

        size_t len = _prefs.getBytesLength("profile");
        if (len == sizeof(ConfigProfile)) {
            _prefs.getBytes("profile", &config, sizeof(ConfigProfile));

            // Detect stale NVS data from old firmware versions.
            String savedUrl = String(config.apiUrl);
            if (savedUrl.isEmpty() ||
                savedUrl.indexOf("your-server") >= 0 ||
                savedUrl.indexOf("example.com") >= 0 ||
                savedUrl.length() < 10) {
                Serial.println("[Config] Stale URL in NVS — resetting to defaults.");
                config = ConfigProfile{};
                needResave = true;
            }
        } else {
            // First boot OR struct size changed after a firmware update.
            // Reset to defaults — WiFi recovery below will restore credentials.
            Serial.println("[Config] Profile size mismatch (new firmware?) — resetting. WiFi will be restored.");
            config = ConfigProfile{};
            needResave = true;
        }

        // ── WiFi credential recovery (always runs last) ───────────────────────
        // These dedicated keys are written by every save() / saveWifiOnly() call
        // and are NEVER erased by struct-size resets. So even after a firmware
        // update wipes the blob, the password is safe here.
        String savedSsid = _prefs.getString("wifi_ssid", "");
        String savedPass = _prefs.getString("wifi_pass", "");
        if (savedSsid.length() > 0) {
            strlcpy(config.wifiSsid, savedSsid.c_str(), sizeof(config.wifiSsid));
            strlcpy(config.wifiPass, savedPass.c_str(), sizeof(config.wifiPass));
            Serial.printf("[Config] WiFi restored: SSID='%s'\n", config.wifiSsid);
        } else {
            Serial.println("[Config] No saved WiFi. Use http://192.168.4.1 or /api/wifi to configure.");
        }

        // If we reset the blob, re-save now that WiFi is populated correctly.
        if (needResave) save();
    }
};

// Global singleton — declared in main.cpp, extern everywhere else
extern ConfigManager configManager;
