/**
 * @file PubSubClient.cpp
 * @brief A simple client for MQTT.
 * @author Nicholas O'Leary - http://knolleary.net
 * @author Holger Mueller - https://github.com/hmueller01/pubsubclient3
 * @copyright MIT License 2008-2026
 *
 * This file is part of the PubSubClient library.
 */

#include "PubSubClient.h"

/**
 * @brief Give other tasks a chance to run while waiting for data.
 *
 * On FreeRTOS a yield only reschedules a task of equal or higher priority, which is
 * what a real time scheduler has to promise. A task spinning in a wait loop therefore
 * never lets the idle task run, and a watchdog fed from that idle task fires. Blocking
 * for a single tick is what actually hands the CPU over.
 *
 * vTaskDelay(1) rather than delay(1): the intent is one tick, and delay(ms) maps to
 * vTaskDelay(ms / portTICK_PERIOD_MS), which rounds down to zero below a 1 kHz tick.
 */
#if defined(ARDUINO_ARCH_ESP32) || defined(ESP32)
#define PUBSUB_WAIT_YIELD() vTaskDelay(1)
#else
#define PUBSUB_WAIT_YIELD() yield()
#endif

/**
 * @brief Minimum interval, in ms, between cooperative yields while actively draining bytes in readPacket().
 *
 * readByte() only yields while waiting for data (socket empty). When a large packet is already buffered in
 * the network stack, available() stays > 0 and the byte loop would run for thousands of iterations without
 * ever handing the CPU over, starving the idle/other tasks and tripping the watchdog. This bounds the time
 * between yields without throttling throughput (a single tick yield every ~50 ms is negligible).
 */
#ifndef PUBSUB_YIELD_INTERVAL_MS
#define PUBSUB_YIELD_INTERVAL_MS 50UL
#endif

/**
 * @brief Macro to check if a string 's' can be safely added to the MQTT _buffer.
 *
 * If either check fails, the client connection is stopped and the function returns false.
 * @param l current length in the _buffer
 * @param s string to check
 */
#define CHECK_STRING_LENGTH(l, s)                                                   \
    if (!(s) || ((l) > _bufferSize) || (_bufferSize - (l) < 2) ||                    \
        (strnlen((s), _bufferSize) > 0xFFFF) ||                                      \
        (strnlen((s), _bufferSize) > _bufferSize - (l) - 2)) {                        \
        closeConnection(MQTT_CONNECT_FAILED);                                      \
        return false;                                                             \
    }

PubSubClient::PubSubClient() {
    setBufferSize(MQTT_MAX_PACKET_SIZE);
    setKeepAlive(MQTT_KEEPALIVE);
    setSocketTimeout(MQTT_SOCKET_TIMEOUT);
}

PubSubClient::PubSubClient(Client& client) : PubSubClient() {
    setClient(client);
}

PubSubClient::PubSubClient(IPAddress addr, uint16_t port, Client& client) : PubSubClient() {
    setServer(addr, port);
    setClient(client);
}

PubSubClient::PubSubClient(IPAddress addr, uint16_t port, Client& client, Stream& stream) : PubSubClient() {
    setServer(addr, port);
    setClient(client);
    setStream(stream);
}

PubSubClient::PubSubClient(IPAddress addr, uint16_t port, MQTT_CALLBACK_SIGNATURE, Client& client) : PubSubClient() {
    setServer(addr, port);
    setCallback(callback);
    setClient(client);
}

PubSubClient::PubSubClient(IPAddress addr, uint16_t port, MQTT_CALLBACK_SIGNATURE, Client& client, Stream& stream) : PubSubClient() {
    setServer(addr, port);
    setCallback(callback);
    setClient(client);
    setStream(stream);
}

PubSubClient::PubSubClient(uint8_t* ip, uint16_t port, Client& client) : PubSubClient() {
    setServer(ip, port);
    setClient(client);
}

PubSubClient::PubSubClient(uint8_t* ip, uint16_t port, Client& client, Stream& stream) : PubSubClient() {
    setServer(ip, port);
    setClient(client);
    setStream(stream);
}

PubSubClient::PubSubClient(uint8_t* ip, uint16_t port, MQTT_CALLBACK_SIGNATURE, Client& client) : PubSubClient() {
    setServer(ip, port);
    setCallback(callback);
    setClient(client);
}

PubSubClient::PubSubClient(uint8_t* ip, uint16_t port, MQTT_CALLBACK_SIGNATURE, Client& client, Stream& stream) : PubSubClient() {
    setServer(ip, port);
    setCallback(callback);
    setClient(client);
    setStream(stream);
}

PubSubClient::PubSubClient(const char* domain, uint16_t port, Client& client) : PubSubClient() {
    setServer(domain, port);
    setClient(client);
}

PubSubClient::PubSubClient(const char* domain, uint16_t port, Client& client, Stream& stream) : PubSubClient() {
    setServer(domain, port);
    setClient(client);
    setStream(stream);
}

PubSubClient::PubSubClient(const char* domain, uint16_t port, MQTT_CALLBACK_SIGNATURE, Client& client) : PubSubClient() {
    setServer(domain, port);
    setCallback(callback);
    setClient(client);
}

PubSubClient::PubSubClient(const char* domain, uint16_t port, MQTT_CALLBACK_SIGNATURE, Client& client, Stream& stream) : PubSubClient() {
    setServer(domain, port);
    setCallback(callback);
    setClient(client);
    setStream(stream);
}

PubSubClient::~PubSubClient() {
    free(_domain);
    free(_buffer);
#if MQTT_ENABLE_INCREMENTAL_RX
    free(_rxBuffer);
#endif
    free(_qos2ClientId);
}

void PubSubClient::resetReceiveState() {
#if MQTT_ENABLE_INCREMENTAL_RX
    _rxState = RX_IDLE;
    _rxHdrLen = 0;
    _rxMultiplier = 0;
    _rxLen = _rxLength = _rxRemaining = _rxStreamSkip = 0;
#endif
    _packetReadStart = 0;
    // disconnect() is allowed inside the callback; keep its lifetime guard armed.
    if (!(_rxFlags & MQTT_RX_BUSY)) _rxFlags = 0;
}

void PubSubClient::closeConnection(int reason) {
    _state = reason;
    if (_client) _client->stop();
    _pingOutstanding = false;
    _txActive = false;
    _txRemaining = _bufferWritePos = 0;
    resetReceiveState();
}

void PubSubClient::clearQos2Session() {
    memset(_rxQos2Ids, 0, sizeof(_rxQos2Ids));
    free(_qos2ClientId);
    _qos2ClientId = nullptr;
}

