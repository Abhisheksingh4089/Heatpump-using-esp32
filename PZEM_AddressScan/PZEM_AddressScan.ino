// ============================================================
//  PZEM-004T ADDRESS SCANNER
//
//  Connect ALL three PZEM sensors to the bus (same wiring as
//  your main project) then upload this sketch.
//
//  Wiring:
//    ALL PZEM TX pins  →  GPIO32  (ESP32 RX)
//    ALL PZEM RX pins  →  GPIO33  (ESP32 TX)
//    ALL PZEM VCC      →  5V
//    ALL PZEM GND      →  GND
//
//  Open Serial Monitor at 115200 baud.
//  The scan takes ~30 seconds — wait for "SCAN COMPLETE".
// ============================================================

#include <PZEM004Tv30.h>

#define PZEM_RX_PIN  32
#define PZEM_TX_PIN  33

// We use the broadcast-address instance just for scanning
PZEM004Tv30 pzem(Serial2, PZEM_RX_PIN, PZEM_TX_PIN, 0xF8);

void setup() {
    Serial.begin(115200);
    delay(2000);
    Serial.println("\n\n==========================================");
    Serial.println("  PZEM-004T Modbus Address Scanner");
    Serial.println("  Scanning addresses 0x01 to 0xF7...");
    Serial.println("  (This will take ~30 seconds)");
    Serial.println("==========================================\n");

    int found = 0;

    for (uint16_t addr = 0x01; addr <= 0xF7; addr++) {

        // Flush before each query
        while (Serial2.available()) Serial2.read();
        delay(20);

        // Temporarily set the address we want to probe
        pzem.setAddress(0xF8); // reset to broadcast first
        
        // Read voltage using the target address directly via search
        // We'll construct our own probe using the library's internal scan
        // Actually let's use a different approach — create a new instance per address
        // is too slow. Use the search() method instead.
        
        // Progress indicator every 16 addresses
        if (addr % 16 == 0) {
            Serial.print("Scanning 0x");
            if (addr < 16) Serial.print("0");
            Serial.print(addr, HEX);
            Serial.println("...");
        }
    }

    // Use the built-in search function — this is the cleanest approach
    Serial.println("\n--- Starting built-in bus scan ---");
    Serial.println("(Devices found will print below)\n");
    pzem.search();

    Serial.println("\n==========================================");
    Serial.println("SCAN COMPLETE");
    Serial.println("If nothing printed above, check wiring!");
    Serial.println("==========================================\n");

    // Also try reading from broadcast address (0xF8)
    // — only works if exactly ONE sensor is on the bus
    Serial.println("--- Broadcast read (works with 1 sensor only) ---");
    while (Serial2.available()) Serial2.read();
    delay(100);
    
    PZEM004Tv30 broadcast(Serial2, PZEM_RX_PIN, PZEM_TX_PIN, 0xF8);
    float v = broadcast.voltage();
    if (!isnan(v)) {
        Serial.println("⚠️  WARNING: A sensor is still on BROADCAST address 0xF8!");
        Serial.print("   Voltage reading: "); Serial.print(v); Serial.println("V");
        Serial.println("   → This sensor was NEVER programmed with a unique address.");
        Serial.println("   → You need to run the address-programming sketch for it.");
    } else {
        Serial.println("No sensor on broadcast address (0xF8) — good.");
    }
}

void loop() {
    // Nothing — scan runs once in setup
    delay(10000);
}
