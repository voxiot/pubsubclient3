#include "BDDTest.h"
#include "Buffer.h"
#include "PubSubClient.h"
#include "ShimClient.h"
#include "trace.h"

byte server[] = {172, 16, 0, 2};

// function declarations
void callback(char* topic, uint8_t* payload, size_t plength);
int test_publish();
int test_publish_bytes();
int test_publish_retained();
int test_publish_retained_2();
int test_publish_not_connected();
int test_publish_long();
int test_publish_too_long();
int test_publish_P();
int test_publish_P_too_long();
int test_publish_empty_topic();
int test_publish_null_payload();
int test_publish_qos1();
int test_publish_qos2();
int test_publish_P_qos1();
int test_publish_P_qos2();
int test_publish_FlashStringHelper();
int test_publish_FlashStringHelper2();
int test_publish_P_FlashStringHelper();
int test_publish_P_P();
int test_publish_completes_short_writes();
int test_publish_nothing_written_keeps_connection();
int test_publish_partial_write_header();
int test_publish_partial_write_payload();
int test_publish_zero_payload_write_after_header();
int test_publish_zero_write_between_payload_buffers();
#ifdef MQTT_MAX_TRANSFER_SIZE
int test_control_packet_zero_write_after_chunk();
#endif

void callback(_UNUSED_ char* topic, _UNUSED_ uint8_t* payload, _UNUSED_ size_t plength) {
    // handle message arrived
}

int test_publish() {
    IT("publishes a null-terminated string");
    ShimClient shimClient;
    shimClient.setAllowConnect(true);

    byte connack[] = {0x20, 0x02, 0x00, 0x00};
    shimClient.respond(connack, sizeof(connack));

    PubSubClient client(server, 1883, callback, shimClient);
    bool rc = client.connect("client_test1");
    IS_TRUE(rc);

    byte publish[] = {0x30, 0x0e, 0x00, 0x05, 't', 'o', 'p', 'i', 'c', 'p', 'a', 'y', 'l', 'o', 'a', 'd'};
    shimClient.expect(publish, sizeof(publish));

    rc = client.publish("topic", "payload");
    IS_TRUE(rc);

    IS_FALSE(shimClient.error());

    END_IT
}

int test_publish_bytes() {
    IT("publishes a byte array");
    ShimClient shimClient;
    shimClient.setAllowConnect(true);

    byte payload[] = {0x01, 0x02, 0x03, 0x00, 0x05};
    size_t length = sizeof(payload);

    byte connack[] = {0x20, 0x02, 0x00, 0x00};
    shimClient.respond(connack, sizeof(connack));

    PubSubClient client(server, 1883, callback, shimClient);
    bool rc = client.connect("client_test1");
    IS_TRUE(rc);

    byte publish[] = {0x30, 0x0c, 0x00, 0x05, 't', 'o', 'p', 'i', 'c', 0x01, 0x02, 0x03, 0x00, 0x05};
    shimClient.expect(publish, sizeof(publish));

    rc = client.publish("topic", payload, length);
    IS_TRUE(rc);

    IS_FALSE(shimClient.error());

    END_IT
}

int test_publish_retained() {
    IT("publishes retained - 1");
    ShimClient shimClient;
    shimClient.setAllowConnect(true);

    byte payload[] = {0x01, 0x02, 0x03, 0x00, 0x05};
    size_t length = sizeof(payload);

    byte connack[] = {0x20, 0x02, 0x00, 0x00};
    shimClient.respond(connack, sizeof(connack));

    PubSubClient client(server, 1883, callback, shimClient);
    bool rc = client.connect("client_test1");
    IS_TRUE(rc);

    byte publish[] = {0x31, 0x0c, 0x00, 0x05, 't', 'o', 'p', 'i', 'c', 0x1, 0x2, 0x3, 0x0, 0x5};
    shimClient.expect(publish, sizeof(publish));

    rc = client.publish("topic", payload, length, true);
    IS_TRUE(rc);

    IS_FALSE(shimClient.error());

    END_IT
}

