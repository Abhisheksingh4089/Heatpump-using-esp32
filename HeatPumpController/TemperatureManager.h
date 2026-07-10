#pragma once
#include <Arduino.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "SystemData.h"
#include "Logger.h"

#define ONEWIRE_PIN          13   // Changed from 15 — GPIO15 is a strapping pin on ESP32/T-Call
#define TEMP_RESOLUTION_BITS 12

class TemperatureManager {
public:
    void begin() {
        logger.info("[Temp] Initialising TemperatureManager...");
        _oneWire = new OneWire(ONEWIRE_PIN);
        _sensors = new DallasTemperature(_oneWire);
        _sensors->begin();
        _sensors->setResolution(TEMP_RESOLUTION_BITS);
        _sensors->setWaitForConversion(false);
        _discoverSensors();
    }

    void update() {
        if (hpSystem.tempCount == 0) { _discoverSensors(); return; }

        _sensors->requestTemperatures();
        unsigned long t = millis();
        while (!_sensors->isConversionComplete()) {
            if (millis() - t > 1000) { logger.error("[Temp] Conversion timeout."); break; }
            yield();
        }
        for (uint8_t i = 0; i < hpSystem.tempCount; i++) _readSensor(i);
    }

private:
    OneWire*           _oneWire = nullptr;
    DallasTemperature* _sensors = nullptr;

    void _discoverSensors() {
        uint8_t found = _sensors->getDeviceCount();
        hpSystem.tempCount = min((uint8_t)found, (uint8_t)8);
        logger.logf(LogLevel::INFO, "[Temp] Found %d DS18B20 sensor(s).", hpSystem.tempCount);

        for (uint8_t i = 0; i < hpSystem.tempCount; i++) {
            DeviceAddress addr;
            if (_sensors->getAddress(addr, i)) {
                memcpy(hpSystem.temps[i].address, addr, 8);
                hpSystem.temps[i].online = true;
                snprintf(hpSystem.temps[i].name, sizeof(hpSystem.temps[i].name),
                         "Sensor_%02X", addr[7]);
                logger.logf(LogLevel::INFO, "[Temp] Sensor %d ROM=%02X%02X%02X%02X%02X%02X%02X%02X",
                            i, addr[0],addr[1],addr[2],addr[3],addr[4],addr[5],addr[6],addr[7]);
            }
        }

        if (hpSystem.tempCount == 0) {
            hpSystem.alarms.tempSensorLost = true;
            logger.warning("[Temp] No DS18B20 sensors found.");
        }
    }

    void _readSensor(uint8_t i) {
        TempSensor& s = hpSystem.temps[i];
        float t = _sensors->getTempC(s.address);

        if (t == DEVICE_DISCONNECTED_C || isnan(t)) {
            if (s.online) logger.logf(LogLevel::WARNING, "[Temp] Sensor %d lost.", i);
            s.online = false;
            s.lastUpdate = millis();
            hpSystem.alarms.tempSensorLost = true;
            return;
        }

        s.value  = t;
        s.online = true;
        s.lastUpdate = millis();
        hpSystem.alarms.tempSensorLost = false;
        logger.logf(LogLevel::INFO, "[Temp] Sensor %d (%s): %.2f°C", i, s.name, s.value);
    }
};

extern TemperatureManager tempManager;
