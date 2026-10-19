/**
 * @file PubSubClient.h
 * @brief A simple client for MQTT.
 * @author Nicholas O'Leary - http://knolleary.net
 * @author Holger Mueller - https://github.com/hmueller01/pubsubclient3
 * @copyright MIT License 2008-2026
 *
 * This file is part of the PubSubClient library.
 */

#ifndef PubSubClient_h
#define PubSubClient_h

#include <Arduino.h>

#include "Client.h"
#include "IPAddress.h"
#include "Stream.h"

#define MQTT_VERSION_3_1   3  ///< Defines MQTT 3.1 protocol version, see #MQTT_VERSION
#define MQTT_VERSION_3_1_1 4  ///< Defines MQTT 3.1.1 protocol version, see #MQTT_VERSION

//< @note The following #define directives can be used to configure the library.

/**
 * @brief Sets the version of the MQTT protocol to use (3.1 or 3.1.1). [#MQTT_VERSION_3_1, #MQTT_VERSION_3_1_1].
 * @note Default value is #MQTT_VERSION_3_1_1 for MQTT 3.1.1.
 */
#ifndef MQTT_VERSION
#define MQTT_VERSION MQTT_VERSION_3_1_1
#endif

/**
 * @brief Maximum packet size defined by MQTT protocol.
 */
#ifndef MQTT_MAX_POSSIBLE_PACKET_SIZE
#define MQTT_MAX_POSSIBLE_PACKET_SIZE ((size_t)268435455)  // might be limited to 65535 if size_t is 16-bit (unsigned int)
#endif

/**
 * @brief Platform-aware sane upper bound for an incoming packet size, in bytes.
 * The MQTT protocol maximum (#MQTT_MAX_POSSIBLE_PACKET_SIZE, 256 MB) is far beyond what constrained targets can
 * ever receive, so it is a poor guard: a corrupted/garbage remaining-length field (e.g. a flipped bit turning a
 * 20-byte packet into a 200 MB "packet" on a flaky link or a TLS desync) looks valid but indicates a broken
 * stream. This constant is the threshold above which an oversized packet is treated as bogus and the connection
 * is always closed (fail-fast reconnect, which is the only way to resynchronise), regardless of the oversized
 * policy (see setOversizedPolicy()). Tuned per platform; override with -D to fit your product.
 */
#ifndef MQTT_MAX_SANE_PACKET_SIZE
#if defined(ESP8266)
#define MQTT_MAX_SANE_PACKET_SIZE ((size_t)16384)  // ESP8266: little free heap (shared with WiFi/TLS)
#elif defined(ESP32) || defined(ARDUINO_ARCH_ESP32)
#define MQTT_MAX_SANE_PACKET_SIZE ((size_t)65536)  // ESP32 without PSRAM
#else
#define MQTT_MAX_SANE_PACKET_SIZE MQTT_MAX_POSSIBLE_PACKET_SIZE  // generic: fall back to the protocol maximum
#endif
#endif

/**
 * @defgroup group_oversized Oversized packet policy
 * @brief How to handle an incoming packet that does not fit the receive buffer, used by setOversizedPolicy().
 * @{
 */
#define MQTT_OVERSIZED_DISCONNECT ((uint8_t)0)  ///< Default: close the connection (fail-fast). A reconnect resynchronises the stream, which is the robust choice on unreliable links where an oversized length often means corruption/desync.
#define MQTT_OVERSIZED_IGNORE     ((uint8_t)1)  ///< Drain/drop the packet and keep the connection (the classic behavior). Only applied up to #MQTT_MAX_SANE_PACKET_SIZE; a larger (bogus) packet still disconnects.
/** @} */

/**
 * @brief Sets the largest packet size, in bytes, the client will handle.
 * Any packet received that exceeds this size will be ignored.
 * This value can be overridden by calling setBufferSize.
 * @note Default value is 256 bytes.
 */
#ifndef MQTT_MAX_PACKET_SIZE
#define MQTT_MAX_PACKET_SIZE 256
#endif

/**
 * @brief Sets the keepalive interval, in seconds, the client will use.
 * This is used to maintain the connection when no other packets are being sent or received.
 * This value can be overridden by calling setKeepAlive.
 * @note Default value is 15 seconds.
 */
#ifndef MQTT_KEEPALIVE
#define MQTT_KEEPALIVE 15
#endif

/**
 * @brief Sets the timeout, in seconds, when reading from the network.
 * This also applies as the timeout for calls to connect.
 * This value can be overridden by calling setSocketTimeout.
 * @note Default value is 15 seconds.
 */
#ifndef MQTT_SOCKET_TIMEOUT
#define MQTT_SOCKET_TIMEOUT 15
#endif

/**
 * @brief Sets the maximum number of bytes passed to the network client in each write call.
 * Some hardware has a limit to how much data can be passed to them in one go,
 * such as the Arduino Wifi Shield e.g. use 80.
 * @note Defaults to undefined, which passes the entire packet in each write call.
 */
#ifndef MQTT_MAX_TRANSFER_SIZE  // just a hack that it gets shown in Doxygen
#define MQTT_MAX_TRANSFER_SIZE 80
#undef MQTT_MAX_TRANSFER_SIZE
#endif

/**
 * @defgroup group_state state() result
 * @brief These values indicate the current PubSubClient::state() of the client.
 * @{
 */
#define MQTT_CONNECTION_TIMEOUT      -4  ///< The network connection timed out or server didn't respond within the keepalive time.
#define MQTT_CONNECTION_LOST         -3  ///< The network connection was lost/broken.
#define MQTT_CONNECT_FAILED          -2  ///< The network connection failed.
#define MQTT_DISCONNECTED            -1  ///< The client is disconnected cleanly.
#define MQTT_CONNECTED               0   ///< The client is connected.
#define MQTT_CONNECT_BAD_PROTOCOL    1   ///< The server does not support the requested MQTT version.
#define MQTT_CONNECT_BAD_CLIENT_ID   2   ///< The server rejected the client identifier.
#define MQTT_CONNECT_UNAVAILABLE     3   ///< The server was unable to accept the connection.
#define MQTT_CONNECT_BAD_CREDENTIALS 4   ///< The username or password is not valid.
#define MQTT_CONNECT_UNAUTHORIZED    5   ///< The client is not authorized to connect to the server.
/** @} */

