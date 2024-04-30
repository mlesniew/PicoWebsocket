// Platform compatibility: espressif8266

#include <Arduino.h>
#include <Ethernet.h>

#include <PicoWebsocket.h>

byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };

EthernetClient ethernet_client;
PicoWebsocket::Client websocket(
    ethernet_client,    // Arduino Client to use
    "/mirror"           // HTTP path (optional, defaults to /)
);

void setup() {
    Serial.begin(115200);
    Serial.println("Connecting to network...");
    Ethernet.init(5); // ss pin
    while (!Ethernet.begin(mac)) {
        Serial.println("Failed, retrying...");
    }
    Serial.println(Ethernet.localIP());
}

void loop() {
    while (!websocket.connected() && !websocket.connect("ws.vi-server.org", 80)) {
        delay(1000);
    }

    Serial.println("--- connected ---");

    while (websocket.connected()) {
        yield();

        if (websocket.available()) {
            uint8_t buffer[128];
            const auto bytes_read = websocket.read(buffer, 128);
            Serial.write(buffer, bytes_read);
        }

        static unsigned int counter = 0;
        static unsigned long last_msg = millis();
        if (millis() - last_msg > 5000) {
            websocket.printf("Hello from PicoWebsocket #%u\n", ++counter);
            last_msg = millis();
        }
    }

    Serial.println("--- disconnected ---");
}
