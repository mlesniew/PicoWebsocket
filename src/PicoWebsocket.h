#pragma once

#include <Arduino.h>
#include <Client.h>

#ifndef PICOWEBSOCKET_MAX_HTTP_LINE_LENGTH
#define PICOWEBSOCKET_MAX_HTTP_LINE_LENGTH 128
#endif

namespace PicoWebsocket {

class ClientBase: public ::Client {
    public:
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

        unsigned long socket_timeout_ms;

    protected:
        ClientBase(::Client & client, unsigned long socket_timeout_ms = 1000, bool is_client = true);

        virtual void on_pong(const void * data, const size_t size) {};

        enum Opcode : uint8_t {
            DATA_CONTINUATION = 0x0,
            DATA_TEXT = 0x1,
            DATA_BINARY = 0x2,
            CTRL_CLOSE = 0x8,
            CTRL_PING  = 0x9,
            CTRL_PONG  = 0xa,
            ERR = 0xff,
        };

        // HTTP related methods
        virtual void on_http_line_too_long() = 0;
        virtual void on_http_timeout() = 0;
        virtual void on_http_violation() = 0;
        String read_http_line(const unsigned long timeout_ms);
        std::pair<String, String> read_http_header();

        void discard_incoming_data();
        void on_violation();

        bool await_data_frame();

        void write_head(Opcode opcode, bool fin, size_t payload_length);
        Opcode read_head();

        size_t write_frame(Opcode opcode, bool fin, const void * payload, size_t size);

        size_t read_payload(void * buffer, const size_t size, const bool all = false);
        size_t write_payload(const void * payload, const size_t size);

        size_t read_all(const void * buffer, const size_t size, const unsigned long timeout_ms);
        size_t write_all(const void * buffer, const size_t size);

        void close(const uint16_t code = 0);
        void stop(uint16_t code);

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

class Client: public ClientBase {
    public:
        Client(::Client & client, const String & path = "/", const String & protocol = "", unsigned long socket_timeout_ms = 1000): 
            ClientBase(client, socket_timeout_ms, true), path(path), protocol(protocol) {}

        virtual int connect(IPAddress ip, uint16_t port) override;
        virtual int connect(const char * host, uint16_t port) override;

        String path;
        String protocol;

    protected:
        virtual void on_http_line_too_long() override;
        virtual void on_http_timeout() override;
        virtual void on_http_violation() override;
        void on_http_error();
        bool handshake(const String & host);
};

template <typename Socket>
class SocketOwner {
    public:
        SocketOwner(const Socket & socket): socket(socket) {}

    protected:
        Socket socket;
};

class ServerClient;

class ServerInterface {
public:
    ServerInterface(const String & protocol = "", unsigned long socket_timeout_ms = 1000): protocol(protocol), socket_timeout_ms(socket_timeout_ms) {}
    virtual ~ServerInterface() {}

    virtual bool check_url(const String & url) { return true; }
    virtual bool check_http_header(const String & header, const String & value) { return true; }

    virtual void on_pong(ServerClient & client, const void * data, const size_t size) {}

    String protocol;
    unsigned long socket_timeout_ms;

};

class ServerClient: public ClientBase {
    public:
        ServerClient(::Client & client, ServerInterface & server): ClientBase(client, server.socket_timeout_ms, false), server(server) {
        }

        virtual int connect(IPAddress ip, uint16_t port) override { return 0; }
        virtual int connect(const char * host, uint16_t port) override { return 0; }

    protected:
        virtual void on_http_line_too_long() override;
        virtual void on_http_timeout() override;
        virtual void on_http_violation() override;
        void on_http_error(const unsigned short code, const String & message);
        void handshake();

        void on_pong(const void * data, const size_t size) { server.on_pong(*this, data, size); }

        ServerInterface & server;
};

template <typename ServerSocket>
class Server: public ServerInterface {
    protected:
        ServerSocket & server;

    public:
        using ClientSocket = decltype(server.accept());

        class Client: public SocketOwner<ClientSocket>, public PicoWebsocket::ServerClient {
            public:
                Client(const ClientSocket & client, ServerInterface & server): SocketOwner<ClientSocket>(client), PicoWebsocket::ServerClient(this->socket, server) {
                    if (this->client.connected()) {
                        handshake();
                    }
                }

                Client(const Client & other): SocketOwner<ClientSocket>(other.socket), PicoWebsocket::ServerClient(this->socket, other.server) { }
        };

        Server(ServerSocket & server, const String & protocol = "", unsigned long socket_timeout_ms = 1000)
            : ServerInterface(protocol, socket_timeout_ms), server(server) { }

        Client accept() { return Client(server.accept(), *this); }
        void begin() { server.begin(); }
};

}
