# Arduino Client for MQTT

This library provides a client for doing simple publish/subscribe messaging with
a server that supports MQTT.

## Notes of this fork

This library descends from [knolleary/pubsubclient v2.8](https://github.com/knolleary/pubsubclient/releases/tag/v2.8) and the maintained fork [hmueller01/pubsubclient3](https://github.com/hmueller01/pubsubclient3).

Starting with **v4.0.0**, this repository (`voxiot/pubsubclient3`) **diverges** from upstream. It is maintained for the Voxiot product line and focuses on robust, watchdog-safe, 24/7 operation on unreliable connectivity (flaky WiFi, Internet and broker) on resource-constrained targets (ESP8266 / ESP32). The receive path was reworked so that MQTT never freezes the main loop — important when the device must keep running local/offline functions while the broker is slow or unreachable.

> [!IMPORTANT]
> **Diverged fork.** Based on upstream `hmueller01/pubsubclient3 v3.3.2`, independently versioned from **v4.0.0** onward.
>
> We no longer track upstream version-for-version. Upstream changes are reviewed individually and cherry-picked only when they make sense for this fork. The public API remains backward compatible. Optional v4 resilience features are **opt-in**; the oversized-packet default and outgoing-write safety fixes apply automatically (see below).

## Resilience and non-blocking operation (v4)

Optional features below preserve the classic defaults. An oversized incoming packet disconnects by default (see `setOversizedPolicy`), and outgoing-write safety fixes apply automatically, including the packet-level failure handling in v4.0.2.

- **Non-blocking receive (opt-in).** By default the receive path is synchronous: a single `loop()` call can block while a packet is being read. For firmware that must stay responsive (e.g. a button that opens a gate from local memory while the broker is down), enable incremental receive with a dedicated buffer:

  ```c
  client.setRxBufferSize(512);     // dedicated RX buffer; enables non-blocking receive
  client.setMaxBytesPerLoop(256);  // optional: cap bytes processed per loop() to bound latency
  ```

  `loop()` then consumes only the bytes already available and returns, reassembling packets across calls. The RX buffer is separate from the send buffer, so an in-progress receive is never corrupted by an outgoing publish or keepalive. Allocate it once, early (before TLS), to avoid heap fragmentation; on allocation failure the client keeps working on the classic blocking path. Works with `setStream()` too (payload is streamed incrementally).

- **Packet read deadline, decoupled from the socket timeout.** `setSocketTimeout()` bounds how long the client waits for a single byte; `setPacketTimeout()` bounds the time a whole packet may take to arrive. Keeping a short socket timeout keeps the loop responsive, while a separate packet timeout still allows a large message to arrive on a slow link. A partially received packet that stalls is aborted (never blocks).

  ```c
  client.setSocketTimeout(2);   // short: bounds per-byte wait / loop responsiveness
  client.setPacketTimeout(10);  // 0 (default) = fall back to the socket timeout
  ```

- **Oversized-packet policy.** When an incoming packet does not fit the buffer:

  ```c
  client.setOversizedPolicy(MQTT_OVERSIZED_DISCONNECT); // default (v4): fail-fast, reconnect resynchronises
  client.setOversizedPolicy(MQTT_OVERSIZED_IGNORE);     // classic: drop the packet, keep the connection
  ```

  On unreliable links an oversized remaining-length is more often corruption/desync than a genuinely large message, and only a reconnect can resynchronise a broken stream — hence the fail-fast default. A packet larger than `MQTT_MAX_SANE_PACKET_SIZE` (platform-aware: 16 KiB on ESP8266, 64 KiB on ESP32, protocol max otherwise; override with `-D`) is always treated as bogus and disconnects, regardless of policy.

- **Received-message metadata.** Inside the callback, `getRxQos()`, `getRxRetained()` and `getRxDup()` expose the QoS, retain and DUP flags of the current message.

See [CHANGELOG.md](CHANGELOG.md) for the full list of v4 changes and fixes.

### Outgoing PUBLISH failures: before and after v4.0.2

A PUBLISH can require several network writes: `beginPublish()` sends the MQTT header, topic and optional packet identifier; `write()` / `write_P()` buffer the payload; buffer flushes and `endPublish()` send the remaining bytes. The header announces the size of the complete packet before the payload is sent.

**Before v4.0.2**, the decision to keep or close the connection considered only the current network-write call. If the header had already been sent but a later payload write accepted zero bytes until its timeout, the library returned a failure and discarded the buffered payload while keeping the connection open. The broker was still waiting for that payload and could consume the next MQTT packet as its continuation, corrupting packet boundaries and eventually reporting a malformed packet. The same gap existed between complete transfer chunks when `MQTT_MAX_TRANSFER_SIZE` was enabled.

**Starting with v4.0.2**, the write path also knows whether an earlier part of the same MQTT packet has already been sent:

| Write outcome | Before v4.0.2 | v4.0.2 |
| --- | --- | --- |
| No byte of the entire packet is sent; transport remains connected | Return failure and keep the connection | Unchanged: return failure and keep the connection |
| Header is sent; the payload write sends zero bytes and cannot complete | Return failure, but leave an incomplete packet on an open connection | Return failure and close the connection with `MQTT_CONNECTION_LOST` |
| A complete transfer chunk is sent; the next chunk sends zero bytes and cannot complete | Could keep the connection despite an incomplete packet | Close the connection with `MQTT_CONNECTION_LOST` |
| A write sends some bytes, then cannot complete | Close the connection | Unchanged: close the connection |
| Short writes eventually complete within the existing timeout | Complete the write and keep the connection | Unchanged |

For example, `publish("topic", "payload")` at QoS 0 sends a nine-byte header/topic prefix followed by seven payload bytes. If all nine prefix bytes are accepted and none of the payload can be sent, v4.0.2 closes the connection. Another publication cannot be appended to that incomplete packet; the application must reconnect before publishing again. Pending payload data is discarded rather than reused after reconnection. This does not add automatic message retransmission or prove broker delivery.

If the first transport write accepts zero bytes and no part of the packet was sent, the operation returns failure immediately without retrying until the socket timeout. The application can retry on a later loop. This avoids an extra library retry loop; the underlying `Client::write()` call can still block according to its own transport behavior. Writes that continue an already-started packet retain the existing retry timeout and disconnect on an incomplete send.

The fix covers `publish()`, `publish_P()`, the `beginPublish()` / `write()` / `endPublish()` flow, QoS 0/1/2, payloads larger than the buffer, and chunked writes. It preserves the public API and existing `setSocketTimeout()` retry deadline, and adds no per-instance fields, buffers or dynamic allocations.

`setOversizedPolicy()` controls **incoming oversized packets only**. It does not control outgoing transmission failures: once a packet has started, abandoning its remaining bytes requires closing the connection to restore framing on a new connection.

Regression coverage is in `tests/src/publish_spec.cpp`, including zero-byte payload failures after a complete header, failures between payload buffers and transfer chunks, blocked subsequent publishes, and clean reconnection. The chunk-specific case is enabled when the test executable and library are compiled with `MQTT_MAX_TRANSFER_SIZE` defined (for example, `4`).

## Examples

The library comes with a number of example sketches. See File > Examples > PubSubClient
within the Arduino application.

Full API documentation is available here: https://hmueller01.github.io/pubsubclient3/api

## Limitations

 - The client is based on the [MQTT Version 3.1.1 specification](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) with some limitations.
 - It can publish at QoS 0 and since [v3.2.0](https://github.com/hmueller01/pubsubclient3/releases/tag/v3.2.0) also at QoS 1 or 2.

   **WARNING:** No retransmission is supported to keep the library as much memory friendly as possible.

   Note: Without retransmission support, the publish QoS is only meaningful when the broker sends your
   message to a subscriber, supposing that the subscriber subscribes with a QoS greater then or equal to
   the publish QoS. Consider that MQTT runs over TCP, so retransmission isn't really required in most cases, especially when publishing to the broker.
 - It can subscribe at QoS 0 or QoS 1.
 - The maximum message size, including header, is **256 bytes** by default. This
   is configurable via `MQTT_MAX_PACKET_SIZE` in `PubSubClient.h` or can be changed
   by calling `PubSubClient::setBufferSize(size)`. An incoming packet larger than the
   buffer is, by default (v4), handled by closing the connection (see
   `setOversizedPolicy()` to restore the classic ignore-and-stay-connected behavior).
 - The keepalive interval is set to 15 seconds by default. This is configurable
   via `MQTT_KEEPALIVE` in `PubSubClient.h` or can be changed by calling
   `PubSubClient::setKeepAlive(keepAlive)`.
 - The client uses MQTT 3.1.1 by default. It can be changed to use MQTT 3.1 by
   changing value of `MQTT_VERSION` in `PubSubClient.h`.
 - Since [v3.3.0](https://github.com/hmueller01/pubsubclient3/releases/tag/v3.3.0) it can publish and subscribe to `PROGMEM` or `__FlashStringHelper` topics.
   Details see [mqtt_progmem](https://github.com/hmueller01/pubsubclient3/blob/aae84e4d1aa65e752e19e30239b5796b4fe2705b/examples/mqtt_progmem/src/mqtt_progmem.cpp#L39-L48) example.
   But if you like to publish `PROGMEM` topics you have to use
   ```c
   const char TOPIC[] PROGMEM = "test";
   const char HELLO_WORLD[] PROGMEM = "hello world";
   client.beginPublish_P(TOPIC, strlen_P(HELLO_WORLD), MQTT_QOS0, false);
   client.write_P(HELLO_WORLD);
   client.endPublish();
   ```
   as `client.publish_P(...)` is already used for `PROGMEM` payloads.

## Compatible Hardware

The library uses the Arduino Ethernet Client api for interacting with the
underlying network hardware. This means it Just Works with a growing number of
boards and shields, including:

 - Arduino Ethernet
 - Arduino Ethernet Shield
 - Arduino YUN – use the included `YunClient` in place of `EthernetClient`, and
   be sure to do a `Bridge.begin()` first
 - Arduino WiFi Shield - if you want to send packets > 90 bytes with this shield,
   enable the `MQTT_MAX_TRANSFER_SIZE` define in `PubSubClient.h`.
 - Sparkfun WiFly Shield – [library](https://github.com/dpslwk/WiFly)
 - TI CC3000 WiFi - [library](https://github.com/sparkfun/SFE_CC3000_Library)
 - Intel Galileo/Edison
 - ESP8266
 - ESP32

The library cannot currently be used with hardware based on the ENC28J60 chip –
such as the Nanode or the Nuelectronics Ethernet Shield. For those, there is an
[alternative library](https://github.com/njh/NanodeMQTT) available.

## Changelog

See [CHANGELOG.md](CHANGELOG.md)

## License

This code is released under the [MIT License](LICENSE.txt).