/// \cond
#define MQTTRETAINED    1        // Retained flag in the header
#define MQTT_RX_DUP     0x08     // DUP flag in the PUBLISH header
#define MQTT_RX_BUSY    0x80     // Set while the message callback is running (loop() reentrancy guard)
#define MQTTCONNECT     1 << 4   // Client request to connect to Server
#define MQTTCONNACK     2 << 4   // Connect Acknowledgment
#define MQTTPUBLISH     3 << 4   // Publish message
#define MQTTPUBACK      4 << 4   // Publish Acknowledgment
#define MQTTPUBREC      5 << 4   // Publish Received (assured delivery part 1)
#define MQTTPUBREL      6 << 4   // Publish Release (assured delivery part 2)
#define MQTTPUBCOMP     7 << 4   // Publish Complete (assured delivery part 3)
#define MQTTSUBSCRIBE   8 << 4   // Client Subscribe request
#define MQTTSUBACK      9 << 4   // Subscribe Acknowledgment
#define MQTTUNSUBSCRIBE 10 << 4  // Client Unsubscribe request
#define MQTTUNSUBACK    11 << 4  // Unsubscribe Acknowledgment
#define MQTTPINGREQ     12 << 4  // PING Request
#define MQTTPINGRESP    13 << 4  // PING Response
#define MQTTDISCONNECT  14 << 4  // Client is Disconnecting
#define MQTTRESERVED    15 << 4  // Reserved
/// \endcond

/**
 * @defgroup group_qos QoS levels
 * @brief Quality of Service (QoS) levels for MQTT messages.
 * @{
 */
#define MQTT_QOS0 ((uint8_t)0)  ///< Quality of Service 0: At most once
#define MQTT_QOS1 ((uint8_t)1)  ///< Quality of Service 1: At least once
#define MQTT_QOS2 ((uint8_t)2)  ///< Quality of Service 2: Exactly once
/// \cond
#define MQTT_QOS_GET_HDR(qos) (((qos) & 0x03) << 1) // Get QoS header bits from QoS value
#define MQTT_HDR_GET_QOS(header) (((header) & 0x06 ) >> 1) // Get QoS value from MQTT header
/// \endcond
/** @} */

/// \cond
// Maximum size of fixed header and variable length size header
#define MQTT_MAX_HEADER_SIZE 5
// Minimal buffer size that can be handled, used to check if the buffer size is sufficient in setBufferSize().
// MQTT_MAX_HEADER_SIZE (5) + protocol name and version (9 for MQTT 3.1, 7 for MQTT 3.1.1) + flags (1) + keepalive (2)
// + client ID length field (2) covers a minimal CONNECT message with an empty client ID
#if MQTT_VERSION == MQTT_VERSION_3_1
#define MQTT_MIN_BUFFER_SIZE (MQTT_MAX_HEADER_SIZE + 9 + 1 + 2 + 2)
#else
#define MQTT_MIN_BUFFER_SIZE (MQTT_MAX_HEADER_SIZE + 7 + 1 + 2 + 2)
#endif
/// \endcond

/// \anchor callback
/**
 * @brief Define the signature required by any callback function.
 * @param topic The topic of the message.
 * @param payload The payload of the message.
 * @param plength The length of the payload.
 * @note topic and payload point into the internal buffer: they are only valid until the callback returns and are
 * overwritten by any publish() / beginPublish() made inside the callback, so copy them first. loop() calls inside the callback are ignored.
 */
#if defined(__has_include) && __has_include(<functional>) && !defined(NOFUNCTIONAL)
#include <functional>
#define MQTT_CALLBACK_SIGNATURE std::function<void(char* topic, uint8_t* payload, size_t plength)> callback
#else
#define MQTT_CALLBACK_SIGNATURE void (*callback)(char* topic, uint8_t* payload, size_t plength)
#endif

/// \cond
#ifdef DEBUG_ESP_PORT
#ifdef DEBUG_PUBSUBCLIENT
#define DEBUG_PSC_PRINTF(fmt, ...) DEBUG_ESP_PORT.printf(("PubSubClient: " fmt), ##__VA_ARGS__)
#else
#define DEBUG_PSC_PRINTF(...)
#endif

#define ERROR_PSC_PRINTF(fmt, ...) DEBUG_ESP_PORT.printf(("PubSubClient error: " fmt), ##__VA_ARGS__)
#define ERROR_PSC_PRINTF_P(fmt, ...) DEBUG_ESP_PORT.printf_P(PSTR("PubSubClient error: " fmt), ##__VA_ARGS__)
#else  // DEBUG_ESP_PORT
#ifndef DEBUG_PSC_PRINTF
#define DEBUG_PSC_PRINTF(...)
#endif
#ifndef ERROR_PSC_PRINTF
#define ERROR_PSC_PRINTF(fmt, ...)
#endif
#ifndef ERROR_PSC_PRINTF_P
#define ERROR_PSC_PRINTF_P(fmt, ...)
#endif
#endif
/// \endcond

/**
 * @class PubSubClient
 * @brief This class provides a client for doing simple publish and subscribe messaging with a server that supports MQTT.
 */
class PubSubClient : public Print {
   private:
    Client* _client{};
    uint8_t* _buffer{};
    size_t _bufferSize{};
    size_t _bufferWritePos{};
    size_t _maxIncomingPacket{};  // max accepted incoming packet size; 0 = limit to _bufferSize (see setMaxIncomingPacket())
    // Non-blocking receive state (opt-in via setRxBufferSize()). When _rxBuffer is set and no Stream is used,
    // loop() reassembles incoming packets incrementally into this dedicated buffer across calls, so the main
    // loop never blocks waiting for bytes. The buffer is dedicated to RX, so outgoing traffic on _buffer
    // (publish/keepalive) can never corrupt an in-progress reassembly.
    uint8_t* _rxBuffer{};
    size_t _rxBufferSize{};
    size_t _maxBytesPerLoop{};         // max bytes processed per loop() in non-blocking mode; 0 = all available (see setMaxBytesPerLoop())
    uint8_t _rxState{};                // RxState: current state of the incremental receive FSM
    uint8_t _rxHdrLen{};               // number of remaining-length bytes of the packet being received
    uint8_t _oversizedPolicy{};        // MQTT_OVERSIZED_DISCONNECT (default) or MQTT_OVERSIZED_IGNORE (see setOversizedPolicy())
    uint32_t _rxMultiplier{};          // remaining-length varint multiplier carried across loop() calls
    size_t _rxLen{};                   // bytes written into _rxBuffer so far
    size_t _rxLength{};                // decoded remaining length of the packet being received
    size_t _rxRemaining{};             // payload bytes still to be received
    size_t _rxStreamSkip{};            // index within the remaining length at which the payload (streamed bytes) begins
    unsigned long _packetReadStart{};  // millis() when the current packet started, used as overall packet read deadline
    unsigned long _keepAliveMillis{};
    unsigned long _socketTimeoutMillis{};
    unsigned long _packetTimeoutMillis{};  // overall deadline for a single packet; 0 = fall back to _socketTimeoutMillis (see setPacketTimeout())
    uint16_t _nextMsgId{};
    uint8_t _rxFlags{};  // PUBLISH header low nibble (retain, QoS, DUP) + MQTT_RX_BUSY while in callback; placed here to use alignment padding
    unsigned long _lastOutActivity{};
    unsigned long _lastInActivity{};
    bool _pingOutstanding{};
    MQTT_CALLBACK_SIGNATURE{};
    IPAddress _ip{};
    char* _domain{};
    uint16_t _port{};
    Stream* _stream{};
    int _state{MQTT_DISCONNECTED};