bool PubSubClient::connect(const char* id, const char* user, const char* pass, const char* willTopic, uint8_t willQos, bool willRetain,
                           const char* willMessage, bool cleanSession) {
    if (_rxFlags & MQTT_RX_BUSY) return false;
    if (_txActive && connected()) return false;
    if (!_client) return false;  // do not crash if client not set
    if (!_buffer || !id) return false;
    if (!connected()) {
        resetReceiveState();
        _txActive = false;
        _txRemaining = _bufferWritePos = 0;
        if (cleanSession) {
            clearQos2Session();
        } else if (!_qos2ClientId || strcmp(_qos2ClientId, id) != 0) {
            clearQos2Session();
            const size_t idLen = strlen(id);
            if (idLen > 0xFFFF) return false;
            _qos2ClientId = (char*)malloc(idLen + 1);
            if (!_qos2ClientId) {
                _state = MQTT_CONNECT_FAILED;
                return false;
            }
            memcpy(_qos2ClientId, id, idLen + 1);
        }
        int result = 0;

        if (_client->connected()) {
            result = 1;
        } else if (_port != 0) {
            if (_domain) {
                result = _client->connect(_domain, _port);
            } else {
                result = _client->connect(_ip, _port);
            }
        }

        if (result == 1) {
            _nextMsgId = 1;       // init msgId (packet identifier)
            _bufferWritePos = 0;  // discard pending outgoing data of a previous (dead) session, see appendBuffer()

#if MQTT_VERSION == MQTT_VERSION_3_1
            const uint8_t protocol[9] = {0x00, 0x06, 'M', 'Q', 'I', 's', 'd', 'p', MQTT_VERSION};
#elif MQTT_VERSION == MQTT_VERSION_3_1_1
            const uint8_t protocol[7] = {0x00, 0x04, 'M', 'Q', 'T', 'T', MQTT_VERSION};
#endif
            // Leave room in the _buffer for header and variable length field
            memcpy(_buffer + MQTT_MAX_HEADER_SIZE, protocol, sizeof(protocol));

            size_t length = MQTT_MAX_HEADER_SIZE + sizeof(protocol);
            uint8_t flags = 0x00;
            if (willTopic) {
                flags = (0x01 << 2) | (willQos << 3) | (willRetain << 5);  // set will flag bit 2, will QoS and will retain bit 5
            }
            if (cleanSession) {
                flags = flags | (0x01 << 1);  // set clean session bit 1
            }
            if (user) {
                flags = flags | (0x01 << 7);  // set user name flag bit 7
                if (pass) {
                    flags = flags | (0x01 << 6);  // set password flag bit 6
                }
            }
            const uint16_t keepAlive = _keepAliveMillis / 1000;
            _buffer[length++] = flags;
            _buffer[length++] = keepAlive >> 8;
            _buffer[length++] = keepAlive & 0xFF;

            CHECK_STRING_LENGTH(length, id)
            length = writeString(id, length);
            if (willTopic) {
                CHECK_STRING_LENGTH(length, willTopic)
                length = writeString(willTopic, length);
                CHECK_STRING_LENGTH(length, willMessage)
                length = writeString(willMessage, length);
            }

            if (user) {
                CHECK_STRING_LENGTH(length, user)
                length = writeString(user, length);
                if (pass) {
                    CHECK_STRING_LENGTH(length, pass)
                    length = writeString(pass, length);
                }
            }

            // A new attempt must not inherit WRITE_INCOMPLETE from the previous socket.
            _state = MQTT_DISCONNECTED;
            if (!writeControlPacket(MQTTCONNECT, length - MQTT_MAX_HEADER_SIZE)) {
                if (_state != MQTT_WRITE_INCOMPLETE) closeConnection(MQTT_CONNECT_FAILED);
                return false;
            }
            _lastInActivity = _lastOutActivity = millis();
            _pingOutstanding = false;

            while (!_client->available()) {
                PUBSUB_WAIT_YIELD();
                if (!_client->connected()) {
                    // fail fast instead of waiting for the socket timeout
                    DEBUG_PSC_PRINTF("connect aborting due to lost connection\n");
                    closeConnection(MQTT_CONNECTION_LOST);
                    return false;
                }
                unsigned long t = millis();
                if (t - _lastInActivity >= _socketTimeoutMillis) {
                    DEBUG_PSC_PRINTF("connect aborting due to timeout\n");
                    closeConnection(MQTT_CONNECTION_TIMEOUT);
                    return false;
                }
            }
            uint8_t hdrLen;
            size_t len = readPacket(&hdrLen);

            if ((len == 4) && ((_buffer[0] & 0xF0) == MQTTCONNACK)) {
                if (_buffer[3] == 0) {
#if MQTT_VERSION == MQTT_VERSION_3_1_1
                    if (cleanSession || !(_buffer[2] & 1)) memset(_rxQos2Ids, 0, sizeof(_rxQos2Ids));
#else
                    // MQTT 3.1 cannot confirm whether the previous broker session survived.
                    memset(_rxQos2Ids, 0, sizeof(_rxQos2Ids));
#endif
                    _lastInActivity = millis();
                    _state = MQTT_CONNECTED;
                    return true;
                } else {
                    _state = _buffer[3];
                }
            } else if (_state == MQTT_DISCONNECTED) {
                // readPacket may already have closed with a more precise timeout/size/read reason.
                ERROR_PSC_PRINTF_P("connect: expected four-byte CONNACK (received=%zu)\n", len);
                closeConnection(MQTT_MALFORMED_PACKET);
                return false;
            }
            // Preserve a broker CONNACK rejection; receive failures already closed their socket.
            if (_state > 0) closeConnection(_state);
        } else {
            _state = MQTT_CONNECT_FAILED;
        }
        return false;
    }
    return true;
}

bool PubSubClient::connected() {
    if (!_client) return false;
    if (!_buffer) return false;  // we can't be connected if we don't have a buffer to read into

    if (_client->connected()) {
        return (_state == MQTT_CONNECTED);
    } else if (_state == MQTT_CONNECTED) {
        DEBUG_PSC_PRINTF("lost connection (client may have more details)\n");
        closeConnection(MQTT_CONNECTION_LOST);
    }
    return false;
}

void PubSubClient::disconnect() {
    // A DISCONNECT in the middle of a PUBLISH would become part of its payload.
    if (_client && _buffer && !_txActive && _state == MQTT_CONNECTED) {
        const uint8_t packet[] = {MQTTDISCONNECT, 0};
        _client->write(packet, sizeof(packet));
        _client->flush();
        _lastInActivity = _lastOutActivity = millis();
    }
    closeConnection(_txActive ? MQTT_PUBLISH_INCOMPLETE : MQTT_DISCONNECTED);
}

/**
 * @brief  Reads a byte into result.
 *
 * @param  result Pointer to result buffer.
 * @return true if byte was read, false if socketTimeout occurred or _client was not set.
 */
bool PubSubClient::readByte(uint8_t* result) {
    if (!_client) return false;  // do not crash if client not set

    unsigned long previousMillis = millis();
    while (!_client->available()) {
        PUBSUB_WAIT_YIELD();
        unsigned long currentMillis = millis();
        if (currentMillis - previousMillis >= _socketTimeoutMillis) {
            closeConnection(MQTT_PACKET_TIMEOUT);
            return false;  // parser state cannot be resumed after returning
        }
        // Overall packet deadline: a peer dripping one byte just below the per-byte timeout (slowloris)
        // would otherwise hold readPacket()/loop() for packetLen * socketTimeout. Bound the whole packet.
        // Independent of the per-byte socket timeout; 0 falls back to it (see setPacketTimeout()).
        const unsigned long packetTimeout = _packetTimeoutMillis ? _packetTimeoutMillis : _socketTimeoutMillis;
        if (currentMillis - _packetReadStart >= packetTimeout) {
            closeConnection(MQTT_PACKET_TIMEOUT);
            return false;
        }
    }
    int rc = _client->read();
    if (rc < 0) {
        closeConnection(MQTT_READ_FAILED);
        return false;
    }
    *result = (uint8_t)rc;
    return true;
}

/**
 * @brief  Reads a byte into result[*pos] and increments *pos.
 * Note: *pos may go out of bounds of result. This must be checked outside of this function!
 *
 * @return true if a byte was read, otherwise false (socketTimeout).
 */
bool PubSubClient::readByte(uint8_t* result, size_t* pos) {
    uint8_t* write_address = &(result[*pos]);
    if (readByte(write_address)) {
        (*pos)++;
        return true;
    }
    return false;
}

/**
 * @brief  Reads a complete packet (header, topic, payload) into _buffer.
 *
 * @param  *hdrLen Returns the variable header length send by MQTT broker (1 .. MQTT_MAX_HEADER_SIZE - 1)
 * @return Number of read bytes, 0 in case of an error (socketTimeout, buffer overflow, _client not set).
 */
