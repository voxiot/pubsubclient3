#include "BDDTest.h"
#include "Buffer.h"
#include "PubSubClient.h"
#include "ShimClient.h"
#include "Stream.h"
#include "trace.h"

#if MQTT_ENABLE_INCREMENTAL_RX
// Non-blocking incremental receive (setRxBufferSize()): verify that a packet split across many loop() calls
// (one or a few bytes available at a time) is reassembled correctly without blocking, that the per-loop byte
// budget is honored, and that an oversized packet is dropped incrementally while the connection stays in sync.

byte server[] = {172, 16, 0, 2};

static bool callback_called = false;
static char lastTopic[1024];
static char lastPayload[1024];
static size_t lastLength = 0;

// function declarations
void callback(char* topic, uint8_t* payload, size_t plength);
static void reset_callback();
static int test_fragmented_receive();
static int test_max_bytes_per_loop();
static int test_fragmented_oversized_discard();
static int test_fragmented_qos1();
static int test_fragmented_stream();
static int test_oversized_disconnect_default();
static int test_bogus_length_disconnect();

static void reset_callback() {
    callback_called = false;
    lastTopic[0] = '\0';
    lastPayload[0] = '\0';
    lastLength = 0;
}

void callback(char* topic, uint8_t* payload, size_t plength) {
    TRACE("Callback received topic=[" << topic << "] plength=" << plength << "\n")
    callback_called = true;
    strcpy(lastTopic, topic);
    memcpy(lastPayload, payload, plength);
    lastLength = plength;
}

// Establish a connected client with non-blocking receive enabled. The CONNACK is consumed by connect()
// (which still uses the blocking path); after this the response buffer is empty.
static bool setup_connected(ShimClient& shimClient, PubSubClient& client, size_t rxBufferSize) {
    shimClient.setAllowConnect(true);
    byte connack[] = {0x20, 0x02, 0x00, 0x00};
    shimClient.respond(connack, sizeof(connack));
    if (!client.setRxBufferSize(rxBufferSize)) return false;
    return client.connect("fragment_test");
}

static int test_fragmented_receive() {
    IT("reassembles a PUBLISH delivered one byte per loop()");
    reset_callback();

    ShimClient shimClient;
    PubSubClient client(server, 1883, callback, shimClient);
    IS_TRUE(setup_connected(shimClient, client, 64));

    // PUBLISH topic="topic" payload="payload", QoS0. Remaining length = 2 + 5 + 7 = 14 (0x0E).
    byte publish[] = {0x30, 0x0E, 0x00, 0x05, 't', 'o', 'p', 'i', 'c', 'p', 'a', 'y', 'l', 'o', 'a', 'd'};

    for (size_t i = 0; i < sizeof(publish); i++) {
        IS_FALSE(callback_called);  // callback must not fire before the last byte
        shimClient.respond(&publish[i], 1);
        IS_TRUE(client.loop());  // never blocks, stays connected
    }

    IS_TRUE(callback_called);
    IS_TRUE(strcmp(lastTopic, "topic") == 0);
    IS_TRUE(lastLength == 7);
    IS_TRUE(memcmp(lastPayload, "payload", 7) == 0);
    IS_FALSE(shimClient.error());
    IS_TRUE(client.connected());

    END_IT
}

static int test_max_bytes_per_loop() {
    IT("honors setMaxBytesPerLoop() (one byte per loop)");
    reset_callback();

    ShimClient shimClient;
    PubSubClient client(server, 1883, callback, shimClient);
    IS_TRUE(setup_connected(shimClient, client, 64));
    client.setMaxBytesPerLoop(1);

    byte publish[] = {0x30, 0x0E, 0x00, 0x05, 't', 'o', 'p', 'i', 'c', 'p', 'a', 'y', 'l', 'o', 'a', 'd'};
    shimClient.respond(publish, sizeof(publish));  // whole packet available at once

    // With a budget of 1 byte per call, the callback must only fire after sizeof(publish) loop() calls
    for (size_t i = 0; i < sizeof(publish) - 1; i++) {
        IS_TRUE(client.loop());
        IS_FALSE(callback_called);
    }
    IS_TRUE(client.loop());  // consumes the final byte
    IS_TRUE(callback_called);
    IS_TRUE(strcmp(lastTopic, "topic") == 0);
    IS_TRUE(memcmp(lastPayload, "payload", 7) == 0);
    IS_FALSE(shimClient.error());

    END_IT
}

