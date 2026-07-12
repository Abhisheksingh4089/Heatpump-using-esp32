#pragma once
#include <Arduino.h>
#include <functional>   // for std::function

// ============================================================
//  LOGGER — RAM ring-buffer event log
//  forEach uses std::function so lambdas with captures work.
// ============================================================

#define LOG_MAX_ENTRIES  100
#define LOG_MSG_LEN       128

enum class LogLevel : uint8_t { INFO = 0, WARNING, ERROR, CRITICAL };

inline const char* logLevelStr(LogLevel l) {
    switch (l) {
        case LogLevel::INFO:     return "INFO";
        case LogLevel::WARNING:  return "WARN";
        case LogLevel::ERROR:    return "ERROR";
        case LogLevel::CRITICAL: return "CRIT";
        default:                 return "INFO";
    }
}

struct LogEntry {
    unsigned long timestamp;
    LogLevel      level;
    char          message[LOG_MSG_LEN];
};

class LogManager {
public:
    void begin() {
        _head  = 0;
        _count = 0;
        log(LogLevel::INFO, "Logger initialised.");
    }

    void log(LogLevel level, const char* msg) {
        LogEntry& e = _buffer[_head];
        e.timestamp = millis();
        e.level     = level;
        strncpy(e.message, msg, LOG_MSG_LEN - 1);
        e.message[LOG_MSG_LEN - 1] = '\0';
        _head = (_head + 1) % LOG_MAX_ENTRIES;
        if (_count < LOG_MAX_ENTRIES) _count++;
        Serial.printf("[%s][%8lu] %s\n", logLevelStr(level), e.timestamp, msg);
    }

    void info(const char* msg)     { log(LogLevel::INFO,     msg); }
    void warning(const char* msg)  { log(LogLevel::WARNING,  msg); }
    void error(const char* msg)    { log(LogLevel::ERROR,    msg); }
    void critical(const char* msg) { log(LogLevel::CRITICAL, msg); }

    void logf(LogLevel level, const char* fmt, ...) {
        char buf[LOG_MSG_LEN];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        log(level, buf);
    }

    // std::function allows lambdas WITH captures (e.g. [&arr])
    void forEach(std::function<void(const LogEntry&)> callback) const {
        if (_count == 0) return;
        int start = (_count < LOG_MAX_ENTRIES) ? 0 : _head;
        for (int i = 0; i < _count; i++) {
            callback(_buffer[(start + i) % LOG_MAX_ENTRIES]);
        }
    }

    uint16_t count() const { return _count; }

private:
    LogEntry  _buffer[LOG_MAX_ENTRIES];
    int       _head  = 0;
    uint16_t  _count = 0;
};

extern LogManager logger;