size_t PubSubClient::readPacket(uint8_t* hdrLen) {
    size_t len = 0;
    _packetReadStart = millis();  // arm the overall packet read deadline (see readByte())
    if (!readByte(_buffer, &len)) return 0;
    bool isPublish = (_buffer[0] & 0xF0) == MQTTPUBLISH;
    bool discard = false;
    uint32_t multiplier = 1;
    size_t length = 0;
    uint8_t digit = 0;
    size_t skip = 0;  // size_t to avoid overflow of topicLen (up to 65535) + 2 for QoS > 0
    uint8_t start = 0;

    do {
        if (len == MQTT_MAX_HEADER_SIZE) {
            // Invalid remaining length encoding - kill the connection
            ERROR_PSC_PRINTF_P("readPacket: malformed remaining-length encoding\n");
            closeConnection(MQTT_MALFORMED_PACKET);
            return 0;
        }
        if (!readByte(&digit)) return 0;
        _buffer[len++] = digit;
        length += (digit & 0x7F) * multiplier;  // length is coded in the lower 7 bits
        multiplier <<= 7;                       // multiplier *= 128
    } while ((digit & 0x80) != 0);  // do while 8th continuation bit is set
    *hdrLen = (uint8_t)(len - 1);

    DEBUG_PSC_PRINTF("readPacket received packet of length %zu (isPublish = %u)\n", length, isPublish);

    // Oversized-packet handling (see setOversizedPolicy()). A length beyond MQTT_MAX_SANE_PACKET_SIZE is treated
    // as bogus (corruption/desync on a flaky link) and always disconnects, so the reconnect can resynchronise.
    {
        const size_t totalLen = (size_t)*hdrLen + 1 + length;  // type byte + remaining-length bytes + remaining length
        if (!_stream) {
            size_t budget = _bufferSize;
            if (_maxIncomingPacket && (_maxIncomingPacket < budget)) budget = _maxIncomingPacket;
            if (totalLen > budget) {
                if ((_oversizedPolicy == MQTT_OVERSIZED_IGNORE) && (totalLen <= MQTT_MAX_SANE_PACKET_SIZE)) {
                    discard = true;
                    // Drain-and-ignore: fall through. The read loop (bounded by the packet deadline and periodic
                    // yields) consumes all bytes and the discard check below drops the packet, keeping
                    // the stream in sync without blocking indefinitely or tripping the watchdog.
                    DEBUG_PSC_PRINTF("readPacket draining oversized packet (%zu > budget %zu)\n", totalLen, budget);
                } else {
                    // Fail-fast: close the connection so the reconnect resynchronises the stream.
                    ERROR_PSC_PRINTF_P("readPacket disconnecting on oversized packet (%zu > budget %zu)\n", totalLen, budget);
                    closeConnection(MQTT_PACKET_TOO_LARGE);
                    return 0;
                }
            }
        } else if (totalLen > MQTT_MAX_SANE_PACKET_SIZE) {
            // Stream mode: payload overflow is streamed out, so only a bogus (beyond-sane) length disconnects.
            ERROR_PSC_PRINTF_P("readPacket disconnecting on bogus streamed packet (%zu)\n", totalLen);
            closeConnection(MQTT_PACKET_TOO_LARGE);
            return 0;
        }
    }

    if (isPublish) {
        if (length < 2) {
            // A PUBLISH packet must at least contain the 2 topic length bytes. Reading them
            // unconditionally would consume bytes of the next packet and break framing.
            ERROR_PSC_PRINTF_P("readPacket: PUBLISH missing topic-length bytes (remaining=%zu)\n", length);
            closeConnection(MQTT_MALFORMED_PACKET);
            return 0;
        }
        // Read in topic length to calculate bytes to skip over for Stream writing
        if (!readByte(_buffer, &len)) return 0;
        if (!readByte(_buffer, &len)) return 0;
        skip = ((size_t)_buffer[*hdrLen + 1] << 8) + _buffer[*hdrLen + 2];
        start = 2;
        if (MQTT_HDR_GET_QOS(_buffer[0]) > MQTT_QOS0) {
            // skip msgId (packet identifier) for QoS 1 and 2 messages
            skip += 2;
        }
    }
    size_t idx = len;
    unsigned long lastYield = millis();

    for (size_t i = start; i < length; i++) {
        if (!readByte(&digit)) return 0;
        if (_stream) {
            if (isPublish && (idx - *hdrLen - 2 > skip)) {
                _stream->write(digit);
            }
        }

        if (len < _bufferSize) {
            _buffer[len++] = digit;
        }
        idx++;

        // Yield cooperatively on long/bulk reads where readByte() never waits (available() stays > 0),
        // otherwise this loop could run without feeding the watchdog. Time-based to avoid throttling throughput.
        const unsigned long now = millis();
        if (now - lastYield >= PUBSUB_YIELD_INTERVAL_MS) {
            PUBSUB_WAIT_YIELD();
            lastYield = now;
        }
    }

    if (!_stream && (discard || idx > _bufferSize)) {
        DEBUG_PSC_PRINTF("readPacket ignoring packet of size %zu exceeding buffer of size %zu\n", length, _bufferSize);
        len = 0;  // This will cause the packet to be ignored.
    }
    return len;
}

/**
 * @brief  After a packet is read handle the content here (call the callback, handle pings).
 *
 * @param  buf Buffer holding the received packet (the shared _buffer in blocking mode, or the dedicated _rxBuffer in non-blocking mode).
 * @param  bufSize Size of buf in bytes.
 * @param  hdrLen Variable header length send by MQTT broker (1 .. MQTT_MAX_HEADER_SIZE - 1).
 * @param  length Number of read bytes in buf.
 * @return true if packet was successfully processed, false if a buffer over or underflow occurred.
 */
bool PubSubClient::handlePacket(uint8_t* buf, size_t bufSize, uint8_t hdrLen, size_t length) {
    uint8_t type = buf[0] & 0xF0;
    DEBUG_PSC_PRINTF("handlePacket(): received message of type %u\n", type);
    if (length > bufSize) {
        // This should never happen as the receive path prevents buffer overflow, but we check again here to be sure and prevent any buffer overflows.
        ERROR_PSC_PRINTF_P("handlePacket: received=%zu exceeds buffer=%zu\n", length, bufSize);
        closeConnection(MQTT_PACKET_TOO_LARGE);
        return false;
    }
    switch (type) {
        case MQTTPUBLISH: {
            if (length < (size_t)hdrLen + 3) {
                ERROR_PSC_PRINTF_P("handlePacket: PUBLISH missing topic-length bytes (type=0x%02x length=%zu)\n", static_cast<unsigned int>(type), length);
                closeConnection(MQTT_MALFORMED_PACKET);
                return false;
            }
            const uint16_t topicLen = (uint16_t)(((uint16_t)buf[hdrLen + 1] << 8) | buf[hdrLen + 2]);
            const uint8_t publishQos = MQTT_HDR_GET_QOS(buf[0]);
            if (!topicLen || topicLen > length - ((size_t)hdrLen + 3) || publishQos > MQTT_QOS2) {
                ERROR_PSC_PRINTF_P("handlePacket: PUBLISH invalid topic length or QoS (topic=%u qos=%u length=%zu)\n", static_cast<unsigned int>(topicLen), static_cast<unsigned int>(publishQos), length);
                closeConnection(MQTT_MALFORMED_PACKET);
                return false;
            }
            size_t payloadOffset = (size_t)hdrLen + 3 + topicLen;
            uint16_t msgId = 0;
            bool duplicate = false;
            if (publishQos > MQTT_QOS0) {
                if (length - payloadOffset < 2) {
                    ERROR_PSC_PRINTF_P("handlePacket: PUBLISH missing packet identifier (type=0x%02x length=%zu)\n", static_cast<unsigned int>(type), length);
                    closeConnection(MQTT_MALFORMED_PACKET);
                    return false;
                }
                msgId = (uint16_t)(((uint16_t)buf[payloadOffset] << 8) | buf[payloadOffset + 1]);
                if (!msgId) {
                    ERROR_PSC_PRINTF_P("handlePacket: zero packet identifier (type=0x%02x length=%zu)\n", static_cast<unsigned int>(type), length);
                    closeConnection(MQTT_MALFORMED_PACKET);
                    return false;
                }
                payloadOffset += 2;
                if (publishQos == MQTT_QOS2) {
                    uint16_t* freeSlot = nullptr;
                    for (size_t i = 0; i < MQTT_RX_QOS2_MAX_INFLIGHT; ++i) {
                        if (_rxQos2Ids[i] == msgId) duplicate = true;
                        if (!_rxQos2Ids[i]) freeSlot = &_rxQos2Ids[i];
                    }
                    if (!duplicate) {
                        if (!freeSlot) {
                            ERROR_PSC_PRINTF_P("handlePacket: QoS2 inflight exhausted (id=%u capacity=%u)\n", static_cast<unsigned int>(msgId), static_cast<unsigned int>(MQTT_RX_QOS2_MAX_INFLIGHT));
                            closeConnection(MQTT_QOS2_INFLIGHT_EXHAUSTED);
                            return false; // no delivery/eviction of an uncompleted exchange
                        }
                        *freeSlot = msgId;
                    }
                }
            }
            if (callback && !duplicate) {
                char* topic = (char*)(buf + hdrLen + 2);
                memmove(topic, topic + 1, topicLen);
                topic[topicLen] = '\0';
                _rxFlags = (buf[0] & 0x0F) | MQTT_RX_BUSY;
                struct CallbackGuard {
                    uint8_t& flags;
                    ~CallbackGuard() { flags = 0; }
                } guard{_rxFlags};
                callback(topic, buf + payloadOffset, length - payloadOffset);
            }
            // The callback may disconnect, publish (overwriting _buffer), or close the transport.
            // Do not touch buf again. Buffer setters/connect are guarded for the callback's lifetime.
            if (!connected()) return false;
            if (_txActive) {
                // A callback must finish its manual publish before returning; an ACK cannot
                // be inserted into the still-open payload.
                closeConnection(MQTT_PUBLISH_INCOMPLETE);
                return false;
            }
            if (publishQos > MQTT_QOS0) {
                const uint8_t ack[] = {(uint8_t)(publishQos == MQTT_QOS1 ? MQTTPUBACK : MQTTPUBREC),
                                       2, (uint8_t)(msgId >> 8), (uint8_t)msgId};
                if (!writeControlResponse(ack, sizeof(ack))) return false;
            }
            break;
        }
        case MQTTPUBACK:
            // MQTT Publish Acknowledgment (QoS 1 publish received): See section 3.4 MQTT v3.1.1 protocol specification
            if (length < 4) {
                ERROR_PSC_PRINTF_P("handlePacket(): Received PUBACK packet with length %zu, expected at least 4 bytes\n", length);
                closeConnection(MQTT_MALFORMED_PACKET);
                return false;
            }
            // No futher action here, as resending is not supported.
            break;
        case MQTTPUBREC:
            // MQTT Publish Received (QoS 2 publisher handshake, part 1): broker acknowledges our QoS 2 PUBLISH.
            // See section 3.5 MQTT v3.1.1 protocol specification.
            if (length < 4) {
                ERROR_PSC_PRINTF_P("handlePacket(): Received PUBREC packet with length %zu, expected at least 4 bytes\n", length);
                closeConnection(MQTT_MALFORMED_PACKET);
                return false;
            }
            // MQTT Publish Release (QoS 2 publisher handshake, part 2): See section 3.6 MQTT v3.1.1 protocol specification
            buf[0] = MQTTPUBREL | 2;  // PUBREL fixed header: bit 1 must be set per spec
            // bytes 1-3 of PUBREL are the same as of PUBREC (remaining length + msgId)
            if (!writeControlResponse(buf, 4)) return false;
            break;
        case MQTTPUBREL:
            // MQTT Publish Release (QoS 2 subscriber handshake, part 2): broker releases the message to us.
            // See section 3.6 MQTT v3.1.1 protocol specification.
            if (length < 4) {
                ERROR_PSC_PRINTF_P("handlePacket(): Received PUBREL packet with length %zu, expected at least 4 bytes\n", length);
                closeConnection(MQTT_MALFORMED_PACKET);
                return false;
            }
            {
                const uint16_t msgId = (uint16_t)(((uint16_t)buf[2] << 8) | buf[3]);
                if (!msgId) {
                    ERROR_PSC_PRINTF_P("handlePacket: zero packet identifier (type=0x%02x length=%zu)\n", static_cast<unsigned int>(type), length);
                    closeConnection(MQTT_MALFORMED_PACKET);
                    return false;
                }
                for (size_t i = 0; i < MQTT_RX_QOS2_MAX_INFLIGHT; ++i) {
                    if (_rxQos2Ids[i] == msgId) _rxQos2Ids[i] = 0;
                }
            }
            // MQTT Publish Complete (QoS 2 subscriber handshake, part 3): See section 3.7 MQTT v3.1.1 protocol specification
            buf[0] = MQTTPUBCOMP;
            // bytes 1-3 of PUBCOMP are the same as of PUBREL (remaining length + msgId)
            if (!writeControlResponse(buf, 4)) return false;
            break;
        case MQTTPUBCOMP:
            // MQTT Publish Complete (QoS 2 publisher handshake, part 3): broker confirms delivery of our QoS 2 PUBLISH.
            // See section 3.7 MQTT v3.1.1 protocol specification.
            if (length < 4) {
                ERROR_PSC_PRINTF_P("handlePacket(): Received PUBCOMP packet with length %zu, expected at least 4 bytes\n", length);
                closeConnection(MQTT_MALFORMED_PACKET);
                return false;
            }
            // No futher action here, as resending is not supported.
            break;
        case MQTTPINGREQ:
            // MQTT Ping Request: See section 3.12 MQTT v3.1.1 protocol specification
            buf[0] = MQTTPINGRESP;
            buf[1] = 0;
            if (!writeControlResponse(buf, 2)) return false;
            break;
        case MQTTPINGRESP:
            _pingOutstanding = false;
            break;
        default:
            break;
    }
    return true;
}

