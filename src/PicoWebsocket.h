#pragma once

#include <Arduino.h>
#include <Client.h>

#include <WiFiClient.h>
#include <WiFiServer.h>

#ifndef PICOWEBSOCKET_MAX_HTTP_LINE_LENGTH
#define PICOWEBSOCKET_MAX_HTTP_LINE_LENGTH 128
#endif

namespace PicoWebsocket {

class Client: public ::Client {
    public:
        Client(::Client & client, String protocol = "", bool is_client = true);

        virtual int connect(IPAddress ip, uint16_t port) override {
            // TODO: Implement
            return 0;
        }

        virtual int connect(const char * host, uint16_t port) override {
            // TODO: Implement
            return 0;
        }

        size_t write(const void * buffer, size_t size, bool fin, bool bin = true);
        virtual size_t write(const uint8_t * buffer, size_t size) override { return write(buffer, size, true, true); }
        virtual size_t write(uint8_t c) override { return write(&c, 1); }

        virtual int available() override;
        virtual int read(uint8_t * buffer, size_t size) override;
        virtual int read() override {
            uint8_t c;
            return (read(&c, 1)) ? c : -1;
        }
        virtual int peek() override;

        virtual void flush() override { client.flush(); }
        virtual void stop() override;

        virtual uint8_t connected() override {
            return client.connected();
        }

        virtual operator bool() { return bool(client); }

        void ping(const void * payload = nullptr, size_t size = 0);
        void pong(const void * payload = nullptr, size_t size = 0);
        void close(const uint16_t code = 0);

        void handshake_server();

        String protocol;
    protected:

        enum Opcode : uint8_t {
            DATA_CONTINUATION = 0x0,
            DATA_TEXT = 0x1,
            DATA_BINARY = 0x2,
            CTRL_CLOSE = 0x8,
            CTRL_PING  = 0x9,
            CTRL_PONG  = 0xa,
            ERR = 0xff,
        };

        String read_http(const unsigned long timeout_ms);

        void discard_incoming_data();
        void on_http_error(const unsigned short code, const String & message);
        void on_http_violation();
        void on_violation();

        std::pair<String, String> read_header();
        bool await_data_frame();

        void write_head(Opcode opcode, bool fin, size_t payload_length);
        Opcode read_head();

        size_t write_frame(Opcode opcode, bool fin, const void * payload, size_t size);

        size_t read_payload(void * buffer, const size_t size, const bool all = false);
        size_t write_payload(const void * payload, const size_t size);

        // TODO: Make timeout_ms configurable from public API
        size_t read_all(const void * buffer, const size_t size, const unsigned long timeout_ms = 1000);
        size_t write_all(const void * buffer, const size_t size);

        ::Client & client;
        const bool is_client;

        // NOTE: The mask is stored in big endian -- this simplifies the masking and unmasking operations a bit.
        uint32_t mask;

        size_t in_frame_size;
        size_t in_frame_pos;

        // state
        bool write_continue;
        bool closing;
};

template <typename Socket>
class SocketOwner {
    public:
        SocketOwner(const Socket & socket): socket(socket) {}

    protected:
        Socket socket;
};

template <typename ServerSocket>
class Server {
    protected:
        ServerSocket & server;

    public:
        using ClientSocket = decltype(server.accept());

        class Client: public SocketOwner<ClientSocket>, public PicoWebsocket::Client {
            public:
                Client(const ClientSocket & client, String protocol): SocketOwner<ClientSocket>(client),
                    PicoWebsocket::Client(this->socket, protocol, false) {
                    if (this->client.connected()) {
                        handshake_server();
                    }
                }

                Client(const Client & other): SocketOwner<ClientSocket>(other.socket), PicoWebsocket::Client(this->socket,
                            other.protocol, false) { }
        };

        Server(ServerSocket & server, String protocol = ""): server(server), protocol(protocol) { }

        Client accept() { return Client(server.accept(), protocol); }
        void begin() { server.begin(); }

        String protocol;
};

}