    // States of the non-blocking incremental receive FSM (see processInbound()).
    // RX_PAYLOAD stores the packet into _rxBuffer and, when a Stream is set, also writes payload bytes to it.
    // RX_DISCARD drops the payload of an oversized packet incrementally (no Stream), keeping the connection in
    // sync (the documented "ignore oversized packet" behavior) without ever blocking the loop.
    enum RxState : uint8_t { RX_IDLE = 0, RX_HEADER, RX_PAYLOAD, RX_DISCARD };

    size_t readPacket(uint8_t* hdrLen);
    bool handlePacket(uint8_t* buf, size_t bufSize, uint8_t hdrLen, size_t length);
    bool processInbound();
    bool dispatchInbound();
    bool readByte(uint8_t* result);
    bool readByte(uint8_t* result, size_t* pos);
    uint8_t buildHeader(uint8_t header, size_t length);
    bool writeToClient(const uint8_t* buf, size_t size, bool packetStarted = false);
    bool writeControlPacket(uint8_t header, size_t length);
    size_t writeBuffer(size_t pos, size_t size, bool packetStarted = false);
    size_t writeStringImpl(bool progmem, const char* string, size_t pos);
    size_t writeString(const char* string, size_t pos);
    size_t writeNextMsgId(size_t pos);

    bool beginPublishImpl(bool progmem, const char* topic, size_t plength, uint8_t qos, bool retained);
    bool subscribeImpl(bool progmem, const char* topic, uint8_t qos);
    bool unsubscribeImpl(bool progmem, const char* topic);

    // Add to buffer and flush if full (only to be used with beginPublish/endPublish)
    size_t appendBuffer(uint8_t data);
    size_t flushBuffer();

   public:
    /**
     * @brief Creates an uninitialised client instance.
     * @note Before it can be used, it must be configured using the property setters setClient() and setServer().
     */
    PubSubClient();

    /**
     * @brief Creates a partially initialised client instance.
     * @param client The network client to use, for example WiFiClient.
     * @note Before it can be used, it must be configured with the property setter setServer().
     */
    PubSubClient(Client& client);

    /**
     * @brief Creates a fully configured client instance.
     * @param addr The address of the server.
     * @param port The port to connect to.
     * @param client The network client to use, for example WiFiClient.
     */
    PubSubClient(IPAddress addr, uint16_t port, Client& client);

    /**
     * @brief Creates a fully configured client instance.
     * @param addr The address of the server.
     * @param port The port to connect to.
     * @param client The network client to use, for example WiFiClient.
     * @param stream A stream to write received messages to.
     */
    PubSubClient(IPAddress addr, uint16_t port, Client& client, Stream& stream);

    /**
     * @brief Creates a fully configured client instance.
     * @param addr The address of the server.
     * @param port The port to connect to.
     * @param callback Pointer to a message \ref callback function.
     * Called when a message arrives for a subscription created by this client.
     * @param client The network client to use, for example WiFiClient.
     */
    PubSubClient(IPAddress addr, uint16_t port, MQTT_CALLBACK_SIGNATURE, Client& client);

    /**
     * @brief Creates a fully configured client instance.
     * @param addr The address of the server.
     * @param port The port to connect to.
     * @param callback Pointer to a message \ref callback function.
     * Called when a message arrives for a subscription created by this client.
     * @param client The network client to use, for example WiFiClient.
     * @param stream A stream to write received messages to.
     */
    PubSubClient(IPAddress addr, uint16_t port, MQTT_CALLBACK_SIGNATURE, Client& client, Stream& stream);

    /**
     * @brief Creates a fully configured client instance.
     * @param ip The address of the server.
     * @param port The port to connect to.
     * @param client The network client to use, for example WiFiClient.
     */
    PubSubClient(uint8_t* ip, uint16_t port, Client& client);

    /**
     * @brief Creates a fully configured client instance.
     * @param ip The address of the server.
     * @param port The port to connect to.
     * @param client The network client to use, for example WiFiClient.
     * @param stream A stream to write received messages to.
     */
    PubSubClient(uint8_t* ip, uint16_t port, Client& client, Stream& stream);

    /**
     * @brief Creates a fully configured client instance.
     * @param ip The address of the server.
     * @param port The port to connect to.
     * @param callback Pointer to a message \ref callback function.
     * Called when a message arrives for a subscription created by this client.
     * @param client The network client to use, for example WiFiClient.
     */
    PubSubClient(uint8_t* ip, uint16_t port, MQTT_CALLBACK_SIGNATURE, Client& client);

    /**
     * @brief Creates a fully configured client instance.
     * @param ip The address of the server.
     * @param port The port to connect to.
     * @param callback Pointer to a message \ref callback function.
     * Called when a message arrives for a subscription created by this client.
     * @param client The network client to use, for example WiFiClient.
     * @param stream A stream to write received messages to.
     */
    PubSubClient(uint8_t* ip, uint16_t port, MQTT_CALLBACK_SIGNATURE, Client& client, Stream& stream);

    /**
     * @brief Creates a fully configured client instance.
     * @param domain The address of the server.
     * @param port The port to connect to.
     * @param client The network client to use, for example WiFiClient.
     */
    PubSubClient(const char* domain, uint16_t port, Client& client);

    /**
     * @brief Creates a fully configured client instance.
     * @param domain The address of the server.
     * @param port The port to connect to.
     * @param client The network client to use, for example WiFiClient.
     * @param stream A stream to write received messages to.
     */
    PubSubClient(const char* domain, uint16_t port, Client& client, Stream& stream);