static int test_fragmented_oversized_discard() {
    IT("drops an oversized packet incrementally and stays in sync (ignore policy)");
    reset_callback();

    ShimClient shimClient;
    PubSubClient client(server, 1883, callback, shimClient);
    IS_TRUE(setup_connected(shimClient, client, 20));  // small RX buffer
    client.setOversizedPolicy(MQTT_OVERSIZED_IGNORE);

    // Oversized PUBLISH: remaining length 2 + 5 + 16 = 23 (0x17), total 25 > 20 -> must be ignored
    byte big[25];
    big[0] = 0x30;
    big[1] = 0x17;
    big[2] = 0x00;
    big[3] = 0x05;
    memcpy(&big[4], "topic", 5);
    memset(&big[9], 'A', 16);
    for (size_t i = 0; i < sizeof(big); i++) {
        shimClient.respond(&big[i], 1);
        IS_TRUE(client.loop());  // never blocks, never disconnects
    }
    IS_FALSE(callback_called);    // oversized packet ignored
    IS_TRUE(client.connected());  // connection kept

    // A following well-sized PUBLISH must still be received: the stream stayed in sync through the discard
    byte publish[] = {0x30, 0x0E, 0x00, 0x05, 't', 'o', 'p', 'i', 'c', 'p', 'a', 'y', 'l', 'o', 'a', 'd'};
    for (size_t i = 0; i < sizeof(publish); i++) {
        shimClient.respond(&publish[i], 1);
        IS_TRUE(client.loop());
    }
    IS_TRUE(callback_called);
    IS_TRUE(strcmp(lastTopic, "topic") == 0);
    IS_TRUE(memcmp(lastPayload, "payload", 7) == 0);
    IS_FALSE(shimClient.error());

    END_IT
}

static int test_fragmented_qos1() {
    IT("reassembles a fragmented QoS 1 PUBLISH and sends PUBACK");
    reset_callback();

    ShimClient shimClient;
    PubSubClient client(server, 1883, callback, shimClient);
    IS_TRUE(setup_connected(shimClient, client, 64));

    // QoS1 PUBLISH (header 0x32): remaining length = 2 (topic len) + 5 (topic) + 2 (msgId) + 2 (payload) = 11 (0x0B)
    byte publish[] = {0x32, 0x0B, 0x00, 0x05, 't', 'o', 'p', 'i', 'c', 0x12, 0x34, 'h', 'i'};
    // Expect a PUBACK with the same msgId 0x1234
    byte puback[] = {0x40, 0x02, 0x12, 0x34};
    shimClient.expect(puback, sizeof(puback));

    for (size_t i = 0; i < sizeof(publish); i++) {
        shimClient.respond(&publish[i], 1);
        IS_TRUE(client.loop());
    }

    IS_TRUE(callback_called);
    IS_TRUE(strcmp(lastTopic, "topic") == 0);
    IS_TRUE(lastLength == 2);
    IS_TRUE(memcmp(lastPayload, "hi", 2) == 0);
    IS_FALSE(shimClient.error());  // PUBACK matched the expected bytes
    IS_TRUE(client.connected());

    END_IT
}

