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
            if (elapsed_ms >= socket_timeout_ms) {
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

Client::Client(::Client & client, String protocol, unsigned long socket_timeout_ms, bool is_client):
    protocol(protocol),
    socket_timeout_ms(socket_timeout_ms),
    client(client),
    is_client(is_client),
    mask(0),
    in_frame_size(0), in_frame_pos(0),
    write_continue(false),
    closing(false) {
}

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

void Client::close(const uint16_t code) {
    // NOTE: The optional 2-byte code can be followed by a message
    // for diagnostic purposes.  While it's easy to implement, there
    // is little value in supporting it and by skipping it we can
    // save a few bytes.
    PRINT_DEBUG("Sending close, code=%i\n", code);

    const uint16_t code_be = ntoh(code);
    closing = true;

    const size_t frame_length = code ? 2 : 0;
    write_frame(Opcode::CTRL_CLOSE, true, &code_be, frame_length);
}

void Client::stop() {
    close(1000);
    const unsigned long start_time = millis();
    while (client.connected() && (millis() - start_time <= socket_timeout_ms)) {
        if (!await_data_frame()) {
            continue;
        }

        // data frame received, discard it
        while ((in_frame_pos < in_frame_size) && (millis() - start_time <= socket_timeout_ms)) {
            // We could read more data at a time and improve performance,
            // but this code is rarely reached and can only run once per
            // connection, so the impact is minimal.
            uint8_t c;
            read_payload(&c, 1);
        }
    }
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
                uint8_t buf[in_frame_size];
                if (!read_payload(buf, in_frame_size, true)) {
                    // read failed, we're already disconnected
                    break;
                }

                const uint16_t code = (in_frame_size >= 2) ? (((uint16_t) buf[0] << 8) | (uint16_t) buf[1]) : 0;
                PRINT_DEBUG("Received close, code=%i\n", code);

                if (!closing) {
                    // WebSocket was not in closing state.  We're entering it now.
                    // We could send a close reply later, but we do it right away
                    // as we're not allowed to send any data frames from this point
                    // on anyway.
                    close(code);
                }

                // We were in closing state or have just entered it.
                // The connection can be closed now.
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

String Client::read_http(const unsigned long timeout_ms = 1000) {
    const unsigned long start_time = millis();

    bool ending = false;

    uint8_t buffer[PICOWEBSOCKET_MAX_HTTP_LINE_LENGTH + 1];
    size_t pos = 0;
    while (true) {

        if (pos >= PICOWEBSOCKET_MAX_HTTP_LINE_LENGTH) {
            // max line length reached
            on_http_error(414, F("HTTP line too long"));
            return "";
        }

        const int c = client.read();
        if (c < 0) {
            // no more data available
            if (millis() - start_time > timeout_ms) {
                // time out reached
                on_http_error(408, F("Request timeout"));
                return "";
            }

            yield();
            continue;
        }

        // got a valid char
        if (ending) {
            // we're waiting for the trailing \n, anything else is a protocol violation
            if (c != '\n') {
                PRINT_DEBUG("Invalid HTTP line ending\n");
                on_http_violation();
                return "";
            } else {
                buffer[pos] = '\0';
                PRINT_DEBUG("HTTP line received: %s\n", (const char *) buffer);
                return (const char *) buffer;
            }
        } else {
            if (c == '\r') {
                // end of line found, wait for the \n now
                ending = true;
            } else if (c < 0x20 || c == 0x7f) {
                // control character
                PRINT_DEBUG("Illegal HTTP line character\n");
                on_http_violation();
                return "";
            } else {
                buffer[pos++] = (uint8_t) c;
            }
        }
    }
}

void Client::discard_incoming_data() {
    PRINT_DEBUG("Discarding remaining received data\n");
    while (client.available()) {
        client.read();
    }
}

void Client::on_http_error(const unsigned short code, const String & message) {
    PRINT_DEBUG("HTTP protocol error %u %s\n", code, message.c_str());
    discard_incoming_data();
    client.printf(
        "%u %s\r\n"
        "Content-Length: 0\r\n\r\n",
        code, message.c_str());
    client.stop();
}

void Client::on_http_violation() {
    PRINT_DEBUG("HTTP protocol violation\n");
    on_http_error(400, F("Protocol Violation"));
}

void Client::on_violation() {
    PRINT_DEBUG("Websocket protocol violation\n");
    close(1002);
    // After a close frame we should wait for a close reply, but since we've
    // encountered a protocol violation, we give up the connection right away.
    discard_incoming_data();
    client.stop();
}

std::pair<String, String> Client::read_header() {
    String request = read_http(socket_timeout_ms);

    if (request == "") {
        return {"", ""};
    }

    const int colon_idx = request.indexOf(':');
    if (colon_idx < 0) {
        PRINT_DEBUG("Malformed HTTP header: colon missing\n");
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

    if (!read_all(head, 2, socket_timeout_ms)) {
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
        if (!read_all(&v, 2, socket_timeout_ms)) {
            PRINT_DEBUG("Error reading header (16-bit payload length)\n");
            return Opcode::ERR;
        }
        payload_length = ntoh(v);
    } else if (payload_length == 127) {
        uint64_t v;
        if (!read_all(&v, 8, socket_timeout_ms)) {
            PRINT_DEBUG("Error reading header (64-bit payload length)\n");
            return Opcode::ERR;
        }
        payload_length = ntoh(v);
    }

    if (has_mask) {
        // mask is stored in big endian
        if (!read_all(&mask, 4, socket_timeout_ms)) {
            PRINT_DEBUG("Error reading header (masking key)\n");
            return Opcode::ERR;
        }
    }

    in_frame_pos = 0;
    in_frame_size = payload_length;

    PRINT_DEBUG("Frame recv: opcode=%1x fin=%i mask=%08x len=%llu mask_key=%08x\n",
                opcode, fin, mask, payload_length, is_client ? 0 : mask);

    // Frame header is now received successfully, run a simple check
    // to see if it conforms to the RFC.
    if ((uint8_t)(opcode) & 0x8) {
        // this is a control frame
        if (!fin) {
            PRINT_DEBUG("Fragmented control frame received\n");
            on_violation();
            return Opcode::ERR;
        }
        if (payload_length >= 126) {
            PRINT_DEBUG("Control frame too long\n");
            on_violation();
            return Opcode::ERR;
        }
    }

    if (is_client == has_mask) {
        PRINT_DEBUG("Masking error\n");
        on_violation();
        return Opcode::ERR;
    }

    return opcode;
}


void Client::handshake_server() {
    // handle handshake
    const String request = read_http();
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
        on_http_error(505, F("HTTP Version Not Supported"));
        return;
    }

    if (method != "GET") {
        on_http_error(405, F("Method Not Allowed"));
        return;
    }

    // TODO: process the URL?

    // Process headers
    String sec_websocket_key;
    String sec_websocket_protocol;
    bool subprotocol_ok = (protocol.length() == 0);
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
            sec_websocket_key = header.second;
        } else if (header.first == "sec-websocket-protocol") {
            int start = 0;
            while (start < (int) header.second.length()) {
                const int space = header.second.indexOf(' ', start);
                const int end = (space < 0) ? header.second.length() : space;
                if (end > start) {
                    sec_websocket_protocol = header.second.substring(start, end);
                    subprotocol_ok = subprotocol_ok || (sec_websocket_protocol == protocol);
                }
                start = end + 1;
            }
        }
    }

    if (error || sec_websocket_key == "" || !connection_upgrade || !upgrade_websocket || !subprotocol_ok) {
        on_http_error(400, F("Bad request"));
        return;
    }

    // All looks good, accept connection upgrade
    client.printf(
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n",
        calc_key(sec_websocket_key).c_str());

    if (sec_websocket_protocol) {
        client.printf("Sec-WebSocket-Protocol: %s\r\n", sec_websocket_protocol.c_str());
    }

    client.print("\r\n");

    // The websocket connection is all set up now.
    PRINT_DEBUG("Handshake complete\n");
}

}