    /**
     * @brief Creates a fully configured client instance.
     * @param domain The address of the server.
     * @param port The port to connect to.
     * @param callback Pointer to a message \ref callback function.
     * Called when a message arrives for a subscription created by this client.
     * @param client The network client to use, for example WiFiClient.
     */
    PubSubClient(const char* domain, uint16_t port, MQTT_CALLBACK_SIGNATURE, Client& client);

    /**
     * @brief Creates a fully configured client instance.
     * @param domain The address of the server.
     * @param port The port to connect to.
     * @param callback Pointer to a message \ref callback function.
     * Called when a message arrives for a subscription created by this client.
     * @param client The network client to use, for example WiFiClient.
     * @param stream A stream to write received messages to.
     */
    PubSubClient(const char* domain, uint16_t port, MQTT_CALLBACK_SIGNATURE, Client& client, Stream& stream);

    /**
     * @brief Destructor for the PubSubClient class.
     */
    ~PubSubClient();

    /**
     * @brief Sets the server details.
     * @param ip The address of the server.
     * @param port The port to connect to.
     * @return The client instance, allowing the function to be chained.
     */
    PubSubClient& setServer(IPAddress ip, uint16_t port);

    /**
     * @brief Sets the server details.
     * @param ip The address of the server.
     * @param port The port to connect to.
     * @return The client instance, allowing the function to be chained.
     */
    PubSubClient& setServer(uint8_t* ip, uint16_t port);

    /**
     * @brief Sets the server details.
     * @param domain The address of the server.
     * @param port The port to connect to.
     * @return The client instance, allowing the function to be chained.
     */
    PubSubClient& setServer(const char* domain, uint16_t port);

    /**
     * @brief Sets the message callback function.
     * @param callback Pointer to a message \ref callback function.
     * Called when a message arrives for a subscription created by this client.
     * @return The client instance, allowing the function to be chained.
     */
    PubSubClient& setCallback(MQTT_CALLBACK_SIGNATURE);

    /**
     * @brief Sets the network client instance to use.
     * @param client The network client to use, for example WiFiClient.
     * @return The client instance, allowing the function to be chained.
     */
    PubSubClient& setClient(Client& client);

    /**
     * @brief Sets the stream to write received messages to.
     * @param stream A stream to write received messages to.
     * @return The client instance, allowing the function to be chained.
     */
    PubSubClient& setStream(Stream& stream);

    /**
     * @brief Sets the keep alive interval used by the client.
     * This value should only be changed when the client is not connected.
     * Set keepAlive to zero (0) to turn off the keep alive mechanism.
     * @param keepAlive The keep alive interval, in seconds.
     * @return The client instance, allowing the function to be chained.
     */
    PubSubClient& setKeepAlive(uint16_t keepAlive);

    /**
     * @brief Sets the socket timeout used by the client.
     * This determines how long the client will wait for incoming data when it expects data to arrive - for example,
     * whilst it is in the middle of reading a MQTT packet.
     * @param timeout The socket timeout, in seconds.
     * @return The client instance, allowing the function to be chained.
     */
    PubSubClient& setSocketTimeout(uint16_t timeout);

    /**
     * @brief Sets the overall deadline for receiving a single MQTT packet.
     * This is independent of setSocketTimeout(): the socket timeout bounds the wait for an individual byte
     * (and thus how long a single readPacket()/loop() call may block), while the packet timeout bounds the
     * time the whole packet may take to arrive. Decoupling them lets you keep a short socket timeout (so the
     * main loop stays responsive and offline functionality is not frozen) while still accepting a large
     * message that legitimately takes longer than the socket timeout to be received on a slow link.
     * @param timeout The packet timeout, in seconds, or 0 to fall back to the socket timeout (the default).
     * @return The client instance, allowing the function to be chained.
     */
    PubSubClient& setPacketTimeout(uint16_t timeout);

    /**
     * @brief Gets the configured packet timeout.
     * @return The configured packet timeout in seconds, or 0 if it falls back to the socket timeout.
     */
    uint16_t getPacketTimeout();

    /**
     * @brief Sets the size, in bytes, of the internal send and receive buffer.
     * This must be large enough to contain the full MQTT packet.
     * When sending or receiving messages, the packet will contain the full topic string,
     * the payload data, and a small number of header bytes.
     * @param size The size, in bytes, for the internal buffer.
     * Must be at least #MQTT_MIN_BUFFER_SIZE bytes to hold a minimal CONNECT packet.
     * @return true If the buffer was resized.
     * false If the buffer could not be resized (out of memory or size too small).
     * @note Changing the buffer size while the client is still connected may result in undefined behavior. It is recommended to change the buffer size
     * only when the client is disconnected.
     * @note The buffer size must be at least #MQTT_MIN_BUFFER_SIZE bytes. Otherwise this function will return false and the buffer size will remain
     * unchanged.
     * @note In case MQTT is not needed and to save memory, the buffer can be freed by setting the size to 0.
     * @note If not explicitly set/called, the buffer size will default to #MQTT_MAX_PACKET_SIZE bytes.
     */
    bool setBufferSize(size_t size);

    /**
     * @brief Gets the current size of the internal buffer.
     * @return The size of the internal buffer.
     */
    size_t getBufferSize();

    /**
     * @brief Sets the maximum size, in bytes, of an incoming MQTT packet the client will accept.
     * An incoming packet whose total size exceeds this limit is rejected up front in readPacket() and the
     * connection is closed, instead of draining (and discarding) up to 256 MB byte-by-byte, which would
     * starve the main loop and trigger a watchdog reset.
     * @param size The maximum incoming packet size in bytes, or 0 to limit incoming packets to the current
     * buffer size (getBufferSize()). The latter is the default and the safe choice when no Stream is set.
     * @return The client instance, allowing the function to be chained.
     * @note Only meaningful together with setStream(): without a Stream a packet larger than the buffer can
     * never be processed anyway, so the effective limit is min(size, bufferSize).
     */
    PubSubClient& setMaxIncomingPacket(size_t size);

    /**
     * @brief Gets the configured maximum incoming packet size.
     * @return The configured maximum incoming packet size in bytes, or 0 if it tracks the buffer size.
     */
    size_t getMaxIncomingPacket();

