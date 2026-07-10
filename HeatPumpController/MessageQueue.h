#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// ============================================================
//  MESSAGE QUEUE  —  Store & Forward Cloud Telemetry
//
//  CloudManager does NOT read SystemData directly.
//  Instead:
//      MessageBuilder  ->  MessageQueue  ->  CloudManager
//
//  If WiFi drops, messages stay in the queue.
//  When WiFi returns, the queue drains automatically.
//
//  Phase 1: Queue is RAM-only (max 50 messages).
//  Future:  Overflow to LittleFS for persistent buffering.
// ============================================================

#define MSG_QUEUE_SIZE    50
#define MSG_PAYLOAD_LEN  512

struct QueueMessage {
    unsigned long timestamp = 0;        // millis() when enqueued
    char payload[MSG_PAYLOAD_LEN] = ""; // JSON string
    bool sent = false;
};

class MessageQueue {
public:
    void begin() {
        _head  = 0;
        _tail  = 0;
        _count = 0;
        Serial.println("[MsgQueue] Initialized. Capacity: " + String(MSG_QUEUE_SIZE));
    }

    // Enqueue a JSON string. Oldest message is dropped if full.
    bool enqueue(const char* jsonPayload) {
        if (_count >= MSG_QUEUE_SIZE) {
            // Drop oldest to make space (oldest data sacrificed)
            _tail = (_tail + 1) % MSG_QUEUE_SIZE;
            _count--;
            Serial.println("[MsgQueue] WARN: Queue full — oldest message dropped.");
        }
        QueueMessage& m = _buffer[_head];
        m.timestamp = millis();
        m.sent      = false;
        strncpy(m.payload, jsonPayload, MSG_PAYLOAD_LEN - 1);
        m.payload[MSG_PAYLOAD_LEN - 1] = '\0';

        _head = (_head + 1) % MSG_QUEUE_SIZE;
        _count++;
        return true;
    }

    // Peek at the oldest unsent message (returns nullptr if empty)
    const QueueMessage* peek() const {
        if (_count == 0) return nullptr;
        return &_buffer[_tail];
    }

    // Remove the oldest message after successful send
    void pop() {
        if (_count == 0) return;
        _tail = (_tail + 1) % MSG_QUEUE_SIZE;
        _count--;
    }

    uint16_t depth()   const { return _count; }
    bool     isEmpty() const { return _count == 0; }
    bool     isFull()  const { return _count >= MSG_QUEUE_SIZE; }

private:
    QueueMessage _buffer[MSG_QUEUE_SIZE];
    int      _head  = 0;
    int      _tail  = 0;
    uint16_t _count = 0;
};

extern MessageQueue msgQueue;