int test_publish_retained_2() {
    IT("publishes retained - 2");
    ShimClient shimClient;
    shimClient.setAllowConnect(true);

    byte connack[] = {0x20, 0x02, 0x00, 0x00};
    shimClient.respond(connack, sizeof(connack));

    PubSubClient client(server, 1883, callback, shimClient);
    bool rc = client.connect("client_test1");
    IS_TRUE(rc);

    byte publish[] = {0x31, 0x0c, 0x00, 0x05, 't', 'o', 'p', 'i', 'c', 'A', 'B', 'C', 'D', 'E'};
    shimClient.expect(publish, sizeof(publish));

    rc = client.publish("topic", "ABCDE", true);
    IS_TRUE(rc);

    IS_FALSE(shimClient.error());

    END_IT
}

int test_publish_not_connected() {
    IT("publish fails when not connected");
    ShimClient shimClient;

    PubSubClient client(server, 1883, callback, shimClient);

    bool rc = client.publish("topic", "payload");
    IS_FALSE(rc);

    IS_FALSE(shimClient.error());

    END_IT
}

int test_publish_long() {
    IT("publishes with long payload message (> buffer size)");
    ShimClient shimClient;
    shimClient.setAllowConnect(true);

    // buffer size 64 bytes - 5 bytes header - 2 bytes topic length = max. 57 bytes topic
    //              0        1         2         3         4         5         6         7         8         9         0         1         2
    char topic[] = "123456789012345678901234567890123456789012345678901234567";

    //                0        1         2         3         4         5         6         7         8         9         0         1         2
    char payload[] = "123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890";
    size_t plength = strlen(payload);

    byte connack[] = {0x20, 0x02, 0x00, 0x00};
    shimClient.respond(connack, 4);

    PubSubClient client(server, 1883, callback, shimClient);
    client.setBufferSize(64);
    bool rc = client.connect("client_test1");
    IS_TRUE(rc);

    byte publish[256];
    publish[0] = 0x30;  // PUBLISH, QoS 0, no retain
    publish[1] = 0xb3;  // Remaining length byte 1: 2 + 57 + 120 bytes (topic length bytes + topic length + payload length = 179)
    publish[2] = 0x01;  // Remaining length byte 2
    publish[3] = 0x00;  // Topic length MSB
    publish[4] = 0x39;  // Topic length LSB (57 bytes)
    memcpy(&publish[5], topic, sizeof(topic) - 1);
    memcpy(&publish[5 + sizeof(topic) - 1], payload, sizeof(payload) - 1);
    shimClient.expect(publish, 5 + sizeof(topic) - 1 + sizeof(payload) - 1);

    rc = client.beginPublish(topic, plength, 0, false);
    IS_TRUE(rc);
    plength = client.write((uint8_t*)payload, plength);
    IS_EQUAL(plength, strlen(payload));
    rc = client.endPublish();
    IS_TRUE(rc);

    IS_FALSE(shimClient.error());

    END_IT
}

int test_publish_too_long() {
    IT("publish fails when topic/payload are too long");
    ShimClient shimClient;
    shimClient.setAllowConnect(true);

    //              0        1         2         3         4         5         6         7         8         9         0         1         2
    char topic[] = "1234567890123456789012345678901234567890123456789012345678901234";

    //                0        1         2         3         4         5         6         7         8         9         0         1         2
    char payload[] = "123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890";

    byte connack[] = {0x20, 0x02, 0x00, 0x00};
    shimClient.respond(connack, 4);

    PubSubClient client(server, 1883, callback, shimClient);
    client.setBufferSize(64);
    bool rc = client.connect("client_test1");
    IS_TRUE(rc);

    rc = client.publish(topic, payload);
    IS_FALSE(rc);

    IS_FALSE(shimClient.error());

    END_IT
}