    /**
     * @brief Enables non-blocking receive by allocating a dedicated receive buffer, or disables it.
     * When a receive buffer is set, loop() reassembles incoming packets incrementally into it, consuming only
     * the bytes already available in the network stack and returning immediately — the main loop never blocks
     * waiting for bytes, so offline functionality stays responsive even when the broker/connection is slow or
     * stalls mid-packet. The receive buffer is separate from the send buffer, so an in-progress reassembly can
     * never be corrupted by an outgoing publish or keepalive.
     * @param size The receive buffer size in bytes. Must be at least #MQTT_MIN_BUFFER_SIZE and large enough to
     * hold the largest incoming message to be delivered to the callback (header + topic + payload). 0 frees the
     * buffer and returns to the classic blocking receive path.
     * @return true If the buffer was (re)allocated (or freed). false If allocation failed, the size was too
     * small, or a packet is currently being reassembled (resize/free is refused mid-packet).
     * @note Opt-in: if never called, the client keeps the classic blocking behavior and allocates no extra RAM.
     * Allocate once, early (before TLS), to avoid heap fragmentation. On allocation failure the previous state
     * is kept and the client stays usable (it falls back to the blocking path when no receive buffer exists).
     * @note Phase 1: the non-blocking path is used only when no Stream is set (see setStream()); with a Stream
     * the classic blocking path is used.
     */
    bool setRxBufferSize(size_t size);

    /**
     * @brief Gets the current size of the dedicated receive buffer.
     * @return The size of the receive buffer, or 0 if non-blocking receive is disabled.
     */
    size_t getRxBufferSize();

    /**
     * @brief Sets the maximum number of received bytes processed per loop() call in non-blocking mode.
     * This bounds how long a single loop() call spends on inbound MQTT traffic, which in turn bounds the
     * latency of other work done in the same loop (e.g. local/offline control functions). Remaining bytes are
     * processed on the following loop() calls.
     * @param maxBytes The per-call byte budget, or 0 to process all currently available bytes (the default).
     * @return The client instance, allowing the function to be chained.
     * @note Only effective when non-blocking receive is enabled via setRxBufferSize().
     */
    PubSubClient& setMaxBytesPerLoop(size_t maxBytes);

    /**
     * @brief Gets the configured per-loop received byte budget.
     * @return The per-loop byte budget, or 0 if all available bytes are processed each call.
     */
    size_t getMaxBytesPerLoop();

    /**
     * @brief Sets how an incoming packet that does not fit the receive buffer is handled.
     * @param policy #MQTT_OVERSIZED_DISCONNECT (default) to close the connection (fail-fast; a reconnect
     * resynchronises the stream, the robust choice on unreliable links), or #MQTT_OVERSIZED_IGNORE to drop the
     * packet and keep the connection (classic behavior). With #MQTT_OVERSIZED_IGNORE, a packet larger than
     * #MQTT_MAX_SANE_PACKET_SIZE is still treated as bogus and disconnects. Applies to both the blocking and the
     * non-blocking receive paths. A packet handled via a Stream (setStream()) is not considered oversized unless
     * it exceeds #MQTT_MAX_SANE_PACKET_SIZE.
     * @return The client instance, allowing the function to be chained.
     */
    PubSubClient& setOversizedPolicy(uint8_t policy);

    /**
     * @brief Gets the configured oversized-packet policy.
     * @return #MQTT_OVERSIZED_DISCONNECT or #MQTT_OVERSIZED_IGNORE.
     */
    uint8_t getOversizedPolicy();

    /**
     * @brief Connects the client using a clean session without username and password.
     * @param id The client ID to use when connecting to the server.
     * @return true If client succeeded in establishing a connection to the broker.
     * false If client failed to establish a connection to the broker.
     */
    inline bool connect(const char* id) {
        return connect(id, nullptr, nullptr, nullptr, MQTT_QOS0, false, nullptr, true);
    }

    /**
     * @brief Connects the client using a clean session with username and password.
     * @param id The client ID to use when connecting to the server.
     * @param user The username to use.
     * @param pass The password to use.
     * @note If **user** is NULL, no username or password is used.
     * @note If **pass** is NULL, no password is used.
     * @return true If client succeeded in establishing a connection to the broker.
     * false If client failed to establish a connection to the broker.
     */
    inline bool connect(const char* id, const char* user, const char* pass) {
        return connect(id, user, pass, nullptr, MQTT_QOS0, false, nullptr, true);
    }

    /**
     * @brief Connects the client using a clean session and will.
     * @param id The client ID to use when connecting to the server.
     * @param willTopic The topic to be used by the will message.
     * @param willQos The quality of service to be used by the will message. [0, 1, 2].
     * @param willRetain Publish the will message with the retain flag.
     * @param willMessage The message to be used by the will message.
     * @note If **willTopic** is NULL, no will message is sent.
     * @return true If client succeeded in establishing a connection to the broker.
     * false If client failed to establish a connection to the broker.
     */
    inline bool connect(const char* id, const char* willTopic, uint8_t willQos, bool willRetain, const char* willMessage) {
        return connect(id, nullptr, nullptr, willTopic, willQos, willRetain, willMessage, true);
    }

    /**
     * @brief Connects the client using a clean session with username, password and will.
     * @param id The client ID to use when connecting to the server.
     * @param user The username to use.
     * @param pass The password to use.
     * @param willTopic The topic to be used by the will message.
     * @param willQos The quality of service to be used by the will message. [0, 1, 2].
     * @param willRetain Publish the will message with the retain flag.
     * @param willMessage The message to be used by the will message.
     * @note If **user** is NULL, no username or password is used.
     * @note If **pass** is NULL, no password is used.
     * @note If **willTopic** is NULL, no will message is sent.
     * @return true If client succeeded in establishing a connection to the broker.
     * false If client failed to establish a connection to the broker.
     */
    inline bool connect(const char* id, const char* user, const char* pass, const char* willTopic, uint8_t willQos, bool willRetain,
                        const char* willMessage) {
        return connect(id, user, pass, willTopic, willQos, willRetain, willMessage, true);
    }

    /**
     * @brief Connects the client with all possible parameters (user, password, will and session).
     * @param id The client ID to use when connecting to the server.
     * @param user The username to use.
     * @param pass The password to use.
     * @param willTopic The topic to be used by the will message.
     * @param willQos The quality of service to be used by the will message. [0, 1, 2].
     * @param willRetain Publish the will message with the retain flag.
     * @param willMessage The message to be used by the will message.
     * @param cleanSession True to connect with a clean session.
     * @note If **user** is NULL, no username or password is used.
     * @note If **pass** is NULL, no password is used.
     * @note If **willTopic** is NULL, no will message is sent.
     * @return true If client succeeded in establishing a connection to the broker.
     * false If client failed to establish a connection to the broker.
     */
    bool connect(const char* id, const char* user, const char* pass, const char* willTopic, uint8_t willQos, bool willRetain, const char* willMessage,
                 bool cleanSession);

