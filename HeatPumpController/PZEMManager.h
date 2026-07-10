#pragma once
#include <Arduino.h>
#include <PZEM004Tv30.h>
#include "SystemData.h"
#include "ConfigProfile.h"
#include "Logger.h"

// ============================================================
//  PZEM MANAGER  —  PZEM-004T v4.0
//  Library: PZEM004Tv30 by Olehs
//
//  Board: LILYGO T-Call V1.4 (ESP32-WROVER-B + SIM800H)
//
//  WIRING — SEPARATE RX PINS (fixes bus contention):
//
//    ESP32 GPIO33 (TX) ──┬──► PZEM 1 RX  (phase R)
//                        ├──► PZEM 2 RX  (phase Y)
//                        └──► PZEM 3 RX  (phase B)
//
//    PZEM 1 TX ──► ESP32 GPIO32   (RX for sensor 1)
//    PZEM 2 TX ──► ESP32 GPIO34   (RX for sensor 2)  ← NEW WIRE
//    PZEM 3 TX ──► ESP32 GPIO35   (RX for sensor 3)  ← NEW WIRE
//
//  WHY SEPARATE RX PINS?
//    TTL UART TX pins actively drive HIGH when idle. With 3 PZEM
//    TX lines shorted together, the 2 idle sensors (driving HIGH)
//    fight the transmitting sensor (driving LOW) — corrupting every
//    response frame. Separate RX pins eliminate this completely.
//    GPIO34 and GPIO35 are input-only pins, perfect for UART RX.
//
//  ⚠️  GPIO16/17 = PSRAM reserved
//  ⚠️  GPIO26/27 = SIM800H modem lines — DO NOT USE
// ============================================================

#define PZEM_TX_PIN   33   // ESP32 TX → ALL PZEM RX pins (shared)
#define PZEM_RX_PIN_0 32   // PZEM 1 TX → ESP32 (phase R)
#define PZEM_RX_PIN_1 34   // PZEM 2 TX → ESP32 (phase Y)  ← input-only pin
#define PZEM_RX_PIN_2 35   // PZEM 3 TX → ESP32 (phase B)  ← input-only pin

static const uint8_t PZEM_RX_PINS[3] = { PZEM_RX_PIN_0, PZEM_RX_PIN_1, PZEM_RX_PIN_2 };

// Modbus addresses — must match what is programmed in each sensor
static const uint8_t PZEM_ADDRS[3]   = { 0x01, 0x02, 0x03 };

class PZEMManager {
public:
    void begin() {
        logger.info("[PZEM] Initialising 3x PZEM v4.0 — separate RX pins mode");

        // Initialise Serial2 on the first sensor's pins; we will
        // reconfigure RX between reads for the other sensors.
        _pzem[0] = new PZEM004Tv30(Serial2, PZEM_RX_PIN_0, PZEM_TX_PIN, 0x01);
        _pzem[1] = new PZEM004Tv30(Serial2, PZEM_RX_PIN_0, PZEM_TX_PIN, 0x02);
        _pzem[2] = new PZEM004Tv30(Serial2, PZEM_RX_PIN_0, PZEM_TX_PIN, 0x03);

        logger.logf(LogLevel::INFO,
            "[PZEM] RX pins: %d / %d / %d   TX pin: %d",
            PZEM_RX_PIN_0, PZEM_RX_PIN_1, PZEM_RX_PIN_2, PZEM_TX_PIN);
    }

    void update() {
        if (millis() - _lastCycleTime >= 5000) {
            _lastCycleTime = millis();
            for (uint8_t i = 0; i < hpSystem.pzemCount; i++) {
                _readDevice(i);
                delay(300);  // idle gap between sensors
            }
        }
    }

    const PZEMData& getData(uint8_t index = 0) const {
        return hpSystem.pzem[index];
    }

    void resetEnergy(uint8_t index = 0) {
        if (_pzem[index]) {
            // Switch to the correct RX pin before issuing the reset command
            Serial2.begin(9600, SERIAL_8N1, PZEM_RX_PINS[index], PZEM_TX_PIN);
            delay(20);
            _pzem[index]->resetEnergy();
            logger.logf(LogLevel::INFO, "[PZEM] Energy reset for sensor %d.", index + 1);
        }
    }

private:
    PZEM004Tv30* _pzem[3]   = {nullptr, nullptr, nullptr};
    uint32_t _lastCycleTime = 0;

    void _flushRx() {
        while (Serial2.available()) (void)Serial2.read();
    }

    void _readDevice(uint8_t i) {
        if (_pzem[i] == nullptr) return;

        // ── Key step: route Serial2 RX to THIS sensor's dedicated pin ──
        // This completely isolates the sensor's TX line from the others,
        // eliminating bus contention. The shared TX pin is unaffected.
        Serial2.begin(9600, SERIAL_8N1, PZEM_RX_PINS[i], PZEM_TX_PIN);
        delay(20);       // let UART peripheral stabilise after reconfigure
        _flushRx();      // clear any noise picked up during reconfigure
        delay(20);       // short settle before driving TX

        // Read all values — .voltage() triggers the one Modbus query.
        // Remaining calls return from the library's internal 200ms cache.
        float v  = _pzem[i]->voltage();
        float a  = _pzem[i]->current();
        float w  = _pzem[i]->power();
        float e  = _pzem[i]->energy();
        float hz = _pzem[i]->frequency();
        float pf = _pzem[i]->pf();

        _flushRx();  // clean up trailing bytes

        PZEMData& d = hpSystem.pzem[i];

        if (isnan(v)) {
            bool wasOnline = d.online;
            d.online     = false;
            d.lastUpdate = millis();
            hpSystem.alarms.pzemOffline = true;
            logger.logf(LogLevel::WARNING,
                "[PZEM] Sensor %d (addr 0x%02X, RX=GPIO%d) %s",
                i + 1, PZEM_ADDRS[i], PZEM_RX_PINS[i],
                wasOnline ? "went OFFLINE" : "offline (no response)");
            return;
        }

        d.voltage     = v;
        d.current     = a;
        d.power       = w;
        d.energy      = e;
        d.frequency   = hz;
        d.powerFactor = pf;
        d.online      = true;
        d.lastUpdate  = millis();

        bool anyOffline = false;
        for (uint8_t j = 0; j < hpSystem.pzemCount; j++) {
            if (!hpSystem.pzem[j].online) { anyOffline = true; break; }
        }
        hpSystem.alarms.pzemOffline = anyOffline;

        logger.logf(LogLevel::INFO,
            "[PZEM%d] V=%.1fV  I=%.3fA  P=%.1fW  E=%.4fkWh  F=%.1fHz  PF=%.2f",
            i + 1, v, a, w, e, hz, pf);
    }
};

extern PZEMManager pzemManager;