bool PubSubClient::loop() {
    if (_rxFlags & MQTT_RX_BUSY) {
        // loop() called from within the message callback: the receive buffer still holds the message, so do not touch it
        ERROR_PSC_PRINTF_P("loop() must not be called from the message callback\n");
        return true;
    }
    if (!connected()) {
        return false;
    }
    // Until endPublish(), the peer is still reading the announced payload. No ACK/ping/other
    // packet can be sent, and blocking RX would reuse the payload buffer.
    if (_txActive) return true;
    bool ret = true;
    const unsigned long t = millis();
    if (_keepAliveMillis && ((t - _lastInActivity > _keepAliveMillis) || (t - _lastOutActivity > _keepAliveMillis))) {
        if (_pingOutstanding) {
            DEBUG_PSC_PRINTF("loop aborting due to timeout\n");
            closeConnection(MQTT_KEEPALIVE_TIMEOUT);
            return false;
        } else {
            _buffer[0] = MQTTPINGREQ;
            _buffer[1] = 0;
            // Automatic protocol writes make one transport attempt in incremental mode.
#if MQTT_ENABLE_INCREMENTAL_RX
            const bool allowRetry = _rxBuffer == nullptr;
#else
            const bool allowRetry = true;
#endif
            if (writeToClient(_buffer, 2, false, allowRetry)) {
                // Re-stamp both from t: writeToClient() set _lastOutActivity from a later millis() read of
                // its own, and the ping schedule must not drift apart for what is a single event.
                _lastInActivity = _lastOutActivity = t;
                _pingOutstanding = true;
            } else if (!connected()) {
                // A half-written PINGREQ desyncs the stream like any other truncated packet, so
                // writeToClient() closed the connection: do not fall through to the receive path.
                return false;
            }
            // Nothing written: the socket is still in sync, so leave _pingOutstanding alone and let a
            // later loop() send the ping once the transmit window frees up.
        }
    }
#if MQTT_ENABLE_INCREMENTAL_RX
    if (_rxBuffer) {
        // Non-blocking receive path (opt-in via setRxBufferSize()): reassemble into the dedicated _rxBuffer,
        // consuming only already-available bytes. Also streams payload bytes to a configured Stream (phase 2).
        // Runs every loop() to enforce the stall timeout while a packet is partially received. Does not flush
        // pending outgoing _buffer data: RX and TX use separate buffers, so an in-progress publish payload stays intact.
        if (!processInbound()) {
            // processInbound() has closed the connection and set _state
            return false;
        }
    } else
#endif
    if (_client->available()) {
        uint8_t hdrLen;
        size_t len = readPacket(&hdrLen);
        if (len > 0) {
            _lastInActivity = t;
            ret = handlePacket(_buffer, _bufferSize, hdrLen, len);
            if (!ret && _state == MQTT_CONNECTED) {
                ERROR_PSC_PRINTF_P("loop: packet handler rejected malformed packet\n");
                closeConnection(MQTT_MALFORMED_PACKET);
            }
        } else if (!connected()) {
            // readPacket has closed the connection
            return false;
        }
    }
    return ret;
}

#if MQTT_ENABLE_INCREMENTAL_RX
/**
 * @brief  Non-blocking incremental receive. Consumes only the bytes currently available in the network stack
 * (never waits) and reassembles the packet into the dedicated _rxBuffer across loop() calls. Dispatches a
 * completed packet via handlePacket(). Enforces the per-packet stall timeout so a half-received packet cannot
 * linger forever. Only used when a receive buffer is set, with or without a Stream (see loop()).
 *
 * @return true if processing is ok (packet dispatched, or still in progress). false if the connection was
 * closed (oversized/invalid packet, stall timeout, or handlePacket() failure); _state is set accordingly.
 */