    /**
     * @brief Disconnects the client.
     */
    void disconnect();

    /**
     * @brief Publishes a non retained message to the specified topic using QoS 0.
     * @param topic The topic to publish to.
     * @param payload The message to publish.
     * @return true If the publish succeeded.
     * false If the publish failed, either connection lost or message too large.
     */
    inline bool publish(const char* topic, const char* payload) {
        return publish(topic, payload, MQTT_QOS0, false);
    }

    /**
     * @brief Publishes a message to the specified topic using QoS 0.
     * @param topic The topic to publish to.
     * @param payload The message to publish.
     * @param retained Publish the message with the retain flag.
     * @return true If the publish succeeded.
     * false If the publish failed, either connection lost or message too large.
     */
    inline bool publish(const char* topic, const char* payload, bool retained) {
        return publish(topic, payload, MQTT_QOS0, retained);
    }

    /**
     * @brief Publishes a message to the specified topic.
     * @param topic The topic to publish to.
     * @param payload The message to publish.
     * @param qos The quality of service (\ref group_qos) to publish at. [0, 1, 2].
     * @param retained Publish the message with the retain flag.
     * @return true If the publish succeeded.
     * false If the publish failed, either connection lost or message too large.
     */
    inline bool publish(const char* topic, const char* payload, uint8_t qos, bool retained) {
        return publish(topic, reinterpret_cast<const uint8_t*>(payload), payload ? strlen(payload) : 0, qos, retained);
    }

    /**
     * @brief Publishes a message to the specified topic.
     * @param topic The topic from __FlashStringHelper to publish to.
     * @param payload The message to publish.
     * @param qos The quality of service (\ref group_qos) to publish at. [0, 1, 2].
     * @param retained Publish the message with the retain flag.
     * @return true If the publish succeeded.
     * false If the publish failed, either connection lost or message too large.
     */
    inline bool publish(const __FlashStringHelper* topic, const char* payload, uint8_t qos, bool retained) {
        return publish(topic, reinterpret_cast<const uint8_t*>(payload), payload ? strlen(payload) : 0, qos, retained);
    }

    /**
     * @brief Publishes a message from __FlashStringHelper to the specified topic from __FlashStringHelper.
     * @param topic The topic to publish to.
     * @param payload The message to publish.
     * @param qos The quality of service (\ref group_qos) to publish at. [0, 1, 2].
     * @param retained Publish the message with the retain flag.
     * @return true If the publish succeeded.
     * false If the publish failed, either connection lost or message too large.
     */
    inline bool publish(const __FlashStringHelper* topic, const __FlashStringHelper* payload, uint8_t qos, bool retained) {
        return publish_P(topic, reinterpret_cast<const uint8_t*>(payload), payload ? strlen_P(reinterpret_cast<const char*>(payload)) : 0, qos, retained);
    }

    /**
     * @brief Publishes a non retained message to the specified topic using QoS 0.
     * @param topic The topic to publish to.
     * @param payload The message to publish.
     * @param plength The length of the payload.
     * @return true If the publish succeeded.
     * false If the publish failed, either connection lost or message too large.
     */
    inline bool publish(const char* topic, const uint8_t* payload, size_t plength) {
        return publish(topic, payload, plength, MQTT_QOS0, false);
    }

    /**
     * @brief Publishes a message to the specified topic using QoS 0.
     * @param topic The topic to publish to.
     * @param payload The message to publish.
     * @param plength The length of the payload.
     * @param retained Publish the message with the retain flag.
     * @return true If the publish succeeded.
     * false If the publish failed, either connection lost or message too large.
     */
    inline bool publish(const char* topic, const uint8_t* payload, size_t plength, bool retained) {
        return publish(topic, payload, plength, MQTT_QOS0, retained);
    }

    /**
     * @brief Publishes a message to the specified topic.
     * @param topic The topic to publish to.
     * @param payload The message to publish.
     * @param plength The length of the payload.
     * @param qos The quality of service (\ref group_qos) to publish at. [0, 1, 2].
     * @param retained Publish the message with the retain flag.
     * @return true If the publish succeeded.
     * false If the publish failed, either connection lost or message too large.
     */
    bool publish(const char* topic, const uint8_t* payload, size_t plength, uint8_t qos, bool retained);

    /**
     * @brief Publishes a message to the specified topic.
     * @param topic The topic from __FlashStringHelper to publish to.
     * @param payload The message to publish.
     * @param plength The length of the payload.
     * @param qos The quality of service (\ref group_qos) to publish at. [0, 1, 2].
     * @param retained Publish the message with the retain flag.
     * @return true If the publish succeeded.
     * false If the publish failed, either connection lost or message too large.
     */
    bool publish(const __FlashStringHelper* topic, const uint8_t* payload, size_t plength, uint8_t qos, bool retained);

    /**
     * @brief Publishes a message stored in PROGMEM to the specified topic using QoS 0.
     * @param topic The topic to publish to.
     * @param payload The message to publish.
     * @param retained Publish the message with the retain flag.
     * @return true If the publish succeeded.
     * false If the publish failed, either connection lost or message too large.
     */
    inline bool publish_P(const char* topic, PGM_P payload, bool retained) {
        return publish_P(topic, payload, MQTT_QOS0, retained);
    }

    /**
     * @brief Publishes a message stored in PROGMEM to the specified topic.
     * @param topic The topic to publish to.
     * @param payload The message to publish.
     * @param qos The quality of service (\ref group_qos) to publish at. [0, 1, 2].
     * @param retained Publish the message with the retain flag.
     * @return true If the publish succeeded.
     * false If the publish failed, either connection lost or message too large.
     */
    inline bool publish_P(const char* topic, PGM_P payload, uint8_t qos, bool retained) {
        return publish_P(topic, reinterpret_cast<const uint8_t*>(payload), payload ? strlen_P(payload) : 0, qos, retained);
    }