int test_publish_P() {
    IT("publishes using PROGMEM payload");
    ShimClient shimClient;
    shimClient.setAllowConnect(true);

    byte payload[] = {0x01, 0x02, 0x03, 0x00, 0x05};
    size_t length = sizeof(payload);

    byte connack[] = {0x20, 0x02, 0x00, 0x00};
    shimClient.respond(connack, sizeof(connack));

    PubSubClient client(server, 1883, callback, shimClient);
    bool rc = client.connect("client_test1");
    IS_TRUE(rc);

    byte publish[] = {0x31, 0x0c, 0x00, 0x05, 't', 'o', 'p', 'i', 'c', 0x01, 0x02, 0x03, 0x00, 0x05};
    shimClient.expect(publish, sizeof(publish));

    rc = client.publish_P("topic", payload, length, true);
    IS_TRUE(rc);

    IS_FALSE(shimClient.error());

    END_IT
}

int test_publish_P_too_long() {
    IT("publish using PROGMEM payload fails when topic is too long");
    ShimClient shimClient;
    shimClient.setAllowConnect(true);

    //              0        1         2         3         4         5         6         7         8         9         0         1         2
    char topic[] = "1234567890123456789012345678901234567890123456789012345678901234";

    //                0        1         2         3         4         5         6         7         8         9         0         1         2
    char payload[] = "123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890";

    byte connack[] = {0x20, 0x02, 0x00, 0x00};
    shimClient.respond(connack, sizeof(connack));

    PubSubClient client(server, 1883, callback, shimClient);
    client.setBufferSize(64);
    bool rc = client.connect("client_test1");
    IS_TRUE(rc);

    rc = client.publish_P(topic, payload, false);
    IS_FALSE(rc);

    IS_FALSE(shimClient.error());

    END_IT
}

int test_publish_FlashStringHelper() {
    IT("publishes using FlashStringHelper topic");
    ShimClient shimClient;
    shimClient.setAllowConnect(true);

    char payload[] = "12345";

    byte connack[] = {0x20, 0x02, 0x00, 0x00};
    shimClient.respond(connack, sizeof(connack));

    PubSubClient client(server, 1883, callback, shimClient);
    bool rc = client.connect("client_test1");
    IS_TRUE(rc);

    byte publish[] = {0x31, 0x0c, 0x00, 0x05, 't', 'o', 'p', 'i', 'c', '1', '2', '3', '4', '5'};
    shimClient.expect(publish, sizeof(publish));

    rc = client.publish(F("topic"), payload, MQTT_QOS0, true);
    IS_TRUE(rc);

    IS_FALSE(shimClient.error());

    END_IT
}

int test_publish_FlashStringHelper2() {
    IT("publishes using FlashStringHelper topic and payload");
    ShimClient shimClient;
    shimClient.setAllowConnect(true);

    char payload[] = "12345";

    byte connack[] = {0x20, 0x02, 0x00, 0x00};
    shimClient.respond(connack, sizeof(connack));

    PubSubClient client(server, 1883, callback, shimClient);
    bool rc = client.connect("client_test1");
    IS_TRUE(rc);

    byte publish[] = {0x31, 0x0c, 0x00, 0x05, 't', 'o', 'p', 'i', 'c', '1', '2', '3', '4', '5'};
    shimClient.expect(publish, sizeof(publish));

    rc = client.publish(F("topic"), F(payload), MQTT_QOS0, true);
    IS_TRUE(rc);

    IS_FALSE(shimClient.error());

    END_IT
}

int test_publish_P_FlashStringHelper() {
    IT("publishes using FlashStringHelper topic and PROGMEM payload");
    ShimClient shimClient;
    shimClient.setAllowConnect(true);

    char payload[] PROGMEM = "12345";

    byte connack[] = {0x20, 0x02, 0x00, 0x00};
    shimClient.respond(connack, sizeof(connack));

    PubSubClient client(server, 1883, callback, shimClient);
    bool rc = client.connect("client_test1");
    IS_TRUE(rc);

    byte publish[] = {0x31, 0x0c, 0x00, 0x05, 't', 'o', 'p', 'i', 'c', '1', '2', '3', '4', '5'};
    shimClient.expect(publish, sizeof(publish));

    rc = client.publish_P(F("topic"), payload, MQTT_QOS0, true);
    IS_TRUE(rc);

    IS_FALSE(shimClient.error());

    END_IT
}