bool PubSubClient::processInbound() {
    size_t budget = _maxBytesPerLoop ? _maxBytesPerLoop : (size_t)-1;  // bytes to process this loop() call
    bool progressed = false;
    unsigned long lastYield = millis();

    while ((budget > 0) && _client->available()) {
        int rc = _client->read();
        if (rc < 0) break;  // no byte despite available(): leave, retry next loop()
        const uint8_t b = (uint8_t)rc;
        budget--;
        progressed = true;

        switch (_rxState) {
            case RX_IDLE:
                // First byte of a new packet: the fixed-header type byte
                _rxBuffer[0] = b;
                _rxLen = 1;
                _rxLength = 0;
                _rxMultiplier = 1;
                _packetReadStart = millis();  // arm the stall timeout
                _rxState = RX_HEADER;
                break;

            case RX_HEADER:
                // Remaining-length varint (1 .. MQTT_MAX_HEADER_SIZE - 1 bytes)
                _rxBuffer[_rxLen++] = b;
                _rxLength += (size_t)(b & 0x7F) * _rxMultiplier;
                _rxMultiplier <<= 7;
                if ((b & 0x80) == 0) {
                    // Varint complete: decide how to consume the remaining length
                    _rxHdrLen = (uint8_t)(_rxLen - 1);
                    _rxStreamSkip = 0;  // (re)computed once the topic length is known (see RX_PAYLOAD)
                    if (_rxLength == 0) {
                        if (!dispatchInbound()) return false;  // no payload (e.g. PINGRESP): dispatch now
                        break;
                    }
                    _rxRemaining = _rxLength;
                    {
                        const size_t totalLen = (size_t)_rxHdrLen + 1 + _rxLength;  // type + remaining-length bytes + remaining length
                        if (_stream) {
                            // With a Stream, payload overflow is written to the Stream; only a bogus length disconnects.
                            if (totalLen > MQTT_MAX_SANE_PACKET_SIZE) {
                                ERROR_PSC_PRINTF_P("processInbound disconnecting on bogus streamed packet (%zu)\n", totalLen);
                                _rxState = RX_IDLE;
                                closeConnection(MQTT_PACKET_TOO_LARGE);
                                return false;
                            }
                            _rxState = RX_PAYLOAD;
                        } else {
                            size_t budgetSize = _rxBufferSize;
                            if (_maxIncomingPacket && (_maxIncomingPacket < budgetSize)) budgetSize = _maxIncomingPacket;
                            if (totalLen > budgetSize) {
                                if ((_oversizedPolicy == MQTT_OVERSIZED_IGNORE) && (totalLen <= MQTT_MAX_SANE_PACKET_SIZE)) {
                                    // Drop the payload incrementally and keep the connection in sync, without blocking.
                                    ERROR_PSC_PRINTF_P("processInbound ignoring oversized packet (%zu > budget %zu)\n", totalLen, budgetSize);
                                    _rxState = RX_DISCARD;
                                } else {
                                    // Fail-fast: close so the reconnect resynchronises the stream.
                                    ERROR_PSC_PRINTF_P("processInbound disconnecting on oversized packet (%zu > budget %zu)\n", totalLen, budgetSize);
                                    _rxState = RX_IDLE;
                                    closeConnection(MQTT_PACKET_TOO_LARGE);
                                    return false;
                                }
                            } else {
                                _rxState = RX_PAYLOAD;
                            }
                        }
                    }
                } else if (_rxLen == MQTT_MAX_HEADER_SIZE) {
                    // Continuation bit still set after the maximum number of length bytes: invalid encoding
                    ERROR_PSC_PRINTF_P("processInbound: malformed remaining-length encoding\n");
                    _rxState = RX_IDLE;
                    closeConnection(MQTT_MALFORMED_PACKET);
                    return false;
                }
                break;

            case RX_PAYLOAD: {
                // consumed = 0-based index of this byte within the remaining length
                const size_t consumed = _rxLength - _rxRemaining;
                if (_rxLen < _rxBufferSize) {
                    _rxBuffer[_rxLen++] = b;  // store header/topic/msgId and as much payload as fits
                }
                if (_stream && ((_rxBuffer[0] & 0xF0) == MQTTPUBLISH)) {
                    if (consumed == 2) {
                        // both topic-length bytes are now in the buffer: locate the start of the payload
                        const size_t topicLen = ((size_t)_rxBuffer[_rxHdrLen + 1] << 8) | _rxBuffer[_rxHdrLen + 2];
                        const size_t msgIdLen = (MQTT_HDR_GET_QOS(_rxBuffer[0]) > MQTT_QOS0) ? 2 : 0;
                        _rxStreamSkip = 2 + topicLen + msgIdLen;  // index within the remaining length where payload begins
                    }
                    if ((consumed >= 2) && (consumed >= _rxStreamSkip)) {
                        _stream->write(b);  // write payload bytes to the Stream (also those that did not fit the buffer)
                    }
                }
                if (--_rxRemaining == 0) {
                    if (!dispatchInbound()) return false;
                }
                break;
            }

            case RX_DISCARD:
                // Oversized packet: consume and drop the payload byte, keeping the stream in sync
                if (--_rxRemaining == 0) {
                    _rxState = RX_IDLE;  // fully drained: connection stays, packet ignored
                }
                break;

            default:
                _rxState = RX_IDLE;
                break;
        }

        const unsigned long now = millis();
        if (now - lastYield >= PUBSUB_YIELD_INTERVAL_MS) {
            PUBSUB_WAIT_YIELD();
            lastYield = now;
        }
    }

    if (progressed) _lastInActivity = millis();  // receiving data counts as activity (keepalive)

    if (_rxState != RX_IDLE) {
        // A packet is only partially received. Bound how long it may stay pending, so a peer that stops
        // mid-packet cannot pin the receive buffer forever. Does not block: just aborts on timeout.
        const unsigned long packetTimeout = _packetTimeoutMillis ? _packetTimeoutMillis : _socketTimeoutMillis;
        if (millis() - _packetReadStart >= packetTimeout) {
            ERROR_PSC_PRINTF_P("processInbound aborting stalled packet\n");
            _rxState = RX_IDLE;
            closeConnection(MQTT_PACKET_TIMEOUT);
            return false;
        }
    }
    return true;
}

/**
 * @brief  Dispatch a fully received packet from _rxBuffer to handlePacket() and reset the receive FSM.
 *
 * @return true if the packet was handled. false if handlePacket() failed; the connection is closed here.
 */
bool PubSubClient::dispatchInbound() {
    const bool ok = handlePacket(_rxBuffer, _rxBufferSize, _rxHdrLen, _rxLen);
    _rxState = RX_IDLE;  // ready for the next packet (handlePacket may have overwritten _rxBuffer with an ACK)
    if (!ok && _state == MQTT_CONNECTED) {
        ERROR_PSC_PRINTF_P("dispatchInbound: packet handler rejected malformed packet\n");
        closeConnection(MQTT_MALFORMED_PACKET);
    }
    return ok && connected();
}

#endif

bool PubSubClient::publish(const char* topic, const uint8_t* payload, size_t plength, uint8_t qos, bool retained) {
    if (beginPublish(topic, plength, qos, retained)) {
        size_t rc = write(payload, plength);
        return endPublish() && (rc == plength);
    }
    return false;
}

bool PubSubClient::publish(const __FlashStringHelper* topic, const uint8_t* payload, size_t plength, uint8_t qos, bool retained) {
    if (beginPublish(topic, plength, qos, retained)) {
        size_t rc = write(payload, plength);
        return endPublish() && (rc == plength);
    }
    return false;
}

bool PubSubClient::publish_P(const char* topic, const uint8_t* payload, size_t plength, uint8_t qos, bool retained) {
    if (beginPublish(topic, plength, qos, retained)) {
        size_t rc = write_P(payload, plength);
        return endPublish() && (rc == plength);
    }
    return false;
}

bool PubSubClient::publish_P(const __FlashStringHelper* topic, const uint8_t* payload, size_t plength, uint8_t qos, bool retained) {
    if (beginPublish(topic, plength, qos, retained)) {
        size_t rc = write_P(payload, plength);
        return endPublish() && (rc == plength);
    }
    return false;
}

/**
 * @brief Internal beginPublish implementation using topic stored in RAM or PROGMEM.
 *
 * @param progmem true if the topic is stored in PROGMEM/Flash, false if in RAM.
 * @param topic The topic to publish to.
 * @param plength The length of the payload.
 * @param qos The quality of service (\ref group_qos) to publish at. [0, 1, 2].
 * @param retained Publish the message with the retain flag.
 * @return true If the publish succeeded.
 * false If the publish failed, either connection lost or message too large.
 */