    /**
     * @brief Publishes a message stored in PROGMEM to the specified topic.
     * @param topic The topic from __FlashStringHelper to publish to.
     * @param payload The message to publish.
     * @param qos The quality of service (\ref group_qos) to publish at. [0, 1, 2].
     * @param retained Publish the message with the retain flag.
     * @return true If the publish succeeded.
     * false If the publish failed, either connection lost or message too large.
     */
    bool publish_P(const __FlashStringHelper* topic, PGM_P payload, uint8_t qos, bool retained) {
        return publish_P(topic, reinterpret_cast<const uint8_t*>(payload), payload ? strlen_P(payload) : 0, qos, retained);
    }

    /**
     * @brief Publishes a message stored in PROGMEM to the specified topic using QoS 0.
     * @param topic The topic to publish to.
     * @param payload The message to publish.
     * @param plength The length of the payload.
     * @param retained Publish the message with the retain flag.
     * @return true If the publish succeeded.
     * false If the publish failed, either connection lost or message too large.
     */
    inline bool publish_P(const char* topic, const uint8_t* payload, size_t plength, bool retained) {
        return publish_P(topic, payload, plength, MQTT_QOS0, retained);
    }

    /**
     * @brief Publishes a message stored in PROGMEM to the specified topic.
     * @param topic The topic to publish to.
     * @param payload The message to publish.
     * @param plength The length of the payload.
     * @param qos The quality of service (\ref group_qos) to publish at. [0, 1, 2].
     * @param retained Publish the message with the retain flag.
     * @return true If the publish succeeded.
     * false If the publish failed, either connection lost or message too large.
     */
    bool publish_P(const char* topic, const uint8_t* payload, size_t plength, uint8_t qos, bool retained);

    /**
     * @brief Publishes a message stored in PROGMEM to the specified topic.
     * @param topic The topic from __FlashStringHelper to publish to.
     * @param payload The message from PROGMEM to publish.
     * @param plength The length of the payload.
     * @param qos The quality of service (\ref group_qos) to publish at. [0, 1, 2].
     * @param retained Publish the message with the retain flag.
     * @return true If the publish succeeded.
     * false If the publish failed, either connection lost or message too large.
     */
    bool publish_P(const __FlashStringHelper* topic, const uint8_t* payload, size_t plength, uint8_t qos, bool retained);

    /**
     * @brief Start to publish a message using QoS 0.
     * This API:
     *   beginPublish(...)
     *   one or more calls to write(...)
     *   endPublish()
     * Allows for arbitrarily large payloads to be sent without them having to be copied into
     * a new buffer and held in memory at one time.
     * @param topic The topic to publish to.
     * @param plength The length of the payload.
     * @param retained Publish the message with the retain flag.
     * @return true If the publish succeeded.
     * false If the publish failed, either connection lost or message too large.
     */
    inline bool beginPublish(const char* topic, size_t plength, bool retained) {
        return beginPublishImpl(false, topic, plength, MQTT_QOS0, retained);
    }

    /**
     * @brief Start to publish a message.
     * This API:
     *   beginPublish(...)
     *   one or more calls to write(...)
     *   endPublish()
     * Allows for arbitrarily large payloads to be sent without them having to be copied into
     * a new buffer and held in memory at one time.
     * @param topic The topic to publish to.
     * @param plength The length of the payload.
     * @param qos The quality of service (\ref group_qos) to publish at. [0, 1, 2].
     * @param retained Publish the message with the retain flag.
     * @return true If the publish succeeded.
     * false If the publish failed, either connection lost or message too large.
     */
    inline bool beginPublish(const char* topic, size_t plength, uint8_t qos, bool retained) {
        return beginPublishImpl(false, topic, plength, qos, retained);
    }

    /**
     * @brief Start to publish a message using a topic from __FlashStringHelper F().
     * This API:
     *   beginPublish(...)
     *   one or more calls to write(...)
     *   endPublish()
     * Allows for arbitrarily large payloads to be sent without them having to be copied into
     * a new buffer and held in memory at one time.
     * @param topic The topic from __FlashStringHelper to publish to.
     * @param plength The length of the payload.
     * @param qos The quality of service (\ref group_qos) to publish at. [0, 1, 2].
     * @param retained Publish the message with the retain flag.
     * @return true If the publish succeeded.
     * false If the publish failed, either connection lost or message too large.
     */
    inline bool beginPublish(const __FlashStringHelper* topic, size_t plength, uint8_t qos, bool retained) {
        // convert FlashStringHelper in PROGMEM-pointer
        return beginPublishImpl(true, reinterpret_cast<const char*>(topic), plength, qos, retained);
    }

    /**
     * @brief Start to publish a message using a topic in PROGMEM.
     * This API:
     *   beginPublish_P(...)
     *   one or more calls to write(...)
     *   endPublish()
     * Allows for arbitrarily large payloads to be sent without them having to be copied into
     * a new buffer and held in memory at one time.
     * @param topic The topic in PROGMEM to publish to.
     * @param plength The length of the payload.
     * @param qos The quality of service (\ref group_qos) to publish at. [0, 1, 2].
     * @param retained Publish the message with the retain flag.
     * @return true If the publish succeeded.
     * false If the publish failed, either connection lost or message too large.
     */
    inline bool beginPublish_P(PGM_P topic, size_t plength, uint8_t qos, bool retained) {
        return beginPublishImpl(true, reinterpret_cast<const char*>(topic), plength, qos, retained);
    }

    /**
     * @brief Finish sending a message that was started with a call to beginPublish.
     * @return true If the publish succeeded.
     * false If the publish failed, either connection lost or message too large.
     */
    bool endPublish();

    /**
     * @brief Writes a single byte as a component of a publish started with a call to beginPublish.
     *        For performance reasons, this will be appended to the internal buffer,
     *        which will be flushed when full or on a call to endPublish().
     * @param data A byte to write to the publish payload.
     * @return The number of bytes written (0 or 1). If 0 is returned a write error occurred.
     */
    virtual size_t write(uint8_t data);

    /**
     * @brief Writes an array of bytes as a component of a publish started with a call to beginPublish.
     *        For performance reasons, this will be appended to the internal buffer,
     *        which will be flushed when full or on a call to endPublish().
     * @param buf The bytes to write.
     * @param size The length of the payload to be sent.
     * @return The number of bytes written. If return value is != size a write error occurred.
     */
    virtual size_t write(const uint8_t* buf, size_t size);

    /**
     * @brief Writes a string in PROGMEM as a component of a publish started with a call to beginPublish.
     *        For performance reasons, this will be appended to the internal buffer,
     *        which will be flushed when full or on a call to endPublish().
     * @param string The message to write.
     * @return The number of bytes written. If return value is != string length a write error occurred.
     */
    inline size_t write_P(PGM_P string) {
        return write_P(reinterpret_cast<const uint8_t*>(string), strlen_P(string));
    }

