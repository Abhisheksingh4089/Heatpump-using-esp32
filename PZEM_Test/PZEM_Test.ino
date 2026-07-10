// ============================================================
//  PZEM-004T DIAGNOSTIC TEST SKETCH
//  Upload this to ESP32, then open Serial Monitor at 115200.
//
//  Board: LILYGO T-Call V1.4 (ESP32-WROVER-B + SIM800H)
//
//  Wiring (LEFT side of board):
//    PZEM TX  →  ESP32 GPIO 25  (labeled "25" on PCB)
//    PZEM RX  →  ESP32 GPIO 26  (labeled "26" on PCB, right below 25)
//    PZEM VCC →  ESP32 5V pin
//    PZEM GND →  ESP32 GND pin
//
//  ⚠️  DO NOT use the TX/RX pins on the board (GPIO1/3 = USB serial)
//  ⚠️  GPIO16/17 = reserved by PSRAM inside WROVER-B module
//  ⚠️  GPIO32/33 = ADC-only on WROVER-B, unreliable as output (TX)
//
//  If you still see ERROR, swap the GPIO25 and GPIO26 wires.
// ============================================================

#include <PZEM004Tv30.h>

#define PZEM_RX_PIN  25   // PZEM TX → GPIO25
#define PZEM_TX_PIN  26   // PZEM RX → GPIO26

// Note: PZEM004Tv30 calls Serial2.begin() internally — do NOT call it again!
PZEM004Tv30 pzem(Serial2, PZEM_RX_PIN, PZEM_TX_PIN);

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("==========================================");
    Serial.println("  PZEM-004T Diagnostic Test");
    Serial.println("  PZEM TX → GPIO25   PZEM RX → GPIO26");
    Serial.println("==========================================");
}

void loop() {
    Serial.println("\n--- Reading PZEM ---");

    float voltage = pzem.voltage();

    if (isnan(voltage)) {
        Serial.println("❌ ERROR: No response from PZEM sensor!");
        Serial.println("   → Check RX/TX wiring (try swapping them)");
        Serial.println("   → Check 5V and GND connections");
        Serial.println("   → Make sure AC mains is connected to PZEM voltage terminals");
    } else {
        Serial.println("✅ PZEM is ONLINE!");
        Serial.print("   Voltage:      "); Serial.print(voltage);          Serial.println(" V");
        Serial.print("   Current:      "); Serial.print(pzem.current());   Serial.println(" A");
        Serial.print("   Power:        "); Serial.print(pzem.power());     Serial.println(" W");
        Serial.print("   Energy:       "); Serial.print(pzem.energy());    Serial.println(" kWh");
        Serial.print("   Frequency:    "); Serial.print(pzem.frequency()); Serial.println(" Hz");
        Serial.print("   Power Factor: "); Serial.println(pzem.pf());
    }

    delay(2000);
}
