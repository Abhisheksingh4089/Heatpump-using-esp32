#pragma once
#include <Arduino.h>
#include "SystemData.h"
#include "ConfigProfile.h"
#include "Logger.h"

// ============================================================
//  WATER MANAGER  —  AJ-SR04M Waterproof Ultrasonic Sensor
//
//  Board: LILYGO T-Call V1.4 (ESP32-WROVER-B)
//
//  WIRING:
//    AJ-SR04M VCC  → 5V (preferred for max range) or 3.3V
//    AJ-SR04M GND  → GND
//    AJ-SR04M TRIG → GPIO 18  (output)
//    AJ-SR04M ECHO → GPIO 36  (input-only pin — perfect for echo)
//
//  TWO-POINT CALIBRATION (set from dashboard — no reflash needed):
//    emptyDistanceCm = sensor reading when tank is completely EMPTY
//    fullDistanceCm  = sensor reading when tank is completely FULL
//
//  FORMULA:
//    level% = (emptyDist - currentDist) / (emptyDist - fullDist) × 100
//
//  NOISE REJECTION: 5 samples → sort → drop min/max → average → exp filter
//
//  WATER LEVEL STATES:
//    FULL ≥90% | HIGH ≥70% | NORMAL ≥40% | LOW ≥lowAlarm% | CRITICAL ≥shutoff% | EMPTY
// ============================================================

#define WATER_TRIG_PIN         18
#define WATER_ECHO_PIN          2   // GPIO2 (has onboard LED — harmless for ECHO input)
                                    // GPIO36 not exposed on T-Call SM version

// AJ-SR04M timing constants
// Standard /58 formula: distance(cm) = echo_duration(µs) / 58
#define AJ_SR04M_ECHO_DIVISOR    58.0f   // standard round-trip speed-of-sound formula
#define AJ_SR04M_MIN_DIST_CM      2.0f   // software min (sensor handles physical blind zone)
#define AJ_SR04M_MAX_RANGE_CM   450.0f   // practical max
#define ECHO_TIMEOUT_US        38000UL   // ~650cm round-trip
#define WATER_SAMPLE_COUNT      7        // more samples = better median
#define WATER_SAMPLE_DELAY_MS  30        // ms between samples
#define WATER_FILTER_ALPHA      0.6f     // 0=frozen, 1=raw — higher = faster response

#define WATER_READING_TOO_CLOSE  -1.0f
#define WATER_READING_TIMEOUT    -2.0f

class WaterManager {
public:

    // -----------------------------------------------------------
    //  begin() — call once in setup()
    // -----------------------------------------------------------
    void begin() {
        pinMode(WATER_TRIG_PIN, OUTPUT);
        pinMode(WATER_ECHO_PIN, INPUT);   // required on ESP32 — must be explicit
        digitalWrite(WATER_TRIG_PIN, LOW);
        delay(500);   // let AJ-SR04M stabilise after power-on

        const ConfigProfile& cfg = configManager.config;
        logger.logf(LogLevel::INFO,
            "[Water] AJ-SR04M init | EmptyDist=%.0fcm | FullDist=%.0fcm | "
            "Shutoff=%.0f%% | LowAlarm=%.0f%%",
            cfg.emptyDistanceCm, cfg.fullDistanceCm,
            cfg.waterShutoffPercent, cfg.waterLowAlarmPercent);

        // Discard 3 warmup readings (AJ-SR04M gives garbage on cold start)
        for (uint8_t i = 0; i < 3; i++) { _takeSingleSample(); delay(100); }

        // Prime filter — reject obviously bogus readings (> emptyDist + 50cm)
        float firstRead = _measureMedian();
        float sanityMax = cfg.emptyDistanceCm + 50.0f;
        _filteredDistanceCm = (firstRead > 0 && firstRead < sanityMax)
                              ? firstRead
                              : cfg.emptyDistanceCm;
        logger.logf(LogLevel::INFO, "[Water] Filter primed at %.1f cm",
                    _filteredDistanceCm);
    }

    // -----------------------------------------------------------
    //  update() — called by taskSensors() every 1000 ms
    // -----------------------------------------------------------
    void update() {
        float raw = _measureMedian();

        // Stuck detection: if too many consecutive errors, reset filter
        if (raw < 0) {
            _errorCount++;
            if (_errorCount >= 10) {
                logger.warning("[Water] 10 consecutive errors — resetting filter.");
                _filteredDistanceCm = configManager.config.emptyDistanceCm;
                _errorCount = 0;
            }
        } else {
            _errorCount = 0;
        }

        _applyFilter(raw);
        _processReading(_filteredDistanceCm, raw);
    }

private:
    float _filteredDistanceCm = 0.0f;   // exponential filter state
    uint8_t _errorCount       = 0;       // consecutive bad readings

    // ── Single TRIG → ECHO measurement ───────────────────────
    // pulseIn() is reliable on ESP32 for this use case — the ±timing jitter
    // is offset by the 7-sample median. esp_timer busy-wait was worse because
    // FreeRTOS could preempt between TRIG and echo monitoring, missing pulses.
    float _takeSingleSample() {
        digitalWrite(WATER_TRIG_PIN, LOW);
        delayMicroseconds(4);
        digitalWrite(WATER_TRIG_PIN, HIGH);
        delayMicroseconds(10);
        digitalWrite(WATER_TRIG_PIN, LOW);

        unsigned long duration = pulseIn(WATER_ECHO_PIN, HIGH, ECHO_TIMEOUT_US);
        if (duration == 0) return WATER_READING_TIMEOUT;

        float distCm = (float)duration / AJ_SR04M_ECHO_DIVISOR;

        if (distCm < AJ_SR04M_MIN_DIST_CM) return WATER_READING_TOO_CLOSE;
        if (distCm > AJ_SR04M_MAX_RANGE_CM) return WATER_READING_TIMEOUT;

        return distCm;
    }

