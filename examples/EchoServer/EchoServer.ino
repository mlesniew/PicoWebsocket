#include <Arduino.h>
#include <WiFiServer.h>

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


::WiFiServer server(80);
PicoWebsocket::Server<::WiFiServer> websocket_server(server);

void setup() {
    Serial.begin(115200);

    Serial.println("Connecting to WiFi...");
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED) { delay(100); }
    Serial.print("WiFi connected, IP: ");
    Serial.println(WiFi.localIP());

    server.begin();
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