int test_publish_P_P() {
    IT("publishes using PROGMEM topic and PROGMEM payload");
    ShimClient shimClient;
    shimClient.setAllowConnect(true);

    char topic[] PROGMEM = "topic";
    char payload[] PROGMEM = "12345";
    size_t length = strlen_P(payload);

    byte connack[] = {0x20, 0x02, 0x00, 0x00};
    shimClient.respond(connack, sizeof(connack));

    PubSubClient client(server, 1883, callback, shimClient);
    bool rc = client.connect("client_test1");
    IS_TRUE(rc);

    byte publish[] = {0x31, 0x0c, 0x00, 0x05, 't', 'o', 'p', 'i', 'c', '1', '2', '3', '4', '5'};
    shimClient.expect(publish, sizeof(publish));

    rc = client.beginPublish_P(topic, length, MQTT_QOS0, true);
    IS_TRUE(rc);
    length = client.write_P(payload);
    IS_EQUAL(length, strlen_P(payload));
    rc = client.endPublish();
    IS_TRUE(rc);

    IS_FALSE(shimClient.error());

    END_IT
}

int test_publish_empty_topic() {
    IT("publish fails when topic is empty");
    ShimClient shimClient;
    shimClient.setAllowConnect(true);

    byte connack[] = {0x20, 0x02, 0x00, 0x00};
    shimClient.respond(connack, 4);

    PubSubClient client(server, 1883, callback, shimClient);
    bool rc = client.connect("client_test1");
    IS_TRUE(rc);

    rc = client.publish("", "payload");
    IS_FALSE(rc);

    IS_FALSE(shimClient.error());

    END_IT
}

int test_publish_null_payload() {
    IT("publishes with null payload");
    ShimClient shimClient;
    shimClient.setAllowConnect(true);

    byte connack[] = {0x20, 0x02, 0x00, 0x00};
    shimClient.respond(connack, sizeof(connack));

    PubSubClient client(server, 1883, callback, shimClient);
    bool rc = client.connect("client_test1");
    IS_TRUE(rc);

    byte publish[] = {0x30, 0x07, 0x00, 0x05, 't', 'o', 'p', 'i', 'c'};
    shimClient.expect(publish, sizeof(publish));

    rc = client.publish("topic", nullptr);
    IS_TRUE(rc);

    IS_FALSE(shimClient.error());

    END_IT
}

int test_publish_qos1() {
    IT("publishes with QoS 1");
    ShimClient shimClient;
    shimClient.setAllowConnect(true);

    byte connack[] = {0x20, 0x02, 0x00, 0x00};
    shimClient.respond(connack, 4);

    PubSubClient client(server, 1883, callback, shimClient);
    bool rc = client.connect("client_test1");
    IS_TRUE(rc);

    // Example publish packet for QoS 1 (0x32)
    byte publish[] = {0x32, 0x10, 0x00, 0x05, 't', 'o', 'p', 'i', 'c', 0x00, 0x02, 'p', 'a', 'y', 'l', 'o', 'a', 'd'};
    shimClient.expect(publish, sizeof(publish));

    rc = client.publish("topic", "payload", MQTT_QOS1, false);
    IS_TRUE(rc);

    IS_FALSE(shimClient.error());

    END_IT
}

int test_publish_qos2() {
    IT("publishes with QoS 2");
    ShimClient shimClient;
    shimClient.setAllowConnect(true);

    byte connack[] = {0x20, 0x02, 0x00, 0x00};
    shimClient.respond(connack, 4);

    PubSubClient client(server, 1883, callback, shimClient);
    bool rc = client.connect("client_test1");
    IS_TRUE(rc);

    // Example publish packet for QoS 2 (0x34)
    byte publish[] = {0x34, 0x10, 0x00, 0x05, 't', 'o', 'p', 'i', 'c', 0x00, 0x02, 'p', 'a', 'y', 'l', 'o', 'a', 'd'};
    shimClient.expect(publish, sizeof(publish));

    rc = client.publish("topic", "payload", MQTT_QOS2, false);
    IS_TRUE(rc);

    IS_FALSE(shimClient.error());

    END_IT
}

