// ============================================================
//  ⚠️  MUST be defined BEFORE any TinyGSM or network include!
//  Tells TinyGSM which modem chip the T-Call V1.4 has.
// ============================================================
#define TINY_GSM_MODEM_SIM800

// ============================================================
//  HEAT PUMP MONITORING & CONTROL SYSTEM
//  Phase 1 — Local Commissioning
//
//  IDE:     Arduino IDE 2.x
//  Board:   ESP32 Dev Module
//  FW Ver:  1.0.0
//
//  *** REQUIRED LIBRARIES (install via Library Manager) ***
//  - PZEM004Tv30        by Olehs  ← works with v3.0 AND v4.0
//  NOTE: WebServer is BUILT INTO the ESP32 Arduino Core — no install needed!
//  - OneWire            by Paul Stoffregen
//  - DallasTemperature  by Miles Burton
//  - WiFiManager        by tzapu
//  - ArduinoJson        by Benoit Blanchon  (v7.x)
//  - NTPClient          by Fabrice Weinberg
//
//  *** BOARD SETTINGS ***
//  Tools > Board       : ESP32 Dev Module
//  Tools > Partition   : Minimal SPIFFS (1.9MB APP, 190KB SPIFFS)
//  Tools > Upload Speed: 921600
//  Tools > CPU Freq    : 240MHz
// ============================================================

// ---- Core data & infrastructure ----
#include "SystemData.h"
#include "ConfigProfile.h"
#include "Logger.h"
#include "Scheduler.h"
#include "MessageQueue.h"

// ---- Subsystem managers ----
#include "NetworkManager.h"
#include "PZEMManager.h"
#include "TemperatureManager.h"
#include "SensorManager.h"
#include "SimManager.h"
#include "CloudManager.h"
#include "AlarmManager.h"
#include "ControlEngine.h"
#include "WebServerManager.h"
#include "WaterManager.h"

#include <esp_task_wdt.h>

// ============================================================
//  GLOBAL SINGLETON DEFINITIONS
//  Declared extern in each header; defined here exactly once.
// ============================================================
SystemData         hpSystem;     // renamed from 'system'
ConfigManager      configManager;
LogManager         logger;
Scheduler          scheduler;
MessageQueue       msgQueue;
HPNetworkManager   networkManager; // renamed from 'NetworkManager'
PZEMManager        pzemManager;
TemperatureManager tempManager;
SensorManager      sensorManager;
SimManager         simManager;
CloudManager       cloudManager;
AlarmManager       alarmManager;
ControlEngine      controlEngine;
WebServerManager   webServer;
WaterManager       waterManager;

// ============================================================
//  WATCHDOG TIMEOUT
// ============================================================
#define WDT_TIMEOUT_MS  10000

// ============================================================
//  TASK CALLBACKS  (registered with Scheduler)
// ============================================================
void taskSensors() {
    pzemManager.update();
    tempManager.update();
    sensorManager.update();
    waterManager.update();  // AJ-SR04M water level
}

void taskAlarms() {
    alarmManager.evaluate();
}

void taskControl() {
    controlEngine.evaluate();
}

void taskNetwork() {
    networkManager.update();
}

void taskWebServer() {
    webServer.handle();   // process incoming HTTP requests
}

void taskSim() {
    simManager.update();  // keep GPRS alive, refresh CSQ
}

void taskSimSms() {
    simManager.checkSMS(); // check for configuration SMS commands
}

void taskCloud() {
    cloudManager.send();  // WiFi first, GPRS fallback
}

void taskHealth() {
    hpSystem.health.freeHeap    = ESP.getFreeHeap();
    hpSystem.health.minFreeHeap = ESP.getMinFreeHeap();
    hpSystem.health.cpuFreqMHz  = (float)ESP.getCpuFreqMHz();

    logger.logf(LogLevel::INFO,
                "[Health] Heap=%uB RSSI=%ddBm State=%s",
                hpSystem.health.freeHeap,
                hpSystem.wifi.rssi,
                stateToStr(hpSystem.state));
}

void taskNTP() {
    networkManager.syncTime();
}

// ============================================================
//  SETUP
// ============================================================
void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.println();
    Serial.println("===========================================");
    Serial.println("  HeatPump Controller  FW v1.0.0");
    Serial.println("===========================================");

    // 1. BOOTING
    hpSystem.state = DeviceState::BOOTING;

    // 1. BOOTING
    hpSystem.state = DeviceState::BOOTING;

    // 2. Load config from NVS flash
    configManager.begin();
    strlcpy(hpSystem.device.serialNumber,
            configManager.config.serialNumber,
            sizeof(hpSystem.device.serialNumber));

    // 3. Logger
    logger.begin();
    logger.logf(LogLevel::INFO, "Device: %s  FW: %s",
                hpSystem.device.serialNumber, FW_VERSION);

    // 4. Message Queue
    msgQueue.begin();

    // 5. SELF TEST
    hpSystem.state = DeviceState::SELF_TEST;
    logger.info("[Boot] Self-test started...");
    if (ESP.getFreeHeap() < 50000) {
        logger.critical("[Boot] WARNING: Low heap at startup!");
    } else {
        logger.logf(LogLevel::INFO, "[Boot] Self-test OK. Free heap: %u bytes",
                    ESP.getFreeHeap());
    }

    // 6. Network (WiFiManager captive portal on first boot)
    networkManager.begin();

    // 7. NTP Time Sync
    hpSystem.state = DeviceState::SYNC_TIME;
    networkManager.syncTime();

    // 8. Sensors & Logic
    pzemManager.begin();
    tempManager.begin();
    sensorManager.begin();
    alarmManager.begin();
    controlEngine.begin();
    waterManager.begin();    // AJ-SR04M — GPIO18=TRIG, GPIO36=ECHO

    // 8b. Cellular (SIM800L via TinyGSM)
    hpSystem.state = DeviceState::CONNECTING_CELLULAR;
    simManager.begin();

    // 9. Web Server + REST API
    webServer.begin();

    // 10. Register Scheduler tasks
    scheduler.add("Sensors",   configManager.config.pzemIntervalMs,   taskSensors);
    scheduler.add("Alarms",    configManager.config.pzemIntervalMs,   taskAlarms);
    scheduler.add("Control",   configManager.config.pzemIntervalMs,   taskControl);
    scheduler.add("Network",   100,                                   taskNetwork);
    scheduler.add("WebServer", 10,                                    taskWebServer);
    scheduler.add("Health",    30000,                                 taskHealth);
    scheduler.add("NTP",       configManager.config.ntpIntervalMs,    taskNTP);
    scheduler.add("Sim",       30000,                                 taskSim);
    scheduler.add("SimSMS",    10000,                                 taskSimSms);
    scheduler.add("Cloud",     configManager.config.cloudIntervalMs,  taskCloud);

    // 11. READY
    hpSystem.state = DeviceState::READY;

    Serial.printf("\n[READY] Dashboard at: http://%s\n\n", hpSystem.wifi.ip);
    logger.logf(LogLevel::INFO, "[Boot] System ready. Dashboard: http://%s",
                hpSystem.wifi.ip);
}

// ============================================================
//  LOOP
// ============================================================
void loop() {
    scheduler.tick();       // run all scheduled tasks
    yield();                // yield to WiFi stack
}
