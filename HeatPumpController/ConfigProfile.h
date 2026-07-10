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
    float tempSetpoint          = 40.0f;    // °C
    uint32_t relayDelayMs       = 5000;     // Anti-short-cycle

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
// ============================================================
class ConfigManager {
public:
    ConfigProfile config;

    void begin() {
        _prefs.begin("hpconfig", false);
        _load();
        Serial.println("[Config] Profile loaded from NVS.");
    }

    void save() {
        _prefs.putBytes("profile", &config, sizeof(ConfigProfile));
        Serial.println("[Config] Profile saved to NVS.");
    }

    void resetDefaults() {
        config = ConfigProfile{};
        save();
        Serial.println("[Config] Defaults restored.");
    }

private:
    Preferences _prefs;

    void _load() {
        size_t len = _prefs.getBytesLength("profile");
        if (len == sizeof(ConfigProfile)) {
            _prefs.getBytes("profile", &config, sizeof(ConfigProfile));

            // Detect stale NVS data from old firmware versions.
            // If the URL is a placeholder or doesn't match expected domain, reset.
            String savedUrl = String(config.apiUrl);
            if (savedUrl.isEmpty() ||
                savedUrl.indexOf("your-server") >= 0 ||
                savedUrl.indexOf("example.com") >= 0 ||
                savedUrl.length() < 10) {
                Serial.println("[Config] Stale URL detected in NVS. Resetting to defaults.");
                config = ConfigProfile{};
                save();
            }
        } else {
            // First boot or struct size changed — use defaults & save
            Serial.println("[Config] No valid profile found. Using defaults.");
            config = ConfigProfile{};
            save();
        }
    }
};

// Global singleton — declared in main.cpp, extern everywhere else
extern ConfigManager configManager;