int test_publish_P_qos1() {
    IT("publishes using PROGMEM payload with QoS 1 retained");
    ShimClient shimClient;
    shimClient.setAllowConnect(true);

    byte payload[] = {0x01, 0x02, 0x03, 0x04, 0x05};
    size_t length = sizeof(payload);

    byte connack[] = {0x20, 0x02, 0x00, 0x00};
    shimClient.respond(connack, sizeof(connack));

    PubSubClient client(server, 1883, callback, shimClient);
    bool rc = client.connect("client_test1");
    IS_TRUE(rc);

    byte publish[] = {0x33, 0x0e, 0x00, 0x05, 't', 'o', 'p', 'i', 'c', 0x00, 0x02, 0x01, 0x02, 0x03, 0x04, 0x05};
    shimClient.expect(publish, sizeof(publish));

    rc = client.publish_P("topic", payload, length, MQTT_QOS1, true);
    IS_TRUE(rc);

    IS_FALSE(shimClient.error());

    END_IT
}

int test_publish_P_qos2() {
    IT("publishes using PROGMEM payload with QoS 2 retained");
    ShimClient shimClient;
    shimClient.setAllowConnect(true);

    byte payload[] = {0x01, 0x02, 0x03, 0x04, 0x05};
    size_t length = sizeof(payload);

    byte connack[] = {0x20, 0x02, 0x00, 0x00};
    shimClient.respond(connack, sizeof(connack));

    PubSubClient client(server, 1883, callback, shimClient);
    bool rc = client.connect("client_test1");
    IS_TRUE(rc);

    byte publish[] = {0x35, 0x0e, 0x00, 0x05, 't', 'o', 'p', 'i', 'c', 0x00, 0x02, 0x01, 0x02, 0x03, 0x04, 0x05};
    shimClient.expect(publish, sizeof(publish));

    rc = client.publish_P("topic", payload, length, MQTT_QOS2, true);
    IS_TRUE(rc);

    IS_FALSE(shimClient.error());

    END_IT
}

int test_publish_completes_short_writes() {
    IT("completes a publish whose writes are all short");
    ShimClient shimClient;
    shimClient.setAllowConnect(true);

    byte connack[] = {0x20, 0x02, 0x00, 0x00};
    shimClient.respond(connack, sizeof(connack));

    PubSubClient client(server, 1883, callback, shimClient);
    bool rc = client.connect("client_test1");
    IS_TRUE(rc);

    // A socket that never takes more than 4 bytes at a time: a transient short write is the common case
    // in the field (full transmit window), so the packet must be completed rather than abandoned, and the
    // expect() buffer proves the broker sees the same bytes as an unrestricted write would have sent.
    byte publish[] = {0x30, 0x0e, 0x00, 0x05, 't', 'o', 'p', 'i', 'c', 'p', 'a', 'y', 'l', 'o', 'a', 'd'};
    shimClient.expect(publish, sizeof(publish));
    shimClient.setWriteChunk(4);

    rc = client.publish("topic", "payload");
    IS_TRUE(rc);

    IS_FALSE(shimClient.error());
    IS_EQUAL(client.state(), MQTT_CONNECTED);
    IS_TRUE(client.connected());

    END_IT
}

int test_publish_nothing_written_keeps_connection() {
    IT("keeps the connection when the publish write places no byte at all");
    ShimClient shimClient;
    shimClient.setAllowConnect(true);

    byte connack[] = {0x20, 0x02, 0x00, 0x00};
    shimClient.respond(connack, sizeof(connack));

    PubSubClient client(server, 1883, callback, shimClient);
    client.setSocketTimeout(1);  // keep the retry window short; the deadline is reached either way
    bool rc = client.connect("client_test1");
    IS_TRUE(rc);

    // The socket accepts nothing from here on. No byte of the packet reached the broker, so the stream is
    // still correctly framed: the publish fails, but tearing the connection down would be gratuitous.
    shimClient.setWriteLimit(0);

    rc = client.publish("topic", "payload");
    IS_FALSE(rc);

    IS_EQUAL(client.state(), MQTT_CONNECTED);
    IS_TRUE(client.connected());

    END_IT
}

