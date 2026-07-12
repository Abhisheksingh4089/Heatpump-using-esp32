#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <esp_mac.h>
#include "SystemData.h"
#include "Logger.h"

// ============================================================
//  HP NETWORK MANAGER
//
//  NOTE: Renamed from 'NetworkManager' to 'HPNetworkManager'
//  because ESP32 Arduino core 3.x defines its own
//  NetworkManager class in <Network.h> which causes a
//  redefinition conflict.
//
//  Responsibilities:
//    1. WiFi provisioning via WiFiManager captive portal
//    2. Auto-reconnect on drop
//    3. RSSI monitoring
//    4. NTP time sync
//    5. Uptime tracking
// ============================================================

#define WIFI_MANAGER_AP_SSID     "HeatPump-Setup"
#define WIFI_MANAGER_AP_PASS     "heatpump123"
#define WIFI_CONNECT_TIMEOUT_SEC  60
#define NTP_SERVER               "pool.ntp.org"
#define NTP_OFFSET_SEC           0       // UTC only — gmtime() in CloudManager needs pure UTC
#define NTP_UPDATE_INTERVAL_MS   3600000UL

class HPNetworkManager {
public:
    void begin() {
        logger.info("[Net] Initialising HPNetworkManager...");

        // Read true factory MAC directly from eFuse hardware (no WiFi init needed)
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);

        snprintf(hpSystem.device.macAddress, sizeof(hpSystem.device.macAddress),
                 "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
                 
        snprintf(hpSystem.device.deviceId, sizeof(hpSystem.device.deviceId),
                 "%02X%02X%02X", mac[3], mac[4], mac[5]);

        logger.logf(LogLevel::INFO, "[Net] Device ID: %s  MAC: %s",
                    hpSystem.device.deviceId, hpSystem.device.macAddress);

        _connectWithManager();
        _ntpBegin();
    }

    // Called by Scheduler every 100ms
    void update() {
        _checkConnection();
        _updateRSSI();
        _updateUptime();
        _ntpUpdate();
    }

    void syncTime() {
        if (WiFi.status() == WL_CONNECTED && _ntpClient) {
            _ntpClient->forceUpdate();
            hpSystem.wifi.ntpSynced = _ntpClient->isTimeSet();
            logger.logf(LogLevel::INFO, "[Net] NTP synced: %s",
                        _ntpClient->getFormattedTime().c_str());
        }
    }

    unsigned long epochTime() {
        return (_ntpClient && hpSystem.wifi.ntpSynced)
               ? _ntpClient->getEpochTime() : 0;
    }

private:
    WiFiUDP*   _udp       = nullptr;
    NTPClient* _ntpClient = nullptr;
    unsigned long _lastNtpUpdate = 0;

    void _connectWithManager() {
        hpSystem.state = DeviceState::CONNECTING_WIFI;
        logger.info("[Net] Starting non-blocking WiFi setup...");

        // --- Persistent Local Dashboard AP ---
        // We ALWAYS broadcast the Dashboard AP instantly so there is NO blocking time.
        // You can view the UI locally even if you have no router internet.
        WiFi.mode(WIFI_AP_STA);
        WiFi.softAP("HeatPump-Dashboard", "12345678");
        logger.info("[Net] AP Started! Connect to WiFi 'HeatPump-Dashboard' (pass: 12345678) -> http://192.168.4.1");

        // Set default IP to AP IP so WebServer prints it correctly before Station connects
        String("192.168.4.1").toCharArray(hpSystem.wifi.ip, sizeof(hpSystem.wifi.ip));
        hpSystem.wifi.connected = false;
        hpSystem.alarms.wifiLost = true;

        // Try to connect to saved WiFi in the background
        if (strlen(configManager.config.wifiSsid) > 0) {
            logger.logf(LogLevel::INFO, "[Net] Connecting in background to saved WiFi: %s", configManager.config.wifiSsid);
            WiFi.begin(configManager.config.wifiSsid, configManager.config.wifiPass);
            // The _checkConnection() method in the update loop will detect when it successfully connects!
        } else {
            logger.warning("[Net] No WiFi saved. Running on SIM. Use the Dashboard at 192.168.4.1 to configure WiFi!");
        }
    }

    void _onConnected() {
        hpSystem.wifi.connected  = true;
        hpSystem.alarms.wifiLost = false;
        WiFi.localIP().toString().toCharArray(hpSystem.wifi.ip, sizeof(hpSystem.wifi.ip));
        WiFi.SSID().toCharArray(hpSystem.wifi.ssid, sizeof(hpSystem.wifi.ssid));
        hpSystem.wifi.rssi = WiFi.RSSI();

        logger.logf(LogLevel::INFO, "[Net] WiFi Station connected. SSID=%s  RSSI=%ddBm",
                    hpSystem.wifi.ssid, hpSystem.wifi.rssi);

        // Print the real router-assigned IP prominently so user can find the dashboard.
        // setup() always prints 192.168.4.1 (AP IP) because WiFi isn't connected yet then.
        Serial.println();
        Serial.println("╔══════════════════════════════════════════╗");
        Serial.printf( "║  WiFi Connected! SSID: %-18s║\n", hpSystem.wifi.ssid);
        Serial.printf( "║  Local Dashboard: http://%-17s║\n", hpSystem.wifi.ip);
        Serial.printf( "║  AP Dashboard:    http://192.168.4.1     ║\n");
        Serial.println("╚══════════════════════════════════════════╝");
        Serial.println();
    }

    void _checkConnection() {
        bool nowConnected = (WiFi.status() == WL_CONNECTED);
        if (!nowConnected && hpSystem.wifi.connected) {
            hpSystem.wifi.connected  = false;
            hpSystem.alarms.wifiLost = true;
            logger.warning("[Net] WiFi disconnected. Reconnecting with saved credentials...");
            // Use saved config credentials explicitly — NOT WiFi.reconnect() which
            // reconnects to whatever was last passed to WiFi.begin(), which may be
            // stale or empty after a disconnect(true) call from SimManager.
            if (strlen(configManager.config.wifiSsid) > 0) {
                WiFi.begin(configManager.config.wifiSsid, configManager.config.wifiPass);
            }
        }
        if (nowConnected && !hpSystem.wifi.connected) {
            _onConnected();
        }
    }

    void _updateRSSI() {
        if (hpSystem.wifi.connected)
            hpSystem.wifi.rssi = WiFi.RSSI();
    }

    void _updateUptime() {
        hpSystem.health.uptime = millis() / 1000;
    }

    void _ntpBegin() {
        _udp       = new WiFiUDP();
        _ntpClient = new NTPClient(*_udp, NTP_SERVER, NTP_OFFSET_SEC, NTP_UPDATE_INTERVAL_MS);
        _ntpClient->begin();
    }

    void _ntpUpdate() {
        if (!hpSystem.wifi.connected) return;
        // Run immediately on first connect (_lastNtpUpdate == 0), then every hour
        bool firstSync = (_lastNtpUpdate == 0);
        if (!firstSync && (millis() - _lastNtpUpdate < NTP_UPDATE_INTERVAL_MS)) return;
        _ntpClient->update();
        hpSystem.wifi.ntpSynced = _ntpClient->isTimeSet();
        _lastNtpUpdate = millis();
        if (firstSync && hpSystem.wifi.ntpSynced) {
            logger.logf(LogLevel::INFO, "[Net] NTP first sync OK. Epoch: %lu", _ntpClient->getEpochTime());
        }
    }
};

extern HPNetworkManager networkManager;
