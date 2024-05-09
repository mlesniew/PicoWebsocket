# PicoWebsocket

PicoWebsocket is a websocket library for ESP8266 and ESP32 devices offering a
seamless and intuitive interface, enabling effortless integration with any
Arduino framework sockets.

![Build](https://github.com/mlesniew/PicoWebsocket/actions/workflows/ci.yml/badge.svg) ![License](https://img.shields.io/github/license/mlesniew/PicoWebsocket) 

PicoWebsocket let's you easily:
* Create client WebSockets on top of any Arduino client class, e.g. `WiFiClient`, `EthernetClient` or `WiFiClientSecure`.
* Create WebSocket servers on top of any Arduino server class, e.g. `WiFiServer`, `EthernetServer` or `WiFiServerSecure`.
* Use websocket server and client instances just like the raw underlying classes providing the same interface.


## Quickstart

To get started, try compiling and running the code below or explore [examples](examples).

### Client

```
#include <Arduino.h>
#include <ESP8266WiFi.h>        // use <WiFi.h> on the ESP32

#include <PicoWebsocket.h>

// Define the client -- any Arduino Client subclass can be
// used here (e.g. EthernetClient)
WiFiClient client;

// Define the websocket
PicoWebsocket::Client websocket(client);

void setup() {
    Serial.begin(115200);
    WiFi.begin("network", "secret");
}

void loop() {
    // Now we can use websocket just like we would use client.  Internally
    // data is exchanged using the websocket protocol, but we can ignore
    // that -- we'll only have to deal with payload data, as if it was
    // exchanged over a raw TCP connection.

    if (!websocket.connect("ws.vi-server.org", 80)) {
        // connection failed
        return;
    } // else: we're connected 

    // say hello
    websocket.println("Hello from PicoWebsocket!");

    // wait for reply
    while (websocket.connected() && !websocket.available()) {
        yield();
    }

    // read reply to buffer
    uint8_t buffer[128];
    const auto bytes_read = websocket.read(buffer, 128);
    Serial.write(buffer, bytes_read);
}
```

### Server

```
#include <Arduino.h>
#include <ESP8266WiFi.h>        // use <WiFi.h> on the ESP32
#include <WiFiServer.h>

#include <PicoWebsocket.h>

// Define underlying server.  Any class that has a begin() and an accept()
// method will can be used here (e.g. EthernetServer)
WiFiServer server(80);

// Define a websocket server, which wraps server
PicoWebsocket::Server<WiFiServer> websocket_server(server);

void setup() {
    Serial.begin(115200);
    WiFi.begin("network", "secret");
    websocket_server.begin();
}

void loop() {
    // Now we can use websocket server just like we would use server.  Client
    // connections will be transparently upgraded to a websocket connection.
    // We'll only have to deal with payload data, as if it was exchanged over
    // a raw TCP connection.

    auto websocket = websocket_server.accept();
    if (!websocket) {
        // no client
        return;
    }

    // wait for a message
    while (websocket.connected() && !websocket.available()) {
        yield();
    }

    // read the message to a buffer and print it
    uint8_t buffer[128];
    const auto bytes_read = websocket.read(buffer, 128);
    Serial.write(buffer, bytes_read);

    // echo the message
    websocket.write(buffer, bytes_read);
}
```

## Related projects

PicoWebsockets is used by the [PicoMQTT](https://github.com/mlesniew/PicoMQTT) library to implement MQTT over websockets.

## License

This library is open-source software licensed under GNU LGPLv3.