int test_publish_partial_write_header() {
    IT("closes the connection when the publish header is only partially written");
    ShimClient shimClient;
    shimClient.setAllowConnect(true);

    byte connack[] = {0x20, 0x02, 0x00, 0x00};
    shimClient.respond(connack, sizeof(connack));

    PubSubClient client(server, 1883, callback, shimClient);
    client.setSocketTimeout(1);  // bound the retry of the remainder
    bool rc = client.connect("client_test1");
    IS_TRUE(rc);

    // The header+topic write is 9 bytes (2 header + 2 topic length + 5 topic): let only 5 of them out and
    // then nothing, so the remainder can never be completed. Those 5 bytes announced a remaining length,
    // so the broker would read this payload out of the next packet: the client must close to resynchronise.
    shimClient.setWriteLimit(5);

    rc = client.publish("topic", "payload");
    IS_FALSE(rc);

    IS_EQUAL(client.state(), MQTT_CONNECTION_LOST);
    IS_FALSE(shimClient.connected());
    IS_FALSE(client.connected());

    END_IT
}

int test_publish_partial_write_payload() {
    IT("closes the connection when the publish payload is only partially written");
    ShimClient shimClient;
    shimClient.setAllowConnect(true);

    byte connack[] = {0x20, 0x02, 0x00, 0x00};
    shimClient.respond(connack, sizeof(connack));

    PubSubClient client(server, 1883, callback, shimClient);
    client.setSocketTimeout(1);  // bound the retry of the remainder
    bool rc = client.connect("client_test1");
    IS_TRUE(rc);

    // Let the whole 9-byte header+topic out, then only 3 of the 7 payload bytes: the announced remaining
    // length is already on the wire, so the broker would take the next packet as the rest of this payload.
    shimClient.setWriteLimit(9 + 3);

    rc = client.publish("topic", "payload");
    IS_FALSE(rc);

    IS_EQUAL(client.state(), MQTT_CONNECTION_LOST);
    IS_FALSE(shimClient.connected());
    IS_FALSE(client.connected());

    END_IT
}

int test_publish_zero_payload_write_after_header() {
    IT("closes after a full PUBLISH header but zero payload bytes, then reconnects cleanly");
    for (uint8_t qos = MQTT_QOS0; qos <= MQTT_QOS2; qos++) {
        for (uint8_t progmem = 0; progmem < 2; progmem++) {
            ShimClient shimClient;
            byte connack[] = {0x20, 0x02, 0x00, 0x00};
            shimClient.respond(connack, sizeof(connack));
            PubSubClient client(server, 1883, callback, shimClient);
            client.setSocketTimeout(1);
            IS_TRUE(client.connect("client_test1"));

            const uint16_t before = shimClient.received();
            const uint16_t headerSize = (qos == MQTT_QOS0) ? 9 : 11;
            shimClient.setWriteLimit(headerSize);  // accept the whole header, then block all payload writes
            byte payload[] = {'p', 'a', 'y', 'l', 'o', 'a', 'd'};
            bool rc = progmem ? client.publish_P("topic", payload, sizeof(payload), qos, false)
                              : client.publish("topic", payload, sizeof(payload), qos, false);
            IS_FALSE(rc);
            IS_EQUAL(shimClient.received() - before, headerSize);
            IS_EQUAL(client.state(), MQTT_CONNECTION_LOST);
            IS_FALSE(shimClient.connected());
            IS_FALSE(client.connected());
            IS_FALSE(client.endPublish());

            // Restoring transport capacity must not append another packet to the broken connection.
            shimClient.setWriteLimit(1024);
            const uint16_t stoppedAt = shimClient.received();
            IS_FALSE(client.publish("topic", "next"));
            IS_EQUAL(shimClient.received(), stoppedAt);

            shimClient.respond(connack, sizeof(connack));
            IS_TRUE(client.connect("client_test1"));
            // No byte of the failed payload may leak into the new session.
            byte publish[] = {0x30, 0x0b, 0x00, 0x05, 't', 'o', 'p', 'i', 'c', 'n', 'e', 'x', 't'};
            shimClient.expect(publish, sizeof(publish));
            const uint16_t reconnectedAt = shimClient.received();
            IS_TRUE(client.publish("topic", "next"));
            IS_EQUAL(shimClient.received() - reconnectedAt, sizeof(publish));
            IS_FALSE(shimClient.error());
        }
    }
    END_IT
}