bool PubSubClient::beginPublishImpl(bool progmem, const char* topic, size_t plength, uint8_t qos, bool retained) {
    if (!topic || _txActive || qos > MQTT_QOS2) return false;
    const size_t topicLen = progmem ? strlen_P(topic) : strlen(topic);
    if (!topicLen || topicLen > 0xFFFF) return false;
    const size_t nextMsgLen = qos > MQTT_QOS0 ? 2 : 0;
    const size_t overhead = 2 + nextMsgLen;
    // Subtract before adding: plength may be SIZE_MAX and size_t may be only 16 bits.
    if (topicLen > MQTT_MAX_POSSIBLE_PACKET_SIZE - overhead ||
        plength > MQTT_MAX_POSSIBLE_PACKET_SIZE - overhead - topicLen) return false;
    if (!connected() || _bufferSize < MQTT_MAX_HEADER_SIZE + overhead ||
        topicLen > _bufferSize - MQTT_MAX_HEADER_SIZE - overhead) return false;
    const size_t end = writeStringImpl(progmem, topic, MQTT_MAX_HEADER_SIZE);
    if (end == MQTT_MAX_HEADER_SIZE) return false;
    if (nextMsgLen && writeNextMsgId(end) != end + nextMsgLen) return false;
    const uint8_t header = MQTTPUBLISH | MQTT_QOS_GET_HDR(qos) | (retained ? MQTTRETAINED : 0);
    const uint8_t hdrLen = buildHeader(header, overhead + topicLen + plength);
    if (!hdrLen || !writeToClient(_buffer + MQTT_MAX_HEADER_SIZE - hdrLen, hdrLen + overhead + topicLen)) return false;
    _bufferWritePos = 0;
    _txRemaining = plength;
    _txActive = true;
    return true;
}

bool PubSubClient::endPublish() {
    if (!_txActive || !connected()) return false;
    if (_txRemaining) {
        closeConnection(MQTT_PUBLISH_INCOMPLETE);
        return false;
    }
    if (_bufferWritePos && flushBuffer() == 0) return false;
    _txActive = false;
    return true;
}

/**
 * @brief  Build up the header ready to send.
 * Note: the header is built at the end of the first MQTT_MAX_HEADER_SIZE bytes, so will start
 * (MQTT_MAX_HEADER_SIZE - <returned size>) bytes into the _buffer.
 *
 * @param  header Header byte, e.g. MQTTCONNECT, MQTTPUBLISH, MQTTSUBSCRIBE, MQTTUNSUBSCRIBE.
 * @param  length Length to encode in the header.
 * @return Returns the size of the header (1 .. MQTT_MAX_HEADER_SIZE), or 0 in case of a failure (e.g. length to big).
 */
uint8_t PubSubClient::buildHeader(uint8_t header, size_t length) {
    uint8_t hdrBuf[MQTT_MAX_HEADER_SIZE - 1];
    uint8_t hdrLen = 0;
    uint8_t digit;
    size_t len = length;
    do {
        digit = len & 0x7F;  // digit = len % 128
        len >>= 7;           // len = len / 128
        if (len > 0) {
            digit |= 0x80;
        }
        hdrBuf[hdrLen++] = digit;
    } while ((len > 0) && (hdrLen < MQTT_MAX_HEADER_SIZE - 1));

    if (len > 0) {
        ERROR_PSC_PRINTF_P("buildHeader: header=0x%02X, length too big %zu, left %zu\n", header, length, len);
        return 0;
    }

    _buffer[MQTT_MAX_HEADER_SIZE - 1 - hdrLen] = header;
    memcpy(_buffer + MQTT_MAX_HEADER_SIZE - hdrLen, hdrBuf, hdrLen);
    return hdrLen + 1;  // Full header size is variable length bit plus the 1-byte fixed header
}

size_t PubSubClient::write(uint8_t data) {
    return appendBuffer(data);
}

size_t PubSubClient::write(const uint8_t* buf, size_t size) {
    if (!buf || !_txActive || size > _txRemaining) return 0;
    for (size_t i = 0; i < size; i++) {
        if (appendBuffer(buf[i]) == 0) return i;
    }
    return size;
}

size_t PubSubClient::write_P(const uint8_t* buf, size_t size) {
    if (!buf || !_txActive || size > _txRemaining) return 0;
    for (size_t i = 0; i < size; i++) {
        if (appendBuffer((uint8_t)pgm_read_byte_near(buf + i)) == 0) return i;
    }
    return size;
}

/**
 * @brief Write to the network client, completing short writes when retries are enabled.
 *
 * A Client may accept fewer bytes than asked (a full transmit window, a timeout hit mid-record), and what that
 * leaves behind depends on how far it got:
 * - Nothing written and no earlier part of this packet sent: the stream is still correctly framed.
 *   Return immediately if the first write accepts zero bytes; the caller can retry on a later loop.
 * - Partially written: the peer is waiting for the rest of this packet. Either complete it before
 *   sending another packet, or close the connection. Abandoning it on an open connection corrupts framing.
 * The second case is why the bytes that did go out must be followed by the rest: the packet is still in the
 * caller's buffer and the count tells exactly how much is missing, so this retries the remainder (yielding
 * between attempts) until the socket accepts it, the socket closes, or _socketTimeoutMillis runs out. With
 * retries disabled, an incomplete first attempt that started a packet closes instead. This deadline does not configure
 * underlying Client timeouts or bound the duration of a single blocking transport call.
 *
 * A failed write closes the connection if this call or an earlier part of the same packet sent bytes: a reconnect is the
 * one way back in sync, and the alternative is worse than it looks — the next PINGREQ is swallowed by the same
 * truncated packet, so the keepalive tears the connection down anyway, up to two keepalive intervals later,
 * with every publish in between reported as sent and understood by nobody.
 *
 * @param  buf Bytes to write.
 * @param  size Number of bytes to write.
 * @param  packetStarted True if an earlier part of this MQTT packet was already sent (e.g. the PUBLISH header).
 * @param  allowRetry False makes at most one transport write (for a nonempty buffer), with no retry-loop
 * yield or deadline-clock sampling. Closure precedence on an incomplete attempt:
 * - Any byte sent, or packetStarted=true: close internally with failureState, then return false.
 * - Zero bytes and packetStarted=false: return false without closing; the caller chooses its policy.
 * Callers must not close again after an internal close or overwrite its state. A successful send still
 * samples millis() once to update _lastOutActivity. The transport/stop calls themselves may block.
 * @param failureState Reason retained on an incomplete send; mandatory replies use #MQTT_CONTROL_RESPONSE_FAILED.
 * @return true if all size bytes were written (_lastOutActivity updated). false if they were not: if no part
 * of the packet was sent the connection is left alone, otherwise the connection is
 * closed and _state is set to failureState (default #MQTT_WRITE_INCOMPLETE).
 */
bool PubSubClient::writeToClient(const uint8_t* buf, size_t size, bool packetStarted, bool allowRetry, int failureState) {
    if (!_client) return false;

    size_t written = 0;
    const unsigned long start = allowRetry ? millis() : 0;

    while (written < size) {
        written += _client->write(buf + written, size - written);
        if (written == size) break;
        if ((written == 0) && !packetStarted) {
            // No part of this packet was sent: a later packet can still start at a valid boundary.
            ERROR_PSC_PRINTF_P("writeToClient(): wrote no byte of %zu, connection kept\n", size);
            return false;
        }
        if (!allowRetry || millis() - start >= _socketTimeoutMillis) break;
        if (!_client->connected()) break;  // fail fast instead of retrying into a closed socket
        PUBSUB_WAIT_YIELD();
    }

    if (written == size) {
        _lastOutActivity = millis();
        return true;
    }

    ERROR_PSC_PRINTF_P("writeToClient(): sent=%zu expected=%zu packetStarted=%u reason=%d; closing\n", written, size, (unsigned int)packetStarted, failureState);
    closeConnection(failureState);
    return false;
}

// Required replies cannot be forgotten after consuming the incoming packet. Unlike PINGREQ,
// even a zero-byte write closes the connection; incremental mode never retries the transport.
bool PubSubClient::writeControlResponse(const uint8_t* buf, size_t size) {
#if MQTT_ENABLE_INCREMENTAL_RX
    const bool allowRetry = _rxBuffer == nullptr;
#else
    const bool allowRetry = true;
#endif
    // Capture before closing/resetting the transport; no access to the RX packet after callback.
    const unsigned int responseType = size ? static_cast<unsigned int>(buf[0]) : 0u;
    (void)responseType; // logging may be compiled out
    if (writeToClient(buf, size, false, allowRetry, MQTT_CONTROL_RESPONSE_FAILED)) return true;
    ERROR_PSC_PRINTF_P("required response 0x%02x failed (expected=%zu reason=%d)\n", responseType, size, MQTT_CONTROL_RESPONSE_FAILED);
    // Partial writes have already closed internally; only close a still-open failed response.
    if (_state == MQTT_CONNECTED) closeConnection(MQTT_CONTROL_RESPONSE_FAILED);
    return false;
}

