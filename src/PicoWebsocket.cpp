#include <set>

#include <Arduino.h>
#include <Hash.h>
#include <base64.h>

#include "PicoWebsocket.h"

#define PRINT_DEBUG(...) Serial.printf("DBG " __VA_ARGS__)

namespace {

uint16_t ntoh(uint16_t v) {
    return ((v & 0xff00) >> 8)
           | ((v & 0x00ff) << 8);
}

uint32_t ntoh(uint32_t v) {
    return ((v & 0xff000000) >> 24)
           | ((v & 0x00ff0000) >> 8)
           | ((v & 0x0000ff00) << 8)
           | ((v & 0x000000ff) << 24);
}

uint64_t ntoh(uint64_t v) {
    return ((v & 0xff00000000000000) >> 56)
           | ((v & 0x00ff000000000000) >> 40)
           | ((v & 0x0000ff0000000000) >> 24)
           | ((v & 0x000000ff00000000) >> 8)
           | ((v & 0x00000000ff000000) << 8)
           | ((v & 0x0000000000ff0000) << 24)
           | ((v & 0x000000000000ff00) << 40)
           | ((v & 0x00000000000000ff) << 56);
}

void apply_mask(void * data, uint32_t mask, size_t size, size_t offset = 0) {
    // TODO: Optimize:  Instead of applying the mask byte by byte, we can
    // go word by word (4 bytes at a time).  This should give a theoretical
    // quadriple boost.  However two aspects have to be considered:
    //   * size may not be divisible by 4
    //   * word memory access on Espressif boards is only possible at
    //     aligned addresses
    // This means we have to go in three steps:
    //   * go byte by byte until an aligned address is reached
    //   * go word by word until the remaining size is less than 4 bytes
    //   * go byte by byte again until the end of the buffer
    uint8_t * c = (uint8_t *) data;
    uint8_t * m = (uint8_t *) &mask;
    for (size_t i = 0; i < size; ++i) {
        c[i] ^= m[((i + offset) & 3)];
    }
}

String calc_key(const String & challenge) {
    uint8_t hash[20];
    sha1(challenge + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", hash);
    return base64::encode(hash, 20);
}

}

namespace PicoWebsocket {

size_t Client::write_all(const void * buffer, const size_t size) {
    size_t bytes_written = 0;
    while (client.connected() && (bytes_written < size)) {
        bytes_written += client.write(((uint8_t *) buffer) + bytes_written, size - bytes_written);
    }
    return bytes_written == size ? size : 0;
}

size_t Client::read_all(const void * buffer, const size_t size, const unsigned long timeout_ms) {
    size_t bytes_read = 0;
    const unsigned long start_time = millis();

    while (bytes_read < size) {
        while (!client.available()) {
            if (!client.connected()) {
                // connection lost already
                return 0;
            }
            // connection intact, but no data yet -- timout exceeded?
            const unsigned long elapsed_ms = millis() - start_time;
            if (elapsed_ms >= timeout_ms) {
                // timeout, drop connection
                client.stop();
                return 0;
            }
            // wait a little more
            yield();
        }

        // there's some data waiting in buffers to be read
        bytes_read += client.read(((uint8_t *) buffer) + bytes_read, size - bytes_read);
    }

    return size;
}

Client::Client(::Client & client, bool is_client):
    client(client),
    is_client(is_client),
    mask(0),
    in_frame_size(0), in_frame_pos(0),
    write_continue(false)
{}

size_t Client::read_payload(void * buffer, const size_t size, const bool all) {
    const size_t bytes_read = all ? read_all(buffer, size) : client.read((uint8_t *) buffer, size);

    if (!is_client) {
        // we're the server, the received data is masked
        apply_mask(buffer, mask, bytes_read, in_frame_pos);
    }

    in_frame_pos += bytes_read;

    return bytes_read;
}

size_t Client::write_payload(const void * payload, const size_t size) {
    if (is_client) {
        // TODO: Is there a more clever way for masking outgoing data?
        const size_t buffer_size = size < 128 ? size : 128;
        size_t written = 0;
        uint8_t buffer[buffer_size];
        while (written < size) {
            const size_t chunk_size = size <= buffer_size ? size : buffer_size;
            memcpy(buffer, ((const char *) payload) + written, chunk_size);
            apply_mask(buffer, mask, chunk_size, written);
            if (!write_all(buffer, chunk_size)) {
                break;
            }
            written += chunk_size;
        }
        return written;
    } else {
        return write_all(payload, size);
    }
}

size_t Client::write_frame(Opcode opcode, bool fin, const void * payload, size_t size) {
    write_head(opcode, fin, size);
    return write_payload(payload, size);
}

void Client::pong(const void * payload, size_t size) {
    write_frame(Opcode::CTRL_PONG, true, payload, size);
}

void Client::ping(const void * payload, size_t size) {
    write_frame(Opcode::CTRL_PING, true, payload, size);
}

void Client::close(uint16_t reason) {
    // TODO: Add support for a text description
    reason = ntoh(reason);
    write_frame(Opcode::CTRL_CLOSE, true, &reason, reason ? 2 : 0);
}

size_t Client::write(const void * buffer, size_t size, bool fin, bool bin) {
    const Opcode opcode = write_continue ? Opcode::DATA_CONTINUATION : (bin ? Opcode::DATA_BINARY : Opcode::DATA_TEXT);
    write_continue = !fin;
    return write_frame(opcode, fin, buffer, size);
}

bool Client::await_data_frame() {
    while (client.available()) {
        const Opcode opcode = read_head();

        switch (opcode) {
            case Opcode::DATA_CONTINUATION:
            case Opcode::DATA_TEXT:
            case Opcode::DATA_BINARY: {
                // TODO: Handle state transitions
                if (in_frame_size) {
                    // the new frame is non-empty
                    return true;
                }

                // empty data frame received
                break;
            }

            case Opcode::CTRL_CLOSE: {
                // TODO: Handle close
                char buf[in_frame_size + 1];
                if (!read_payload(buf, in_frame_size, true)) {
                    // read failed, we're already disconnected
                    break;
                }
                buf[in_frame_size] = '\0';
                Serial.printf("Close: %i - %s\n", ntoh(*((uint16_t *) buf)), buf + 2);
                close(1000);
                client.stop();
                break;
            }

            case Opcode::CTRL_PING:
            case Opcode::CTRL_PONG: {
                char buf[in_frame_size];
                if (!read_payload(buf, in_frame_size, true)) {
                    // read failed, we're already disconnected
                    break;
                }

                if (opcode == Opcode::CTRL_PING) {
                    pong(buf, in_frame_size);
                } else {
                    // TODO: Handle pong...
                }
                break;
            }

            default: {
                close(1002);    // protocol violation
                on_violation();
                break;
            }
        }
    }
    return false;
}

int Client::available() {
    size_t frame_remain = in_frame_size - in_frame_pos;

    if (!frame_remain) {
        // no data left in current frame, let's see if another frame is available
        if (!await_data_frame()) {
            // no data frame received, give up
            return 0;
        }
        // else: data frame received
        frame_remain = in_frame_size - in_frame_pos;
    }

    // We've started reading a data frame.  We're expecting at least `frame_remain` of payload
    // data to arrive.  The underlying socket may have more data available for reading, but before consuming the
    // next header(s), we can't be sure how much of the remaining data is the payload.  We never return
    // more than `frame_remain`, because that's the only amount of byte's that is guaranteed
    // to be payload data.
    const size_t socket_available = client.available();
    return frame_remain < socket_available ? frame_remain : socket_available;
}

int Client::read(uint8_t * buffer, size_t size) {
    // TODO: Read data from multiple frames if available
    if (in_frame_pos >= in_frame_size) {
        if (!await_data_frame()) {
            return 0;
        }
    }

    const size_t frame_remain = in_frame_size - in_frame_pos;
    const size_t read_size = frame_remain < size ? frame_remain : size;
    return read_payload(buffer, read_size);
}

int Client::peek() {
    if (!available()) {
        // no payload data waiting in buffer
        return -1;
    }

    // at this point we're guaranteed there's data waiting on the client and that the next byte is payload data

    uint8_t c = (uint8_t) client.peek();

    if (!is_client) {
        // we're the server, apply the mask
        uint8_t * m = (uint8_t *) &mask;
        c ^= m[in_frame_pos & 3];
    }

    return c;
}

String Client::read_http() {
    // TODO: readStringUntil has a timeout, but it's measured separately for each received byte -- replace it.
    client.setTimeout(1000);
    String line = client.readStringUntil('\r');
    // TODO: check for data in between
    client.readStringUntil('\n');
    PRINT_DEBUG("HTTP line received: %s\n", line.c_str());
    return line;
}

void Client::on_http_error() {
    PRINT_DEBUG("HTTP protocol error\n");
    client.stop();
}

void Client::on_http_violation() {
    PRINT_DEBUG("HTTP protocol violation\n");
    client.stop();
}

void Client::on_violation() {
    PRINT_DEBUG("Websocket protocol violation\n");
    client.stop();
}

std::pair<String, String> Client::read_header() {
    String request = read_http();

    if (request == "") {
        return {"", ""};
    }

    const int colon_idx = request.indexOf(':');
    if (colon_idx < 0) {
        PRINT_DEBUG("Malformed header -- no colon\n");
        on_http_violation();
        return {"", ""};
    }

    String name = request.substring(0, colon_idx);
    String value = request.substring(colon_idx + 1);

    // convert name to lower case
    name.toLowerCase();

    // remove spaces around value
    value.trim();

    PRINT_DEBUG("HTTP header received: %s: %s\n", name.c_str(), value.c_str());
    return std::make_pair(name, value);
}

void Client::write_head(Opcode opcode, bool fin, size_t payload_length) {
    PRINT_DEBUG("OUT: fin=%i opcode=%1x len=%u\n",
                fin, opcode, payload_length);

    uint8_t buffer[14];
    uint8_t * pos = buffer;

    *pos++ = (opcode & 0x0f) | (fin ? 1 << 7 : 0);

    const uint8_t mask_bit = (is_client ? 1 << 7 : 0);

    if (payload_length <= 125) {
        *pos++ = uint8_t(payload_length) | mask_bit;
    } else if (payload_length <= 0xffff) {
        *pos++ = 126 | mask_bit;
        *pos++ = (payload_length >> 8) & 0xff;
        *pos++ = (payload_length >> 0) & 0xff;
    } else {
        *pos++ = 127 | mask_bit;
        // size_t is 4 bytes only, so the 4 most significant bytes will always be zero
        *(uint32_t *)(pos) = 0;
        pos += 4;
        *pos++ = (payload_length >> 24) & 0xff;
        *pos++ = (payload_length >> 16) & 0xff;
        *pos++ = (payload_length >> 8) & 0xff;
        *pos++ = (payload_length >> 0) & 0xff;
    }

    if (is_client) {
        mask = (uint32_t) random();
        // write mask as is, don't convert since it's already in big endian
        memcpy(pos, &mask, 4);
        pos += 4;
    }

    write_all(buffer, pos - buffer);
}

Client::Opcode Client::read_head() {
    uint8_t head[2];

    if (!read_all(head, 2)) {
        Serial.println("Short read reading head (2).");
        return Opcode::ERR;
    }

    // TODO: Use less reads to get the full frame header

    const bool fin = head[0] & (1 << 7);
    const Opcode opcode = static_cast<Opcode>(head[0] & 0xf);

    const bool has_mask = head[1] & (1 << 7);
    uint64_t payload_length = head[1] & 0x7f;

    if (payload_length == 126) {
        uint16_t v;
        if (!read_all(&v, 2)) {
            PRINT_DEBUG("Error reading header (16-bit payload length)\n");
            return Opcode::ERR;
        }
        payload_length = ntoh(v);
    } else if (payload_length == 127) {
        uint64_t v;
        if (!read_all(&v, 8)) {
            PRINT_DEBUG("Error reading header (64-bit payload length)\n");
            return Opcode::ERR;
        }
        payload_length = ntoh(v);
    }

    if (has_mask) {
        // mask is stored in big endian
        if (!read_all(&mask, 4)) {
            PRINT_DEBUG("Error reading header (masking key)\n");
            return Opcode::ERR;
        }
    }

    // TODO: Check frame for protocol correctness

    in_frame_pos = 0;
    in_frame_size = payload_length;

    PRINT_DEBUG("Websocket Frame: fin=%i opcode=%1x mask=%08x len=%llu mask_key=%08x\n",
                fin, opcode, mask, payload_length, is_client ? 0 : mask);

    return opcode;
}


void Client::handshake_server() {
    // handle handshake
    const String request = read_http();
    PRINT_DEBUG("HTTP recv: %s\n", request.c_str());

    if (request == "") {
        on_http_violation();
        return;
    }

    // GET /websocket/url HTTP/1.1
    const int url_start = request.indexOf(' ');
    const int url_end = request.indexOf(' ', url_start + 1);

    if (url_start < 0 || url_end < 0) {
        PRINT_DEBUG("Malformed HTTP request: %s\n", request.c_str());
        on_http_violation();
        return;
    }

    const String method = request.substring(0, url_start);
    const String url = request.substring(url_start + 1, url_end);
    const String version = request.substring(url_end + 1);

    if (version != "HTTP/1.1") {
        client.print(
            "HTTP/1.1 505 HTTP Version Not Supported\r\n"
            "Content-Length: 0\r\n"
        );
        on_http_error();
        return;
    }

    if (method != "GET") {
        client.print(
            "HTTP/1.1 405 Method Not Allowed\r\n"
            "Content-Length: 0\r\n"
        );
        on_http_error();
        return;
    }

    // TODO: process the URL?

    // Process headers
    String key;
    // TODO: Don't collect all protocols.  Instead, allow the user to specify what is expected and only accept that
    // or, if nothing is specified, accept any protocol suggested by client.
    std::set<String> protocols;
    bool connection_upgrade = false;
    bool upgrade_websocket = false;
    bool error = false;
    // TODO: check Origin header if present
    // TODO: check Host header

    while (!error) {
        auto header = read_header();
        if (header.first == "") {
            PRINT_DEBUG("End of headers reached");
            break;
        } else if (header.first == "connection") {
            header.second.toLowerCase();
            if (header.second == "upgrade") {
                connection_upgrade = true;
            } else {
                error = true;
                PRINT_DEBUG("HTTP Header Connection has incorrect value\n");
            }
        } else if (header.first == "upgrade") {
            header.second.toLowerCase();
            if (header.second == "websocket") {
                upgrade_websocket = true;
            } else {
                error = true;
                PRINT_DEBUG("HTTP Header Upgrade has incorrect value\n");
            }
        } else if (header.first == "sec-websocket-key") {
            key = header.second;
        } else if (header.first == "sec-websocket-protocol") {
            const String & values = header.second;
            int start = 0;

            while (start < values.length()) {
                const int space = values.indexOf(' ', start);
                const int end = (space < 0) ? values.length() : space;

                if (end > start) {
                    protocols.insert(values.substring(start, end));
                }
                start = end + 1;
            }
        }
    }

    if (error || key == "" || !connection_upgrade || !upgrade_websocket || protocols.empty()) {
        client.print(
            "HTTP/1.1 400 Bad request\r\n"
            "Content-Length: 0\r\n"
        );

        on_http_error();
    }

    // All looks good, accept connection upgrade
    client.printf(
        (
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: %s\r\n"
            "Sec-WebSocket-Protocol: %s\r\n\r\n"
        ),
        calc_key(key).c_str(), protocols.begin()->c_str());

    // The websocket connection is all set up now.
    PRINT_DEBUG("Handshake complete\n");
}

}
