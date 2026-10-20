# Arduino Client for MQTT

**English** | [Português (Brasil)](README.pt-BR.md)

This library provides a client for doing simple publish/subscribe messaging with
a server that supports MQTT.

## Notes of this fork

This library descends from [knolleary/pubsubclient v2.8](https://github.com/knolleary/pubsubclient/releases/tag/v2.8) and the maintained fork [hmueller01/pubsubclient3](https://github.com/hmueller01/pubsubclient3).

Starting with **v4.0.0**, this repository (`voxiot/pubsubclient3`) **diverges** from upstream. It is maintained for the Voxiot product line and focuses on robust, watchdog-safe, 24/7 operation on unreliable connectivity (flaky WiFi, Internet and broker) on resource-constrained targets (ESP8266 / ESP32). The receive path was reworked so that MQTT never freezes the main loop — important when the device must keep running local/offline functions while the broker is slow or unreachable.

> [!IMPORTANT]
> **Diverged fork.** Based on upstream `hmueller01/pubsubclient3 v3.3.2`, independently versioned from **v4.0.0** onward.
>
> We no longer track upstream version-for-version. Upstream changes are reviewed individually and cherry-picked only when they make sense for this fork. Existing method signatures are preserved; copying an owning PubSubClient is now explicitly disabled (see the v5.0.0 safety fixes below). Optional v4 resilience features are **opt-in**; the oversized-packet default and outgoing-write safety fixes apply automatically (see below).

## Why version 5.0.0? Migration from v4

Version **5.0.0** is a major release because the packet and session integrity fixes intentionally break two previously accepted usage patterns. Although existing method signatures are preserved, this is not a fully backward-compatible patch:

- **Client copying is prohibited.** Copy construction and copy assignment are deleted because they duplicated owning pointers and could cause double frees. Pass `PubSubClient` by reference or pointer; create and configure a separate instance when an independent client is needed.
- **Manual publishing is an explicit packet transaction.** Every successful `beginPublish()` must be followed by exactly the announced number of payload bytes and `endPublish()`, including for an empty payload. `write()` outside a transaction returns zero; a write exceeding the remaining payload is rejected in full. Ending with missing bytes closes the connection with `MQTT_CONNECTION_LOST`.

While a manual publish is active, `loop()` checks the connection but defers receive and keepalive processing; nested publishes, subscribe/unsubscribe, reconnect and buffer changes are refused. Finish the transaction promptly, and finish a manual publish started inside a callback before returning. Buffer changes and reconnect are also refused for the callback's lifetime. These restrictions prevent other MQTT packets from becoming bytes of an unfinished payload and prevent access to invalidated buffers.

Applications that already keep clients by reference/pointer and complete each manual publish normally require no changes for these two migration points. The release also resets parser state across connections, validates lengths and identifiers, and tracks incoming QoS 2 duplicates. These fixes apply to both blocking and incremental receive; incremental receive remains opt-in. See [CHANGELOG.md](CHANGELOG.md) and the v5 safety details below.

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

  The socket timeout defaults to **15 seconds**. `setSocketTimeout(2)` selects two seconds; `setSocketTimeout(0)` restores `MQTT_SOCKET_TIMEOUT`, including any compile-time override. Unlike earlier versions, zero no longer selects immediate expiry and never means an infinite wait. `MQTT_SOCKET_TIMEOUT` must be between 1 and 65535 seconds, checked by `static_assert`. `setPacketTimeout(0)` still falls back to the effective socket timeout. These are library deadlines, not a guarantee that underlying TCP/TLS calls cannot block longer. Wait loops yield to the platform; a longer timeout can still delay application work even when the watchdog is serviced.

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

### Session and packet safety (v5.0.0)

These fixes apply to blocking and incremental receive. They do not change the application's network/OFFLINE policy or the existing socket timeout.

| Case | Before | Now |
| --- | --- | --- |
| Callback disconnects and frees/resizes buffers | A later ACK could access freed memory | Buffer changes and reconnect are refused during the callback; ACK uses local bytes and is skipped after disconnect |
| Copying a client | Copies shared owning pointers; double free on destruction | Copy construction and assignment are deleted; pass clients by reference/pointer |
| Blocking receive stops mid-packet | Returned with an open, desynchronised socket | Closes with `MQTT_CONNECTION_TIMEOUT`; the next connection starts at a packet boundary |
| Reconnect after fragmented receive | Old header/payload/discard state could consume the new session | All parser state is reset on close and before connect |
| Malformed QoS 1/2 packet lacks an identifier | Callback-busy flag could remain set forever | Validates the complete, nonzero identifier before entering the callback; guard is cleared on callback exit |
| Manual PUBLISH overlaps `loop()` or another operation | ACK, ping or another header could become payload bytes | Tracks the active packet and remaining payload; defers RX/keepalive until `endPublish()` |
| PUBLISH length arithmetic overflows | Wrapped length could produce a malformed packet | Validates by subtraction before adding; rejects lengths outside protocol/buffer bounds |
| IGNORE limit smaller than the blocking buffer | Packet could still reach the callback | Drains and drops it using the configured limit, matching incremental receive |
| Duplicate incoming QoS 2 PUBLISH | Callback could run again before PUBREL | Tracks pending identifiers, repeats PUBREC without repeating the callback, releases on PUBREL |
| Empty or over-65535-byte topics | Could emit malformed SUBSCRIBE/UNSUBSCRIBE/PUBLISH | Rejects without sending; CONNECT strings and private string writes have checked limits too |

`beginPublish()` / `write()` / `endPublish()` is one synchronous packet transaction. Finish it promptly: while active, `loop()` only checks the connection and defers protocol processing. Nested publishes, subscribe/unsubscribe, reconnect and buffer changes are refused. `write()` outside this transaction returns zero. A write larger than the remaining payload is rejected in full (zero returned); smaller writes can still complete it. `endPublish()` with missing bytes returns false and closes with `MQTT_CONNECTION_LOST`. `disconnect()` during an incomplete transaction closes the socket directly, without inserting a DISCONNECT into the payload. Even a zero-length payload requires `endPublish()`.

A callback may publish synchronously or disconnect. It must finish any manual publish before returning; otherwise the connection closes rather than inserting an ACK into its payload. `setBufferSize()` and `setRxBufferSize()` return false while a callback or manual publish is active, including after a callback calls `disconnect()`. Endpoint/transport/stream changes are ignored during those operations. Topic/payload pointers remain transient: a publish inside the callback may overwrite them, so copy needed data first. The client remains single-threaded; these guards do not provide thread synchronisation.

Incoming QoS 2 tracking uses `MQTT_RX_QOS2_MAX_INFLIGHT` fixed `uint16_t` slots (default **1**, two bytes per slot), with no retained payload. Define this macro consistently for the library and application to support more concurrent incoming handshakes. If every slot is occupied, the client closes before delivering or acknowledging a new identifier; it never evicts an unfinished handshake. This does not add outgoing QoS retransmission support. Stream output is raw receive data and may contain retransmitted payload; duplicate suppression applies to the message callback.

For `cleanSession=false`, the library also owns a copy of the client ID (`strlen(id)+1` bytes, allocated at connection setup, not per message) to distinguish persistent sessions. Pending identifiers survive a transport loss only for the same client ID and configured endpoint/transport, and are retained after reconnect only if MQTT 3.1.1 CONNACK confirms Session Present. A clean/new session, changed client ID, broker or transport clears them. Reapplying the same endpoint/transport preserves them. MQTT 3.1 cannot confirm session continuity, so tracking resets on successful reconnect. State is in RAM and is lost on reboot/destruction; this is not durable exactly-once application delivery. Default clean sessions allocate no client-ID copy.

The initial-zero-write fast failure and retry deadline for an already-started packet remain unchanged. `setOversizedPolicy(MQTT_OVERSIZED_IGNORE)` still preserves a correctly drained connection; incomplete packets and failed outgoing writes must close it to restore framing.

Regression coverage: `tests/src/resilience_spec.cpp`, executed by `make test`, covers the ten fixes in both receive modes where applicable, wire ordering, reconnection, persistent-session identity and QoS 2 capacity. The mock transport uses `size_t` counters for array traversal so a legal 65535-byte topic cannot wrap its write loop. Run the suite again with `MQTT_MAX_TRANSFER_SIZE=4` and with AddressSanitizer for chunk boundaries and memory lifetime checks.

### Control responses and configuration checks

PUBACK, PUBREC, PUBREL, PUBCOMP and PINGRESP share a required-response helper. With classic blocking receive, recoverable short writes are completed within the socket retry timeout. With incremental receive enabled by `setRxBufferSize()`, each automatic response makes exactly one transport write attempt: a partial write closes immediately with `MQTT_CONNECTION_LOST`, without retries or a retry-loop yield. In both modes, zero bytes on the first attempt closes the connection because these required replies are not queued for later.

For incoming QoS 1, the callback can run before PUBACK transmission fails, including a zero-byte write. The broker may redeliver the unacknowledged PUBLISH after reconnecting, notably when resuming a persistent session. Incoming QoS 1 has no duplicate suppression here: callback redelivery is expected under at-least-once delivery, so make application side effects idempotent when needed.

PINGREQ follows the same mode-dependent retry policy, but zero bytes keeps the connection and defers the ping to a later `loop()`. `_pingOutstanding` is set only after a complete send. A partial incremental-mode PINGREQ closes immediately, and RX is not processed after that failure.

#### Timing policy and remaining tradeoff

Incremental receive prioritizes responsiveness of automatic protocol writes over recovery from transient short writes. This can cause more reconnects on temporary TX congestion, but an incomplete packet is never left on an open connection. Blocking receive keeps its existing retry behavior. No pending-TX queue, heap allocation or per-client state is added. Private helper calls can affect code size and stack usage; those costs depend on the compiler.

This policy removes library retries from these automatic writes; it does not make every `loop()` call or network operation non-blocking. The first `Client::write()` call can still block internally. CONNECT, SUBSCRIBE, UNSUBSCRIBE, `publish()`, `publish_P()` and manual publishing keep their synchronous retry behavior even when incremental receive is enabled; a publish inside a callback can therefore delay `loop()`. In blocking mode, retries yield to platform tasks, but application work is delayed until the call returns. Servicing the watchdog does not guarantee response deadlines for local inputs or outputs. Validate latency under congestion and network faults on the target hardware against the product's required deadlines.

`make test` also runs `tests/check_config.py` to check accepted and rejected compile-time limits; it uses Python 3's standard library and the configured C++ compiler. Run it separately from `tests/` with `make config-test` or `python3 check_config.py --compiler g++`. Runtime regressions are in `resilience_spec.cpp`, including both receive modes, exact response bytes, single-attempt call counts, PINGREQ deferral and acknowledgement, no RX dispatch after a failed ping, synchronous publish retries, timeout fallback and QoS 2 slot reuse. A scoped host-only clock exercises deadlines without long real waits. The keepalive suite also advances simulated seconds explicitly so host scheduling cannot shift expected ping times; other suites retain their existing clock behavior. Repeat the suite with `MQTT_RX_QOS2_MAX_INFLIGHT=4`, `MQTT_SOCKET_TIMEOUT=1`, and `MQTT_MAX_TRANSFER_SIZE=4` (each definition must also apply to the library).

## Examples

The library comes with a number of example sketches. See File > Examples > PubSubClient
within the Arduino application.

Full API documentation is available here: https://hmueller01.github.io/pubsubclient3/api

## Limitations

 - The client is based on the [MQTT Version 3.1.1 specification](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) with some limitations.
 - Incoming QoS 2 tracking defaults to **one pending identifier** (`MQTT_RX_QOS2_MAX_INFLIGHT=1`). A new distinct identifier arriving while all slots are occupied closes the connection before callback delivery or PUBREC; a duplicate does not consume another slot. PUBREL releases its slot. If concurrent QoS 2 deliveries are expected, define `MQTT_RX_QOS2_MAX_INFLIGHT=4`, for example, consistently for library and application. The identifier array costs `2 * N` bytes per client (four slots add six bytes over the default). A `static_assert` accepts 1 through 65535 slots; this protocol bound does not guarantee that a chosen array fits device RAM.
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