/**
 * @brief  Write a MQTT Control Packet (header and the prepared data) to the client / MQTT broker. The prepared data of size length must already be in the
 * internal _buffer starting at MQTT_MAX_HEADER_SIZE (space is needed for header generation).
 *
 * @param  header Header byte, e.g. MQTTCONNECT, MQTTPUBLISH, MQTTSUBSCRIBE, MQTTUNSUBSCRIBE.
 * @param  length Length of _buffer to write.
 * @return True if successfully sent, otherwise false if build header failed or buffer could not be written.
 */
bool PubSubClient::writeControlPacket(uint8_t header, size_t length) {
    uint8_t hdrLen = buildHeader(header, length);
    if (hdrLen == 0) return false;  // exit here in case of header generation failure

    return writeBuffer(MQTT_MAX_HEADER_SIZE - hdrLen, hdrLen + length);
}

/**
 * @brief  Write the internal _buffer to the client / MQTT broker.
 *
 * @param  pos Position in the _buffer to start writing from.
 * @param  size Number of bytes to write from the _buffer.
 * @param  packetStarted True if an earlier part of the same MQTT packet was already sent.
 * @return Number of bytes written to the client / MQTT broker (0 .. bufferSize). If 0 is returned a write error occurred or buffer index error.
 */
size_t PubSubClient::writeBuffer(size_t pos, size_t size, bool packetStarted) {
    size_t rc = 0;
    if (_client && (size > 0) && (pos <= _bufferSize) && (size <= _bufferSize - pos)) {
#ifdef MQTT_MAX_TRANSFER_SIZE
        uint8_t* writeBuf = _buffer + pos;
        size_t bytesRemaining = size;
        bool result = true;
        while ((bytesRemaining > 0) && result) {
            size_t bytesToWrite = (bytesRemaining > MQTT_MAX_TRANSFER_SIZE) ? MQTT_MAX_TRANSFER_SIZE : bytesRemaining;
            // writeToClient() completes a short chunk before giving up: the packet is split across chunks,
            // so abandoning one mid-packet is the truncated-packet desync, not a lost chunk.
            result = writeToClient(writeBuf, bytesToWrite, packetStarted || (bytesRemaining < size));
            if (result) {
                bytesRemaining -= bytesToWrite;
                writeBuf += bytesToWrite;
            }
            PUBSUB_WAIT_YIELD();
        }
        rc = result ? size : 0;  // if result is false indicate a write error
#else
        rc = writeToClient(_buffer + pos, size, packetStarted) ? size : 0;  // 0 indicates a write error
#endif
    }
    return rc;
}

/**
 * @brief  Internal implementation of writeString using RAM or PROGMEM string.
 * Write an UTF-8 encoded string to the internal buffer at a given position. The string can have a length of 0 to 65535 bytes (depending on size of
 * internal buffer). The buffer is prefixed with two bytes representing the length of the string. See section 1.5.3 of MQTT v3.1.1 protocol specification.
 * @note   If the string does not fit in the buffer or is longer than 65535 bytes nothing is written to the buffer and the returned position is
 * unchanged.
 *
 * @param  progmem true if the string is stored in PROGMEM, false if in RAM.
 * @param  string 'C' string of the data that shall be written in the buffer.
 * @param  pos Position in the internal buffer to write the string.
 * @return New position in the internal buffer (pos + 2 + string length), or pos if a buffer overrun would occur or the string is a nullptr.
 */
size_t PubSubClient::writeStringImpl(bool progmem, const char* string, size_t pos) {
    if (!string) return pos;

    size_t sLen = progmem ? strlen_P(string) : strlen(string);
    if ((pos <= _bufferSize) && (_bufferSize - pos >= 2) && (sLen <= _bufferSize - pos - 2) && (sLen <= 0xFFFF)) {
        _buffer[pos++] = (uint8_t)(sLen >> 8);
        _buffer[pos++] = (uint8_t)(sLen & 0xFF);
        if (progmem) {
            memcpy_P(_buffer + pos, string, sLen);
        } else {
            memcpy(_buffer + pos, string, sLen);
        }
        pos += sLen;
    } else {
        ERROR_PSC_PRINTF_P("writeStringImpl(): string (%zu) does not fit into buf (%zu)\n", pos + 2 + sLen, _bufferSize);
    }
    return pos;
}

/**
 * @brief  Write an UTF-8 encoded string to the internal buffer at a given position. The string can have a length of 0 to 65535 bytes (depending on size of
 * internal buffer). The buffer is prefixed with two bytes representing the length of the string. See section 1.5.3 of MQTT v3.1.1 protocol specification.
 * @note   If the string does not fit in the buffer or is longer than 65535 bytes nothing is written to the buffer and the returned position is
 * unchanged.
 *
 * @param  string 'C' string of the data that shall be written in the buffer.
 * @param  pos Position in the internal buffer to write the string.
 * @return New position in the internal buffer (pos + 2 + string length), or pos if a buffer overrun would occur or the string is a nullptr.
 */
inline size_t PubSubClient::writeString(const char* string, size_t pos) {
    return writeStringImpl(false, string, pos);
}

/**
 * @brief  Write nextMsgId to the internal buffer at the given position.
 * @note   If the nextMsgId (2 bytes) does not fit in the buffer nothing is written to the buffer and the returned position is unchanged.
 *
 * @param  pos Position in the internal buffer to write the nextMsgId.
 * @return New position in the internal buffer (pos + 2), or pos if a buffer overrun would occur.
 */
size_t PubSubClient::writeNextMsgId(size_t pos) {
    if ((pos <= _bufferSize) && (_bufferSize - pos >= 2)) {
        // increment msgId (must not be 0, so wrap to 1). Kept as two statements: modifying and reading
        // _nextMsgId in a single expression is undefined behavior before C++17.
        if (++_nextMsgId == 0) _nextMsgId = 1;
        _buffer[pos++] = (uint8_t)(_nextMsgId >> 8);
        _buffer[pos++] = (uint8_t)(_nextMsgId & 0xFF);
    } else {
        ERROR_PSC_PRINTF_P("writeNextMsgId(): buffer overrun (%zu) \n", pos + 2);
    }
    return pos;
}

/**
 * @brief  Append a byte to the internal _buffer. If the _buffer is full it is flushed to the client / MQTT broker.
 *
 * @param  data Byte to append to the _buffer.
 * @return Number of bytes appended to the _buffer (0 or 1). If 0 is returned a write error occurred.
 */
size_t PubSubClient::appendBuffer(uint8_t data) {
    if (!_txActive || !_txRemaining || !_buffer || !connected()) return 0;
    _buffer[_bufferWritePos++] = data;
    --_txRemaining;
    if (_bufferWritePos >= _bufferSize) {
        if (flushBuffer() == 0) return 0;
    }
    return 1;
}

/**
 * @brief  Flush the internal _buffer (bytes 0 .. _bufferWritePos) to the client / MQTT broker.
 * This is used by endPublish() to flush data written by appendBuffer().
 *
 * @return Number of bytes written to the client / MQTT broker (0 .. bufferSize). If 0 is returned a write error occurred or the _buffer was empty.
 */
size_t PubSubClient::flushBuffer() {
    size_t rc = 0;
    if (connected()) {
        // Payload continues a PUBLISH whose header was already sent by beginPublishImpl().
        rc = writeBuffer(0, _bufferWritePos, true);
    }
    _bufferWritePos = 0;
    return rc;
}

/**
 * @brief Internal subscribes to messages published to the specified topic. The topic can be stored in RAM or PROGMEM.
 * @param progmem true if the topic is stored in PROGMEM/Flash, false if in RAM.
 * @param topic The topic to subscribe to.
 * @param qos The qos to subscribe at. [0, 1].
 * @return true If sending the subscribe succeeded.
 * false If sending the subscribe failed, either connection lost or message too large.
 */
bool PubSubClient::subscribeImpl(bool progmem, const char* topic, uint8_t qos) {
    if (!topic || _txActive) return false;
    if (qos > MQTT_QOS1) return false;  // only QoS 0 and 1 supported

    // get topic length depending on storage (RAM vs PROGMEM)
    size_t topicLen = progmem ? strnlen_P(topic, _bufferSize) : strnlen(topic, _bufferSize);
    if (!topicLen || topicLen > 0xFFFF || _bufferSize < MQTT_MAX_HEADER_SIZE + 5 ||
        topicLen > _bufferSize - MQTT_MAX_HEADER_SIZE - 5) {
        // Too long: header + nextMsgId (2) + topic length bytes (2) + topicLen + QoS (1)
        return false;
    }
    if (connected()) {
        // Leave room in the _buffer for header and variable length field
        size_t length = MQTT_MAX_HEADER_SIZE;
        length = writeNextMsgId(length);  // _buffer size is checked before
        const size_t next = writeStringImpl(progmem, topic, length);
        if (next == length) return false;
        length = next;
        _buffer[length++] = qos;
        return writeControlPacket(MQTTSUBSCRIBE | MQTT_QOS_GET_HDR(MQTT_QOS1), length - MQTT_MAX_HEADER_SIZE);
    }
    return false;
}

