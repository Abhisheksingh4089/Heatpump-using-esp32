#pragma once
#include <Arduino.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "SystemData.h"
#include "Logger.h"

// ✅  GPIO 13 = confirmed working for DS18B20 OneWire on this hardware.
// ⚠️  GPIO 15 = strapping pin — can cause boot conflicts with sensors.
// ⚠️  GPIO 4  = previously tried, caused issues — reverted to GPIO 13.
#define ONEWIRE_PIN          15
#define TEMP_RESOLUTION_BITS 12
#define TEMP_CONVERSION_MS   850UL   // 12-bit needs ~750ms; 850ms gives safe margin
#define TEMP_REDISCOVER_MS  3000UL   // retry discovery every 3s when no sensors

class TemperatureManager {
public:
    void begin() {
        logger.info("[Temp] Initialising TemperatureManager...");

        // Enable internal pull-up as backup — external 4.7kΩ between
        // DATA and 3.3V is still required for reliable operation.
        pinMode(ONEWIRE_PIN, INPUT_PULLUP);

        _oneWire = new OneWire(ONEWIRE_PIN);
        _sensors = new DallasTemperature(_oneWire);
        _sensors->begin();
        _sensors->setResolution(TEMP_RESOLUTION_BITS);
        // BLOCKING mode — safe here because taskSensors already waits
        // for PZEM reads. Eliminates the timing race in non-blocking mode
        // where a missed 800ms window causes -127 reads indefinitely.
        _sensors->setWaitForConversion(true);
        delay(100);  // let bus settle after pin mode change
        _discoverSensors();
    }

    // Force an immediate bus rescan — call this after any disruptive
    // event (SIM800L boot, power rail fluctuation, sensor reconnect).
    void forceRescan() {
        logger.info("[Temp] Force rescan triggered.");
        hpSystem.tempCount = 0;
        _lastDiscovery = 0;  // next update() call will re-discover immediately
    }

    // Called every cycle by taskSensors
    void update() {
        unsigned long now = millis();

        // ── Phase A: No sensors known → try re-discovery ─────
        if (hpSystem.tempCount == 0) {
            if (now - _lastDiscovery >= TEMP_REDISCOVER_MS) {
                _discoverSensors();
            }
            return;
        }

        // ── Blocking read: requestTemperatures() waits for conversion ──
        // setWaitForConversion(true) means this call blocks for ~850ms
        // then reads are immediately valid. Simple and reliable.
        _sensors->requestTemperatures();

        uint8_t onlineCount = 0;
        for (uint8_t i = 0; i < hpSystem.tempCount; i++) {
            _readSensor(i);
            if (hpSystem.temps[i].online) onlineCount++;
        }

        // All sensors dropped → re-discover next cycle
        if (onlineCount == 0) {
            logger.warning("[Temp] All sensors offline. Will re-discover.");
            hpSystem.alarms.tempSensorLost = true;
            hpSystem.tempCount = 0;
            _sensors->begin();  // re-scan bus
        }
    }

private:
    OneWire*           _oneWire      = nullptr;
    DallasTemperature* _sensors      = nullptr;
    unsigned long      _lastDiscovery = 0;

    void _discoverSensors() {
        _lastDiscovery = millis();

        // Clear all slots
        for (uint8_t i = 0; i < 8; i++) {
            hpSystem.temps[i] = TempSensor{};
        }

        _sensors->begin();  // re-scan bus
        uint8_t found = _sensors->getDeviceCount();
        hpSystem.tempCount = min((uint8_t)found, (uint8_t)8);
        logger.logf(LogLevel::INFO, "[Temp] Found %d DS18B20 sensor(s).", hpSystem.tempCount);

        for (uint8_t i = 0; i < hpSystem.tempCount; i++) {
            DeviceAddress addr;
            if (_sensors->getAddress(addr, i)) {
                memcpy(hpSystem.temps[i].address, addr, 8);
                hpSystem.temps[i].online = true;
                _sensors->setResolution(addr, TEMP_RESOLUTION_BITS);
                snprintf(hpSystem.temps[i].name, sizeof(hpSystem.temps[i].name),
                         "Sensor_%02X", addr[7]);
                logger.logf(LogLevel::INFO,
                    "[Temp] Sensor %d: %s  ROM=%02X%02X%02X%02X%02X%02X%02X%02X",
                    i, hpSystem.temps[i].name,
                    addr[0],addr[1],addr[2],addr[3],
                    addr[4],addr[5],addr[6],addr[7]);
            }
        }

        if (hpSystem.tempCount == 0) {
            hpSystem.alarms.tempSensorLost = true;
            logger.warning("[Temp] No sensors found. Retrying in 2s.");
        } else {
            hpSystem.alarms.tempSensorLost = false;
        }
    }

    void _readSensor(uint8_t i) {
        TempSensor& s = hpSystem.temps[i];
        float t = _sensors->getTempC(s.address);

        if (t <= -100.0f || isnan(t)) {
            if (s.online) {
                logger.logf(LogLevel::WARNING,
                    "[Temp] Sensor %d (%s) disconnected.", i, s.name);
            }
            s.online     = false;
            s.lastUpdate = millis();
            hpSystem.alarms.tempSensorLost = true;
            return;
        }

        s.value      = t;
        s.online     = true;
        s.lastUpdate = millis();
        hpSystem.alarms.tempSensorLost = false;
        logger.logf(LogLevel::INFO,
            "[Temp] Sensor %d (%s): %.2f°C", i, s.name, t);
    }
};

extern TemperatureManager tempManager;
