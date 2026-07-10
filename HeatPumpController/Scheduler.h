#pragma once
#include <Arduino.h>

// ============================================================
//  SCHEDULER  —  Central Task Table
//
//  Eliminates scattered millis() checks across managers.
//  Each task has a name, period, and last-run timestamp.
//  The Scheduler's tick() is called every loop() iteration.
//
//  Usage:
//      scheduler.add("PZEM",    1000, []{ pzemManager.update(); });
//      scheduler.add("Cloud",   5000, []{ cloudManager.upload(); });
//      // in loop():
//      scheduler.tick();
// ============================================================

#define SCHEDULER_MAX_TASKS 16

struct Task {
    char          name[24]  = "";
    uint32_t      periodMs  = 0;
    unsigned long lastRun   = 0;
    bool          enabled   = true;
    void (*callback)()      = nullptr;

    // Runtime stats (useful for health diagnostics)
    uint32_t      runCount  = 0;
    uint32_t      lastDuration = 0;  // microseconds
};

class Scheduler {
public:
    // Register a periodic task. Returns task index or -1 if full.
    int add(const char* name, uint32_t periodMs, void (*callback)()) {
        if (_count >= SCHEDULER_MAX_TASKS) {
            Serial.println("[Scheduler] ERROR: Task table full!");
            return -1;
        }
        Task& t    = _tasks[_count];
        strncpy(t.name, name, sizeof(t.name) - 1);
        t.periodMs = periodMs;
        t.lastRun  = 0;     // run immediately on first tick
        t.enabled  = true;
        t.callback = callback;
        Serial.printf("[Scheduler] Task added: %-16s period=%ums\n", name, periodMs);
        return _count++;
    }

    // Enable / disable a task by index
    void enable(int idx)  { if (_valid(idx)) _tasks[idx].enabled = true;  }
    void disable(int idx) { if (_valid(idx)) _tasks[idx].enabled = false; }

    // Change period at runtime (e.g., slow down cloud upload on low heap)
    void setPeriod(int idx, uint32_t periodMs) {
        if (_valid(idx)) _tasks[idx].periodMs = periodMs;
    }

    // Must be called every loop()
    void tick() {
        unsigned long now = millis();
        for (int i = 0; i < _count; i++) {
            Task& t = _tasks[i];
            if (!t.enabled || t.callback == nullptr) continue;
            if (now - t.lastRun >= t.periodMs) {
                unsigned long before = micros();
                t.callback();
                t.lastDuration = (uint32_t)(micros() - before);
                t.lastRun      = now;
                t.runCount++;
            }
        }
    }

    // Access task info (for /api/health)
    const Task* getTask(int idx) const {
        return _valid(idx) ? &_tasks[idx] : nullptr;
    }
    int count() const { return _count; }

private:
    Task _tasks[SCHEDULER_MAX_TASKS];
    int  _count = 0;

    bool _valid(int idx) const { return idx >= 0 && idx < _count; }
};

extern Scheduler scheduler;
