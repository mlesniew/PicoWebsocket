// Platform compatibility: espressif8266

#include <Arduino.h>
#include <Ethernet.h>

#include <PicoWebsocket.h>

byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };

EthernetServer server(80);
PicoWebsocket::Server<EthernetServer> websocket_server(server);

void setup() {
    Serial.begin(115200);

    Serial.println("Connecting to network...");
    Ethernet.init(5); // ss pin
    while (!Ethernet.begin(mac)) {
        Serial.println("Failed, retrying...");
    }
    Serial.println(Ethernet.localIP());

    websocket_server.begin();
}

void loop() {
    auto websocket = websocket_server.accept();
    if (!websocket) {
        return;
    }

    while (websocket.connected()) {
        yield();

        if (websocket.available()) {
            uint8_t buffer[128];
            const auto bytes_read = websocket.read(buffer, 128);
            websocket.write(buffer, bytes_read);
        }
    }
}
