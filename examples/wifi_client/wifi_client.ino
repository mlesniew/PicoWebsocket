#include <Arduino.h>

#if defined(ESP32)
#include <WiFi.h>
#elif defined(ESP8266)
#include <ESP8266WiFi.h>
#else
#error "This board is not supported."
#endif

#include <PicoWebsocket.h>

#if __has_include("config.h")
#include "config.h"
#endif

#ifndef WIFI_SSID
#define WIFI_SSID "WiFi SSID"
#endif

#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "password"
#endif

::WiFiClient wifi_client;
PicoWebsocket::Client websocket(
    wifi_client,    // Arduino Client to use
    "/mirror"       // HTTP path (optional, defaults to /)
);

void setup() {
    Serial.begin(115200);

    Serial.println("Connecting to WiFi...");
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED) { delay(100); }
    Serial.print("WiFi connected, IP: ");
    Serial.println(WiFi.localIP());
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