static int test_fragmented_stream() {
    IT("streams a fragmented PUBLISH payload to the Stream (phase 2)");
    reset_callback();

    Stream stream;
    uint8_t expectedPayload[] = "payload";
    stream.expect(expectedPayload, sizeof(expectedPayload) - 1);  // 7 payload bytes must reach the Stream

    ShimClient shimClient;
    shimClient.setAllowConnect(true);
    byte connack[] = {0x20, 0x02, 0x00, 0x00};
    shimClient.respond(connack, sizeof(connack));

    PubSubClient client(server, 1883, callback, shimClient, stream);
    IS_TRUE(client.setRxBufferSize(64));
    IS_TRUE(client.connect("fragment_test"));

    byte publish[] = {0x30, 0x0E, 0x00, 0x05, 't', 'o', 'p', 'i', 'c', 'p', 'a', 'y', 'l', 'o', 'a', 'd'};
    for (size_t i = 0; i < sizeof(publish); i++) {
        shimClient.respond(&publish[i], 1);
        IS_TRUE(client.loop());
    }

    IS_TRUE(callback_called);
    IS_TRUE(strcmp(lastTopic, "topic") == 0);
    IS_TRUE(lastLength == 7);
    IS_FALSE(stream.error());  // the exact payload bytes were streamed
    IS_FALSE(shimClient.error());
    IS_TRUE(client.connected());

    END_IT
}

static int test_oversized_disconnect_default() {
    IT("disconnects on an oversized packet by default (fail-fast)");
    reset_callback();

    ShimClient shimClient;
    PubSubClient client(server, 1883, callback, shimClient);
    IS_TRUE(setup_connected(shimClient, client, 20));  // default policy = MQTT_OVERSIZED_DISCONNECT
    IS_TRUE(client.getOversizedPolicy() == MQTT_OVERSIZED_DISCONNECT);

    // Oversized PUBLISH: remaining length 23 (0x17), total 25 > 20
    byte big[25];
    big[0] = 0x30;
    big[1] = 0x17;
    big[2] = 0x00;
    big[3] = 0x05;
    memcpy(&big[4], "topic", 5);
    memset(&big[9], 'A', 16);

    bool disconnected = false;
    for (size_t i = 0; i < sizeof(big); i++) {
        shimClient.respond(&big[i], 1);
        if (!client.loop()) {
            disconnected = true;  // fail-fast closed the connection once the oversized length was known
            break;
        }
    }
    IS_TRUE(disconnected);
    IS_FALSE(callback_called);
    IS_FALSE(client.connected());

    END_IT
}

static int test_bogus_length_disconnect() {
    IT("disconnects on a bogus (beyond-sane) length even with ignore policy");
    reset_callback();

    ShimClient shimClient;
    PubSubClient client(server, 1883, callback, shimClient);
    IS_TRUE(setup_connected(shimClient, client, 64));
    client.setOversizedPolicy(MQTT_OVERSIZED_IGNORE);

    // Remaining length encoded as the protocol maximum (0xFF,0xFF,0xFF,0x7F = 268435455): total exceeds
    // MQTT_MAX_SANE_PACKET_SIZE, so it is treated as bogus/corrupt and disconnects regardless of the policy.
    byte header[] = {0x30, 0xFF, 0xFF, 0xFF, 0x7F};
    bool disconnected = false;
    for (size_t i = 0; i < sizeof(header); i++) {
        shimClient.respond(&header[i], 1);
        if (!client.loop()) {
            disconnected = true;
            break;
        }
    }
    IS_TRUE(disconnected);
    IS_FALSE(callback_called);
    IS_FALSE(client.connected());

    END_IT
}

int main() {
    SUITE("Fragmented (non-blocking receive)");
    test_fragmented_receive();
    test_max_bytes_per_loop();
    test_fragmented_oversized_discard();
    test_fragmented_qos1();
    test_fragmented_stream();
    test_oversized_disconnect_default();
    test_bogus_length_disconnect();

    FINISH
}

#else
static int test_blocking_profile() {
    IT("incremental RX is excluded from this build");
    IS_EQUAL(MQTT_ENABLE_INCREMENTAL_RX, 0);
    END_IT
}
int main() {
    SUITE("Blocking-only receive profile");
    test_blocking_profile();
    FINISH
}
#endif