int test_publish_zero_write_between_payload_buffers() {
    IT("closes when a full payload buffer cannot send any byte, including after an earlier full buffer");
    for (uint8_t qos = MQTT_QOS0; qos <= MQTT_QOS2; qos++) {
        for (uint16_t acceptedPayload = 0; acceptedPayload <= 32; acceptedPayload += 32) {
            ShimClient shimClient;
            byte connack[] = {0x20, 0x02, 0x00, 0x00};
            shimClient.respond(connack, sizeof(connack));
            PubSubClient client(server, 1883, callback, shimClient);
            IS_TRUE(client.setBufferSize(32));
            client.setSocketTimeout(1);
            IS_TRUE(client.connect("client_test1"));

            byte payload[64];
            memset(payload, 'x', sizeof(payload));
            const uint16_t before = shimClient.received();
            const uint16_t headerSize = (qos == MQTT_QOS0) ? 9 : 11;
            shimClient.setWriteLimit(headerSize + acceptedPayload);
            IS_TRUE(client.beginPublish("topic", sizeof(payload), qos, false));
            IS_TRUE(client.write(payload, sizeof(payload)) < sizeof(payload));
            IS_EQUAL(shimClient.received() - before, headerSize + acceptedPayload);
            IS_EQUAL(client.state(), MQTT_CONNECTION_LOST);
            IS_FALSE(shimClient.connected());
            IS_FALSE(client.endPublish());
        }
    }
    END_IT
}

#ifdef MQTT_MAX_TRANSFER_SIZE
int test_control_packet_zero_write_after_chunk() {
    IT("closes when a control packet sends a full transfer chunk but no byte of its next chunk");
    ShimClient shimClient;
    byte connack[] = {0x20, 0x02, 0x00, 0x00};
    shimClient.respond(connack, sizeof(connack));
    PubSubClient client(server, 1883, callback, shimClient);
    IS_TRUE(client.setBufferSize(MQTT_MAX_TRANSFER_SIZE + 64));
    client.setSocketTimeout(1);
    IS_TRUE(client.connect("client_test1"));

    char topic[MQTT_MAX_TRANSFER_SIZE + 1];
    memset(topic, 't', sizeof(topic) - 1);
    topic[sizeof(topic) - 1] = '\0';
    const uint16_t before = shimClient.received();
    shimClient.setWriteLimit(MQTT_MAX_TRANSFER_SIZE);
    IS_FALSE(client.subscribe(topic, MQTT_QOS1));
    IS_EQUAL(shimClient.received() - before, MQTT_MAX_TRANSFER_SIZE);
    IS_EQUAL(client.state(), MQTT_CONNECTION_LOST);
    IS_FALSE(shimClient.connected());
    END_IT
}
#endif

int main() {
    SUITE("Publish");
    test_publish();
    test_publish_bytes();
    test_publish_retained();
    test_publish_retained_2();
    test_publish_qos1();
    test_publish_qos2();
    test_publish_null_payload();
    test_publish_long();
    test_publish_not_connected();
    test_publish_empty_topic();
    test_publish_too_long();
    test_publish_P();
    test_publish_P_qos1();
    test_publish_P_qos2();
    test_publish_P_too_long();
    test_publish_FlashStringHelper();
    test_publish_FlashStringHelper2();
    test_publish_P_FlashStringHelper();
    test_publish_P_P();
    test_publish_completes_short_writes();
    test_publish_nothing_written_keeps_connection();
    test_publish_partial_write_header();
    test_publish_partial_write_payload();
    test_publish_zero_payload_write_after_header();
    test_publish_zero_write_between_payload_buffers();
#ifdef MQTT_MAX_TRANSFER_SIZE
    test_control_packet_zero_write_after_chunk();
#endif

    FINISH
}
