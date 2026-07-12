#pragma once
#include <Arduino.h>

// ============================================================
//  FIRMWARE / HARDWARE VERSIONING
// ============================================================
#define FW_VERSION      "1.0.0"
#define HW_VERSION      "1.0"
#define PROTO_VERSION   "1.0"

// ============================================================
//  DEVICE STATE MACHINE
// ============================================================
enum class DeviceState : uint8_t {
    BOOTING,
    SELF_TEST,
    CONNECTING_WIFI,
    CONNECTING_CELLULAR,
    SYNC_TIME,
    READY,
    MONITORING,
    AUTO_MODE,
    MANUAL_MODE,
    FAULT,
    LOCKOUT,
    OTA_UPDATE,
    RESTARTING
};

inline const char* stateToStr(DeviceState s) {
    switch (s) {
        case DeviceState::BOOTING:              return "BOOTING";
        case DeviceState::SELF_TEST:            return "SELF_TEST";
        case DeviceState::CONNECTING_WIFI:      return "CONNECTING_WIFI";
        case DeviceState::CONNECTING_CELLULAR:  return "CONNECTING_CELLULAR";
        case DeviceState::SYNC_TIME:            return "SYNC_TIME";
        case DeviceState::READY:                return "READY";
        case DeviceState::MONITORING:           return "MONITORING";
        case DeviceState::AUTO_MODE:            return "AUTO_MODE";
        case DeviceState::MANUAL_MODE:          return "MANUAL_MODE";
        case DeviceState::FAULT:                return "FAULT";
        case DeviceState::LOCKOUT:              return "LOCKOUT";
        case DeviceState::OTA_UPDATE:           return "OTA_UPDATE";
        case DeviceState::RESTARTING:           return "RESTARTING";
        default:                                return "UNKNOWN";
    }
}

// ============================================================
//  ALARM FLAGS
// ============================================================
struct AlarmData {
    bool pzemOffline        = false;
    bool tempSensorLost     = false;
    bool overvoltage        = false;
    bool undervoltage       = false;
    bool overcurrent        = false;
    bool highTemp           = false;
    bool lowTemp            = false;
    bool highPressure       = false;
    bool lowPressure        = false;
    bool wifiLost           = false;
    bool cloudUnreachable   = false;
    bool lowWaterLevel      = false;   // water < waterLowAlarmPercent
    bool criticalWaterLevel = false;   // water < waterShutoffPercent → HP off

    bool anyActive() const {
        return pzemOffline || tempSensorLost || overvoltage ||
               undervoltage || overcurrent || highTemp || lowTemp ||
               highPressure || lowPressure || wifiLost || cloudUnreachable ||
               lowWaterLevel || criticalWaterLevel;
    }
};

// ============================================================
//  PZEM-004T DATA MODEL  (array-ready: PZEMData pzem[3])
// ============================================================
struct PZEMData {
    float voltage       = 0.0f;
    float current       = 0.0f;
    float power         = 0.0f;
    float energy        = 0.0f;
    float frequency     = 0.0f;
    float powerFactor   = 0.0f;
    bool  online        = false;
    unsigned long lastUpdate = 0;
};

// ============================================================
//  AJ-SR04M WATER TANK DATA
// ============================================================
struct WaterTankData {
    float    sensorDistanceCm  = 0.0f;       // raw distance from sensor to water surface
    float    waterHeightCm     = 0.0f;       // calculated water height from tank bottom
    float    waterLevelPercent = 0.0f;       // 0.0 – 100.0
    char     waterLevelState[16] = "UNKNOWN"; // FULL/HIGH/NORMAL/LOW/CRITICAL/EMPTY/SENSOR_ERROR
    bool     sensorOnline      = false;
    bool     shutoffActive     = false;      // true when HP is being held off by water level
    unsigned long lastUpdate   = 0;
};

// ============================================================
//  DS18B20 TEMPERATURE SENSOR MODEL (array-ready)
// ============================================================
struct TempSensor {
    uint8_t  address[8] = {0};
    char     name[16]   = "Sensor";
    float    value      = 0.0f;
    bool     online     = false;
    unsigned long lastUpdate = 0;
};

// ============================================================
//  RELAY STATE
// ============================================================
enum class RelayState : uint8_t { OFF = 0, ON = 1, FAULT = 2 };

// ============================================================
//  NETWORK STATUS
// ============================================================
struct WiFiStatus {
    bool  connected  = false;
    bool  internetOk = false;
    bool  ntpSynced  = false;
    int8_t rssi      = 0;
    char   ip[16]    = "0.0.0.0";
    char   ssid[32]  = "";
};

// ============================================================
//  CLOUD STATUS
// ============================================================
struct CloudSyncStatus {
    bool  connected    = false;
    unsigned long lastSyncMs = 0;
    uint16_t queueDepth = 0;
};

// ============================================================
//  DEVICE IDENTITY
// ============================================================
struct DeviceInfo {
    char deviceId[32]        = "";
    char serialNumber[32]    = "";
    char firmwareVersion[12] = FW_VERSION;
    char hardwareVersion[12] = HW_VERSION;
    char protocolVersion[12] = PROTO_VERSION;
    char macAddress[18]      = "";
};

// ============================================================
//  HEALTH DATA  (diagnostics — for engineer, not customer)
// ============================================================
struct HealthData {
    uint32_t freeHeap      = 0;
    uint32_t minFreeHeap   = 0;
    uint32_t uptime        = 0;   // seconds
    uint16_t restartCount  = 0;
    float    cpuFreqMHz    = 0.0f;
};

// ============================================================
//  CENTRAL SYSTEM DATA — SINGLE SOURCE OF TRUTH
//
//  NOTE: Named 'hpSystem' (not 'system') because 'system' is
//  a reserved C standard library function (stdlib.h).
// ============================================================
struct SystemData {
    DeviceState state = DeviceState::BOOTING;

    DeviceInfo  device;
    HealthData  health;
    AlarmData   alarms;
    WiFiStatus  wifi;
    CloudSyncStatus cloud;
    RelayState  relay        = RelayState::OFF;   // Heat Pump compressor relay (GPIO 18)
    RelayState  heaterRelay  = RelayState::OFF;   // Heater relay (GPIO 19)
    bool        heaterManualOn = false;            // true = user manually enabled heater
    bool        hpManualOn     = false;            // true = user manually enabled heat pump

    PZEMData      pzem[3];
    TempSensor    temps[8];
    WaterTankData water;
    uint8_t       pzemCount = 3; // 3-Phase (R, Y, B)
    uint8_t       tempCount = 0;
};

// Global singleton — defined once in HeatPumpController.ino
extern SystemData hpSystem;
