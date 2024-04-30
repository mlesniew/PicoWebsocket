#include <Arduino.h>
#include <WiFiClientSecure.h>

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

const char host[] = "echo.websocket.org";
/*
    Getting the server cert:
        openssl s_client -connect echo.websocket.org:443 < /dev/null > cert.crt
    Extracting the SHA1 fingerprint:
        openssl x509 -fingerprint -in cert.crt -noout | cut -d= -f2- | tr : ' ' | xargs -n1 printf "0x%s, "
*/
const uint8_t fingerprint[] = { 0xBB, 0x1A, 0x6B, 0x23, 0xF2, 0xAC, 0xE0, 0x44, 0x36, 0xB2, 0x63, 0x1D, 0x27, 0x3F, 0x45, 0x49, 0xE1, 0x11, 0xF0, 0x68 };

::WiFiClientSecure wifi_client;
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

    // configure secure client as usual before using it

#if defined(ESP8266)
    wifi_client.setFingerprint(fingerprint);
#elif defined(ESP32)
    // The ESP32 doesn't support the simple setFingerprint() function, so for this example,
    // we're disabling security altogether.  This will still establish an encrypted connection,
    // but won't verify the server's identity.
    wifi_client.setInsecure();
#else
    #error "Board not supported"
#endif
}

void loop() {
    while (!websocket.connected() && !websocket.connect(host, 443)) {
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