    // ── 5-sample median with min/max outlier rejection ───────
    float _measureMedian() {
        float valid[WATER_SAMPLE_COUNT];
        uint8_t validCount = 0;
        bool    anyTooClose = false;

        for (uint8_t i = 0; i < WATER_SAMPLE_COUNT; i++) {
            float s = _takeSingleSample();

            if (s == WATER_READING_TOO_CLOSE) {
                anyTooClose = true;           // count blind-zone hits separately
            } else if (s > 0) {
                valid[validCount++] = s;       // only store valid cm readings
            }
            delay(WATER_SAMPLE_DELAY_MS);
        }

        // If most readings hit the blind zone, tank is very full
        if (anyTooClose && validCount < 2) return WATER_READING_TOO_CLOSE;

        // Not enough valid readings → sensor error
        if (validCount < 3) return WATER_READING_TIMEOUT;

        // Insertion sort (fast for tiny arrays)
        for (uint8_t i = 1; i < validCount; i++) {
            float key = valid[i];
            int8_t j  = (int8_t)i - 1;
            while (j >= 0 && valid[j] > key) {
                valid[j + 1] = valid[j];
                j--;
            }
            valid[j + 1] = key;
        }

        // Average middle readings (drop highest & lowest → outlier rejection)
        float sum = 0.0f;
        uint8_t midCount = 0;
        for (uint8_t i = 1; i < validCount - 1; i++) {
            sum += valid[i];
            midCount++;
        }

        return (midCount > 0) ? (sum / (float)midCount) : valid[validCount / 2];
    }

    // ── Exponential low-pass filter ──────────────────────────
    //  Smooths readings between cycles (handles wavy water surfaces)
    //  Formula: filtered = alpha * newValue + (1 - alpha) * filtered
    void _applyFilter(float rawReading) {
        if (rawReading < 0) return;   // don't filter error sentinel values

        if (_filteredDistanceCm <= 0.0f) {
            _filteredDistanceCm = rawReading;   // first valid reading → init filter
        } else {
            _filteredDistanceCm = WATER_FILTER_ALPHA * rawReading
                                + (1.0f - WATER_FILTER_ALPHA) * _filteredDistanceCm;
        }
    }

    // ── Convert filtered distance → water level % (two-point calibration) ──
    void _processReading(float filteredDist, float rawDist) {
        WaterTankData&       w   = hpSystem.water;
        const ConfigProfile& cfg = configManager.config;

        float emptyDist = cfg.emptyDistanceCm;  // reading when tank is empty
        float fullDist  = cfg.fullDistanceCm;   // reading when tank is full
        float span      = emptyDist - fullDist; // total measurable span

        // ── Sensor offline ──
        if (filteredDist == WATER_READING_TIMEOUT || filteredDist <= 0.0f) {
            if (w.sensorOnline) logger.warning("[Water] Sensor offline.");
            w.sensorOnline  = false;
            w.shutoffActive = true;
            strlcpy(w.waterLevelState, "SENSOR_ERROR", sizeof(w.waterLevelState));
            w.lastUpdate = millis();
            hpSystem.alarms.criticalWaterLevel = true;
            return;
        }

        // ── Tank full (reading <= fullDistanceCm) ──
        if (filteredDist <= fullDist || rawDist == WATER_READING_TOO_CLOSE) {
            w.sensorOnline      = true;
            w.sensorDistanceCm  = filteredDist > 0 ? filteredDist : fullDist;
            w.waterHeightCm     = span;
            w.waterLevelPercent = 100.0f;
            w.shutoffActive     = false;
            strlcpy(w.waterLevelState, "FULL", sizeof(w.waterLevelState));
            w.lastUpdate = millis();
            return;
        }

        // ── Normal range: two-point calibration ──
        // pct = (emptyDist - currentDist) / (emptyDist - fullDist) × 100
        // At empty: dist=emptyDist → pct=0%   At full: dist=fullDist → pct=100%
        w.sensorOnline     = true;
        w.sensorDistanceCm = filteredDist;
        w.waterHeightCm    = emptyDist - filteredDist;  // cm of water above bottom

        float pct = (span > 0.0f)
                    ? ((emptyDist - filteredDist) / span) * 100.0f
                    : 0.0f;
        pct = constrain(pct, 0.0f, 100.0f);
        w.waterLevelPercent = pct;

        const char* state;
        if      (pct >= 90.0f)                     state = "FULL";
        else if (pct >= 70.0f)                     state = "HIGH";
        else if (pct >= 40.0f)                     state = "NORMAL";
        else if (pct >= cfg.waterLowAlarmPercent)  state = "LOW";
        else if (pct >= cfg.waterShutoffPercent)   state = "CRITICAL";
        else                                        state = "EMPTY";

        strlcpy(w.waterLevelState, state, sizeof(w.waterLevelState));
        w.shutoffActive = (pct < cfg.waterShutoffPercent);
        w.lastUpdate    = millis();

        logger.logf(LogLevel::INFO,
            "[Water] Dist=%.1fcm | Level=%.1f%% | State=%s",
            filteredDist, pct, state);
    }
};

extern WaterManager waterManager;