/**
 * @brief Internal unsubscribes from messages published to the specified topic. The topic can be stored in RAM or PROGMEM.
 * @param progmem true if the topic is stored in PROGMEM/Flash, false if in RAM.
 * @param topic The topic to unsubscribe from.
 * @return true If sending the unsubscribe succeeded.
 * false If sending the unsubscribe failed, either connection lost or message too large.
 */
bool PubSubClient::unsubscribeImpl(bool progmem, const char* topic) {
    if (!topic || _txActive) return false;

    // get topic length depending on storage (RAM vs PROGMEM)
    size_t topicLen = progmem ? strnlen_P(topic, _bufferSize) : strnlen(topic, _bufferSize);
    if (!topicLen || topicLen > 0xFFFF || _bufferSize < MQTT_MAX_HEADER_SIZE + 4 ||
        topicLen > _bufferSize - MQTT_MAX_HEADER_SIZE - 4) {
        // Too long: header + nextMsgId (2) + topic length bytes (2) + topicLen
        return false;
    }
    if (connected()) {
        size_t length = MQTT_MAX_HEADER_SIZE;
        length = writeNextMsgId(length);  // _buffer size is checked before
        const size_t next = writeStringImpl(progmem, topic, length);
        if (next == length) return false;
        length = next;
        return writeControlPacket(MQTTUNSUBSCRIBE | MQTT_QOS_GET_HDR(MQTT_QOS1), length - MQTT_MAX_HEADER_SIZE);
    }
    return false;
}

PubSubClient& PubSubClient::setServer(uint8_t* ip, uint16_t port) {
    if (!ip) return *this;  // do not crash on a null pointer
    IPAddress addr(ip[0], ip[1], ip[2], ip[3]);
    return setServer(addr, port);
}

PubSubClient& PubSubClient::setServer(IPAddress ip, uint16_t port) {
    if ((_rxFlags & MQTT_RX_BUSY) || _txActive) return *this;
    if (_domain || _port != port || !(_ip == ip)) clearQos2Session();
    _ip = ip;
    _port = port;
    free(_domain);
    _domain = nullptr;
    return *this;
}

PubSubClient& PubSubClient::setServer(const char* domain, uint16_t port) {
    if ((_rxFlags & MQTT_RX_BUSY) || _txActive) return *this;
    if (!domain || !_domain || _port != port || strcmp(_domain, domain) != 0) clearQos2Session();
    char* newDomain = nullptr;
    if (domain) {
        newDomain = (char*)realloc(_domain, strlen(domain) + 1);
    }
    if (newDomain) {
        strcpy(newDomain, domain);
        _domain = newDomain;
        _port = port;
    } else {
        free(_domain);
        _domain = nullptr;
        _port = 0;
    }
    return *this;
}

PubSubClient& PubSubClient::setCallback(MQTT_CALLBACK_SIGNATURE) {
    this->callback = callback;
    return *this;
}

PubSubClient& PubSubClient::setClient(Client& client) {
    if ((_rxFlags & MQTT_RX_BUSY) || _txActive) return *this;
    if (_client != &client) clearQos2Session();
    _client = &client;
    return *this;
}

PubSubClient& PubSubClient::setStream(Stream& stream) {
    if ((_rxFlags & MQTT_RX_BUSY) || _txActive) return *this;
    _stream = &stream;
    return *this;
}

bool PubSubClient::setBufferSize(size_t size) {
    if ((_rxFlags & MQTT_RX_BUSY) || _txActive) return false;
    if (_bufferSize == size) return true;  // if size is unchanged, do nothing and return true
    if (size == 0) {
        // allow to free the buffer if the size is set to 0, but only if disconnected, otherwise we would free the buffer while it is still in use
        if (_state != MQTT_CONNECTED) {
            free(_buffer);
            _buffer = nullptr;
            _bufferSize = 0;
            _bufferWritePos = 0;
        }
        return (_bufferSize == 0);
    }
    if (size < MQTT_MIN_BUFFER_SIZE) {
        // Too small: connect() writes at least MQTT_MIN_BUFFER_SIZE bytes (header, protocol name, version,
        // flags, keepalive and client ID length) and readPacket() up to MQTT_MAX_HEADER_SIZE + 3 bytes
        // into the buffer without further size checks
        return false;
    }
    if (size > MQTT_MAX_POSSIBLE_PACKET_SIZE) {
        // Larger than any possible MQTT packet: reject instead of attempting a huge (likely failing) allocation
        return false;
    }
    if (_bufferWritePos >= size) {
        // Shrinking below pending outgoing data (beginPublish/write): flush it first to
        // keep the appendBuffer() invariant (_bufferWritePos < _bufferSize)
        flushBuffer();
    }
    uint8_t* newBuffer = (uint8_t*)realloc(_buffer, size);  // realloc(nullptr, size) is equivalent to malloc(size)
    if (!newBuffer) {
        // Keep the previous buffer and size untouched, so the client stays usable
        return false;
    }
    _buffer = newBuffer;
    _bufferSize = size;
    return true;
}

size_t PubSubClient::getBufferSize() {
    return _bufferSize;
}

PubSubClient& PubSubClient::setMaxIncomingPacket(size_t size) {
    _maxIncomingPacket = size;
    return *this;
}

size_t PubSubClient::getMaxIncomingPacket() {
    return _maxIncomingPacket;
}

#if MQTT_ENABLE_INCREMENTAL_RX
bool PubSubClient::setRxBufferSize(size_t size) {
    if ((_rxFlags & MQTT_RX_BUSY) || _txActive) return false;
    if (_rxState != RX_IDLE) return false;  // never resize/free while a packet is being reassembled
    if (size == 0) {
        free(_rxBuffer);
        _rxBuffer = nullptr;
        _rxBufferSize = 0;
        return true;  // disabled: back to the blocking receive path
    }
    if (size < MQTT_MIN_BUFFER_SIZE) return false;            // too small to hold a packet header safely
    if (size > MQTT_MAX_POSSIBLE_PACKET_SIZE) return false;   // larger than any possible MQTT packet: reject instead of trying a huge alloc
    uint8_t* newBuffer = (uint8_t*)realloc(_rxBuffer, size);  // realloc(nullptr, size) == malloc(size)
    if (!newBuffer) {
        // Keep the previous buffer/size: on failure with no previous buffer the client simply stays on the
        // blocking path, so it remains usable instead of crashing.
        return false;
    }
    _rxBuffer = newBuffer;
    _rxBufferSize = size;
    return true;
}

size_t PubSubClient::getRxBufferSize() {
    return _rxBufferSize;
}

PubSubClient& PubSubClient::setMaxBytesPerLoop(size_t maxBytes) {
    _maxBytesPerLoop = maxBytes;
    return *this;
}

size_t PubSubClient::getMaxBytesPerLoop() {
    return _maxBytesPerLoop;
}

#endif

PubSubClient& PubSubClient::setOversizedPolicy(uint8_t policy) {
    _oversizedPolicy = (policy == MQTT_OVERSIZED_IGNORE) ? MQTT_OVERSIZED_IGNORE : MQTT_OVERSIZED_DISCONNECT;
    return *this;
}

uint8_t PubSubClient::getOversizedPolicy() {
    return _oversizedPolicy;
}

PubSubClient& PubSubClient::setKeepAlive(uint16_t keepAlive) {
    _keepAliveMillis = keepAlive * 1000UL;
    return *this;
}

PubSubClient& PubSubClient::setSocketTimeout(uint16_t timeout) {
    _socketTimeoutMillis = (timeout ? timeout : MQTT_SOCKET_TIMEOUT) * 1000UL;
    return *this;
}

PubSubClient& PubSubClient::setPacketTimeout(uint16_t timeout) {
    _packetTimeoutMillis = timeout * 1000UL;
    return *this;
}

uint16_t PubSubClient::getPacketTimeout() {
    return (uint16_t)(_packetTimeoutMillis / 1000UL);
}

int PubSubClient::state() {
    return _state;
}