    /**
     * @brief Writes an array of progmem bytes as a component of a publish started with a call to beginPublish.
     *        For performance reasons, this will be appended to the internal buffer,
     *        which will be flushed when full or on a call to endPublish().
     * @param buf The bytes to write.
     * @param size The length of the payload to be sent.
     * @return The number of bytes written. If return value is != size a write error occurred.
     */
    size_t write_P(const uint8_t* buf, size_t size);

    /**
     * @brief Subscribes to messages published to the specified topic using QoS 0.
     * @param topic The topic to subscribe to.
     * @return true If sending the subscribe succeeded.
     * false If sending the subscribe failed, either connection lost or message too large.
     */
    inline bool subscribe(const char* topic) {
        return subscribeImpl(false, topic, MQTT_QOS0);
    }

    /**
     * @brief Subscribes to messages published to the specified topic from __FlashStringHelper using QoS 0.
     * @param topic The topic from __FlashStringHelper to subscribe to.
     * @return true If sending the subscribe succeeded.
     * false If sending the subscribe failed, either connection lost or message too large.
     */
    inline bool subscribe(const __FlashStringHelper* topic) {
        // convert FlashStringHelper in PROGMEM-pointer
        return subscribeImpl(true, reinterpret_cast<const char*>(topic), MQTT_QOS0);
    }

    /**
     * @brief Subscribes to messages published to the specified topic in PROGMEM using QoS 0.
     * @param topic The topic in PROGMEM to subscribe to.
     * @return true If sending the subscribe succeeded.
     * false If sending the subscribe failed, either connection lost or message too large.
     */
    inline bool subscribe_P(PGM_P topic) {
        return subscribeImpl(true, reinterpret_cast<const char*>(topic), MQTT_QOS0);
    }

    /**
     * @brief Subscribes to messages published to the specified topic.
     * @param topic The topic to subscribe to.
     * @param qos The qos to subscribe at. [0, 1].
     * @return true If sending the subscribe succeeded.
     * false If sending the subscribe failed, either connection lost or message too large.
     */
    inline bool subscribe(const char* topic, uint8_t qos) {
        return subscribeImpl(false, topic, qos);
    }

    /**
     * @brief Subscribes to messages published to the specified topic from __FlashStringHelper.
     * @param topic The topic from __FlashStringHelper to subscribe to.
     * @param qos The qos to subscribe at. [0, 1].
     * @return true If sending the subscribe succeeded.
     * false If sending the subscribe failed, either connection lost or message too large.
     */
    inline bool subscribe(const __FlashStringHelper* topic, uint8_t qos) {
        // convert FlashStringHelper in PROGMEM-pointer
        return subscribeImpl(true, reinterpret_cast<const char*>(topic), qos);
    }

    /**
     * @brief Subscribes to messages published to the specified topic in PROGMEM.
     * @param topic The topic in PROGMEM to subscribe to.
     * @param qos The qos to subscribe at. [0, 1].
     * @return true If sending the subscribe succeeded.
     * false If sending the subscribe failed, either connection lost or message too large.
     */
    inline bool subscribe_P(PGM_P topic, uint8_t qos) {
        return subscribeImpl(true, reinterpret_cast<const char*>(topic), qos);
    }

    /**
     * @brief Unsubscribes from the specified topic.
     * @param topic The topic to unsubscribe from.
     * @return true If sending the unsubscribe succeeded.
     * false If sending the unsubscribe failed, either connection lost or message too large.
     */
    inline bool unsubscribe(const char* topic) {
        return unsubscribeImpl(false, topic);
    }

    /**
     * @brief Unsubscribes from the specified topic from __FlashStringHelper.
     * @param topic The topic from __FlashStringHelper to unsubscribe from.
     * @return true If sending the unsubscribe succeeded.
     * false If sending the unsubscribe failed, either connection lost or message too large.
     */
    inline bool unsubscribe(const __FlashStringHelper* topic) {
        // convert FlashStringHelper in PROGMEM-pointer
        return unsubscribeImpl(true, reinterpret_cast<const char*>(topic));
    }

    /**
     * @brief Unsubscribes from the specified topic in PROGMEM.
     * @param topic The topic in PROGMEM to unsubscribe from.
     * @return true If sending the unsubscribe succeeded.
     * false If sending the unsubscribe failed, either connection lost or message too large.
     */
    inline bool unsubscribe_P(PGM_P topic) {
        return unsubscribeImpl(true, reinterpret_cast<const char*>(topic));
    }

    /**
     * @brief This should be called regularly to allow the client to process incoming messages and maintain its connection to the server.
     * @return true If the client is still connected.
     * false If the client is no longer connected.
     */
    bool loop();

    /**
     * @brief Checks whether the client is connected to the server.
     * @return true If the client is connected.
     * false If the client is not connected.
     */
    bool connected();

    /**
     * @brief Returns the current state of the client.
     * If a connection attempt fails, this can be used to get more information about the failure.
     * @note All of the values have corresponding constants defined in PubSubClient.h.
     * @return See \ref group_state
     */
    int state();

    /**
     * @brief Returns the QoS of the message currently delivered to the message \ref callback.
     * @note Only valid inside the callback; outside of it the value is reset to 0.
     * Not thread-safe: call it only from the context that runs loop(). Calling loop() from inside the callback is
     * ignored (it returns true without reading), as it would overwrite the topic and payload buffers.
     * @return The QoS level (0, 1 or 2) of the received message.
     */
    inline uint8_t getRxQos() const {
        return MQTT_HDR_GET_QOS(_rxFlags);
    }

    /**
     * @brief Returns whether the message currently delivered to the message \ref callback has the retain flag set.
     * @note Only valid inside the callback (see getRxQos() for reentrancy and thread-safety notes).
     * The flag is only set for retained messages sent by the server right after subscribing.
     * @return true if the message is a retained message.
     */
    inline bool getRxRetained() const {
        return _rxFlags & MQTTRETAINED;
    }

    /**
     * @brief Returns whether the message currently delivered to the message \ref callback has the DUP flag set (re-delivery).
     * @note Only valid inside the callback (see getRxQos() for reentrancy and thread-safety notes).
     * @return true if the message is a duplicate delivery attempt.
     */
    inline bool getRxDup() const {
        return _rxFlags & MQTT_RX_DUP;
    }
};

#endif
