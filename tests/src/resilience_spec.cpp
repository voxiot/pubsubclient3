#include "BDDTest.h"
#include "PubSubClient.h"
#include "ShimClient.h"
#include <deque>
#include <vector>
#include <string>
#include <type_traits>
#include <limits>
#include <unistd.h>

// A socket discards unread bytes on stop(). Record the actual wire, not only return values.
class WireClient : public ShimClient {
 public:
    std::deque<uint8_t> input;
    std::vector<uint8_t> wire;
    size_t writeCalls = 0;
    size_t stopCalls = 0;
    unsigned long timeAtWrite = 0;
    unsigned long yieldsAtWrite = 0;
    using ShimClient::write;
    using ShimClient::read;
    size_t write(const uint8_t* bytes, size_t size) override {
        ++writeCalls;
        timeAtWrite = peekTestMillis();
        yieldsAtWrite = getTestYieldCalls();
        const size_t n = ShimClient::write(bytes, size);
        wire.insert(wire.end(), bytes, bytes + n);
        return n;
    }
    int available() override { return !input.empty(); }
    int read() override {
        if (input.empty()) return -1;
        const int b = input.front(); input.pop_front(); return b;
    }
    void stop() override { ++stopCalls; input.clear(); ShimClient::stop(); }
    void queue(std::initializer_list<uint8_t> bytes) { input.insert(input.end(), bytes); }
};

static size_t calls;
static PubSubClient* current;
static bool callbackChecks;
static bool disconnectInCallback;
static bool publishInCallback;
static bool incompleteInCallback;
static void received(char*, uint8_t*, size_t) {
    ++calls;
    if (disconnectInCallback) {
        current->disconnect();
        callbackChecks = current->getRxQos() == MQTT_QOS1 &&
                         !current->setBufferSize(0) && !current->setRxBufferSize(0) &&
                         !current->connect("nested");
    }
    if (publishInCallback) callbackChecks = current->publish("reply", "ok");
    if (incompleteInCallback) callbackChecks = current->beginPublish("reply", 2, false);
}

struct Fixture {
    WireClient socket;
    PubSubClient mqtt;
    explicit Fixture(bool incremental) : mqtt("broker", 1883, received, socket) {
        calls = 0; current = &mqtt; callbackChecks = false;
        disconnectInCallback = publishInCallback = incompleteInCallback = false;
        mqtt.setSocketTimeout(1); mqtt.setKeepAlive(0);
        if (incremental) mqtt.setRxBufferSize(64);
    }
    bool connect(const char* id = "test", bool clean = true, bool present = false) {
        mqtt.connected(); // let the previous transport loss be observed before the new CONNACK arrives
        socket.queue({0x20, 2, (uint8_t)(present ? 1 : 0), 0});
        const bool result = mqtt.connect(id, nullptr, nullptr, nullptr, 0, false, nullptr, clean);
        socket.wire.clear();
        return result;
    }
};

static int callback_lifetime(bool incremental) {
    IT("callback disconnect cannot free its active buffers or reconnect");
    Fixture f(incremental);
    IS_TRUE(f.connect()); disconnectInCallback = true;
    f.socket.queue({0x32, 6, 0, 1, 't', 0, 7, 'x'});
    IS_FALSE(f.mqtt.loop());
    IS_EQUAL(calls, 1u); IS_TRUE(callbackChecks);
    IS_EQUAL(f.socket.wire.size(), 2u); // only DISCONNECT, no ACK after it
    IS_TRUE(f.mqtt.setBufferSize(0));
    IS_TRUE(f.mqtt.setRxBufferSize(0));
    END_IT
}

static int ownership() {
    IT("owning clients cannot be copied or copy-assigned");
    IS_FALSE(std::is_copy_constructible<PubSubClient>::value);
    IS_FALSE(std::is_copy_assignable<PubSubClient>::value);
    END_IT
}

static int blocking_timeout() {
    IT("blocking timeout in header or payload closes the socket");
    for (int tail = 0; tail < 2; ++tail) {
        Fixture f(false); IS_TRUE(f.connect());
        if (tail) f.socket.queue({0x30, 5, 0, 1, 't'});
        else f.socket.queue({0x30, 0x80});
        IS_FALSE(f.mqtt.loop()); IS_FALSE(f.mqtt.connected());
        IS_EQUAL(f.mqtt.state(), MQTT_CONNECTION_TIMEOUT);
        IS_EQUAL(calls, 0u);
        IS_TRUE(f.connect());
        f.socket.queue({0x30, 4, 0, 1, 't', 'x'});
        IS_TRUE(f.mqtt.loop()); IS_EQUAL(calls, 1u);
    }
    END_IT
}

static int reconnect_resets_rx() {
    IT("incremental reconnect drops old header, payload and discard state");
    for (int state = 0; state < 3; ++state) {
        Fixture f(true); IS_TRUE(f.connect());
        f.mqtt.setSocketTimeout(2);
        if (state == 0) f.socket.queue({0x30, 0x80});
        if (state == 1) f.socket.queue({0x30, 5, 0, 1});
        if (state == 2) {
            f.mqtt.setOversizedPolicy(MQTT_OVERSIZED_IGNORE);
            f.socket.queue({0x30, 80, 0, 1});
        }
        IS_TRUE(f.mqtt.loop());
        f.socket.stop(); IS_FALSE(f.mqtt.connected());
        IS_TRUE(f.connect());
        f.socket.queue({0x30, 4, 0, 1, 't', 'x'});
        IS_TRUE(f.mqtt.loop()); IS_EQUAL(calls, 1u);
    }
    END_IT
}

static int malformed_qos_recovers(bool incremental) {
    IT("missing or zero packet ID cannot leave the callback guard locked");
    for (int zero = 0; zero < 2; ++zero) {
        Fixture f(incremental); IS_TRUE(f.connect());
        if (zero) f.socket.queue({0x32, 5, 0, 1, 't', 0, 0});
        else f.socket.queue({0x32, 3, 0, 1, 't'});
        IS_FALSE(f.mqtt.loop()); IS_EQUAL(calls, 0u);
        IS_TRUE(f.connect());
        f.socket.queue({0x30, 4, 0, 1, 't', 'x'});
        IS_TRUE(f.mqtt.loop()); IS_EQUAL(calls, 1u);
    }
    END_IT
}

static int manual_publish(bool incremental) {
    IT("manual PUBLISH defers RX and keepalive until its payload is complete");
    Fixture f(incremental); IS_TRUE(f.connect());
    f.mqtt.setKeepAlive(1);
    IS_TRUE(f.mqtt.beginPublish("t", 3, false));
    const size_t header = f.socket.wire.size();
    IS_EQUAL(f.mqtt.write((uint8_t)'a'), 1u);
    f.socket.queue({0x32, 6, 0, 1, 'r', 0, 7, 'x'});
    sleep(2); // a ping would now be due if loop() entered keepalive processing
    IS_TRUE(f.mqtt.loop()); IS_EQUAL(calls, 0u);
    IS_EQUAL(f.socket.wire.size(), header);
    IS_FALSE(f.mqtt.subscribe("sub")); IS_FALSE(f.mqtt.unsubscribe("sub"));
    IS_FALSE(f.mqtt.beginPublish("other", 0, false));
    IS_FALSE(f.mqtt.setBufferSize(512)); IS_FALSE(f.mqtt.setRxBufferSize(128));
    IS_EQUAL(f.mqtt.write((const uint8_t*)"bc", 2), 2u);
    IS_TRUE(f.mqtt.endPublish());
    IS_EQUAL(f.socket.wire.size(), header + 3);
    IS_TRUE(f.socket.wire[header] == 'a' && f.socket.wire[header + 1] == 'b' && f.socket.wire[header + 2] == 'c');
    f.mqtt.setKeepAlive(0);
    IS_TRUE(f.mqtt.loop()); IS_EQUAL(calls, 1u);
    IS_EQUAL(f.socket.wire.size(), header + 7); // ACK only after the payload
    END_IT
}

static int publish_lengths() {
    IT("manual payload rejects excess bytes and closes on incomplete end or disconnect");
    Fixture f(false); IS_TRUE(f.connect());
    IS_EQUAL(f.mqtt.write((uint8_t)'x'), 0u);
    IS_FALSE(f.mqtt.endPublish());
    IS_TRUE(f.mqtt.beginPublish("t", 1, false));
    IS_EQUAL(f.mqtt.write((const uint8_t*)"xx", 2), 0u);
    IS_EQUAL(f.mqtt.write((uint8_t)'x'), 1u);
    IS_EQUAL(f.mqtt.write((uint8_t)'x'), 0u);
    IS_TRUE(f.mqtt.endPublish()); IS_TRUE(f.mqtt.connected());
    IS_TRUE(f.mqtt.beginPublish("t", 2, false));
    IS_EQUAL(f.mqtt.write((uint8_t)'x'), 1u);
    IS_FALSE(f.mqtt.endPublish()); IS_FALSE(f.mqtt.connected());
    IS_TRUE(f.connect()); IS_TRUE(f.mqtt.beginPublish("t", 2, false));
    const size_t header = f.socket.wire.size();
    f.mqtt.disconnect(); IS_EQUAL(f.socket.wire.size(), header);
    END_IT
}

static int length_overflow() {
    IT("PUBLISH rejects wrapped and excessive remaining lengths without sending bytes");
    Fixture f(false); IS_TRUE(f.connect());
    IS_FALSE(f.mqtt.beginPublish("t", std::numeric_limits<size_t>::max() - 2, false));
    IS_FALSE(f.mqtt.beginPublish("t", MQTT_MAX_POSSIBLE_PACKET_SIZE, false));
    IS_TRUE(f.socket.wire.empty()); IS_TRUE(f.mqtt.connected());
    // Largest representable remaining length is still legal; disconnect without a payload allocation.
    IS_TRUE(f.mqtt.beginPublish("t", MQTT_MAX_POSSIBLE_PACKET_SIZE - 3, false));
    IS_FALSE(f.mqtt.endPublish());
    END_IT
}

static int incoming_budget(bool incremental) {
    IT("IGNORE respects a limit smaller than the physical buffer and drains to the next packet");
    Fixture f(incremental); IS_TRUE(f.connect());
    f.mqtt.setMaxIncomingPacket(6).setOversizedPolicy(MQTT_OVERSIZED_IGNORE);
    f.socket.queue({0x30, 5, 0, 1, 't', 'x', 'y', 0x30, 4, 0, 1, 't', 'z'});
    IS_TRUE(f.mqtt.loop());
    if (!incremental) IS_TRUE(f.mqtt.loop());
    IS_EQUAL(calls, 1u); IS_TRUE(f.mqtt.connected());
    END_IT
}

static int qos2_dedup(bool incremental) {
    IT("incoming QoS2 duplicates re-ACK without redelivery until PUBREL releases the ID");
    Fixture f(incremental); IS_TRUE(f.connect());
    f.socket.queue({0x34, 6, 0, 1, 't', 0, 7, 'x'});
    IS_TRUE(f.mqtt.loop()); IS_EQUAL(calls, 1u);
    f.socket.queue({0x3c, 6, 0, 1, 't', 0, 7, 'x'});
    IS_TRUE(f.mqtt.loop()); IS_EQUAL(calls, 1u);
    IS_EQUAL(f.socket.wire.size(), 8u);
    f.socket.queue({0x62, 2, 0, 7}); IS_TRUE(f.mqtt.loop());
    f.socket.queue({0x62, 2, 0, 7}); IS_TRUE(f.mqtt.loop());
    f.socket.queue({0x34, 6, 0, 1, 't', 0, 7, 'y'});
    IS_TRUE(f.mqtt.loop()); IS_EQUAL(calls, 2u);
    END_IT
}

static int qos2_capacity(bool incremental) {
    IT("incoming QoS2 capacity exhaustion closes before delivering an untracked message");
    Fixture f(incremental); IS_TRUE(f.connect());
    for (size_t i = 1; i <= MQTT_RX_QOS2_MAX_INFLIGHT; ++i) {
        f.socket.queue({0x34, 6, 0, 1, 't', (uint8_t)(i >> 8), (uint8_t)i, 'x'});
        IS_TRUE(f.mqtt.loop());
    }
    const size_t delivered = calls;
    IS_EQUAL(delivered, (size_t)MQTT_RX_QOS2_MAX_INFLIGHT);
    // A duplicate at capacity must be acknowledged without taking another slot.
    f.socket.queue({0x3c, 6, 0, 1, 't', 0, 1, 'x'});
    IS_TRUE(f.mqtt.loop()); IS_EQUAL(calls, delivered);
    // Release one slot, then fill it with a distinct identifier.
    f.socket.queue({0x62, 2, 0, 1}); IS_TRUE(f.mqtt.loop());
    const size_t next = MQTT_RX_QOS2_MAX_INFLIGHT + 1u;
    f.socket.queue({0x34, 6, 0, 1, 't', (uint8_t)(next >> 8), (uint8_t)next, 'x'});
    IS_TRUE(f.mqtt.loop()); IS_EQUAL(calls, delivered + 1);
    const size_t wireBefore = f.socket.wire.size();
    f.socket.queue({0x34, 6, 0, 1, 't', 0x7f, 0xfe, 'x'});
    IS_FALSE(f.mqtt.loop()); IS_EQUAL(calls, delivered + 1);
    IS_EQUAL(f.socket.wire.size(), wireBefore); // no PUBREC for the untracked message
    IS_FALSE(f.mqtt.connected());
    END_IT
}

static int qos2_session() {
    IT("QoS2 IDs survive only a confirmed persistent session for the same client and broker");
    Fixture f(true); IS_TRUE(f.connect("persistent", false));
    f.socket.queue({0x34, 6, 0, 1, 't', 0, 7, 'x'});
    IS_TRUE(f.mqtt.loop()); IS_EQUAL(calls, 1u);
    f.socket.stop();
    f.mqtt.setServer("broker", 1883); f.mqtt.setClient(f.socket);
    IS_TRUE(f.connect("persistent", false, true));
    f.socket.queue({0x3c, 6, 0, 1, 't', 0, 7, 'x'});
    IS_TRUE(f.mqtt.loop());
#if MQTT_VERSION == MQTT_VERSION_3_1_1
    IS_EQUAL(calls, 1u);
#else
    IS_EQUAL(calls, 2u);
#endif
    f.socket.stop();
    IS_TRUE(f.connect("persistent", false, false));
    const size_t before = calls;
    f.socket.queue({0x34, 6, 0, 1, 't', 0, 7, 'x'});
    IS_TRUE(f.mqtt.loop()); IS_EQUAL(calls, before + 1);
    f.socket.stop(); IS_TRUE(f.connect("other", false, true));
    f.socket.queue({0x34, 6, 0, 1, 't', 0, 7, 'x'});
    IS_TRUE(f.mqtt.loop()); IS_EQUAL(calls, before + 2);
    f.socket.stop(); f.mqtt.setServer("other-broker", 1883);
    IS_TRUE(f.connect("other", false, true));
    f.socket.queue({0x34, 6, 0, 1, 't', 0, 7, 'x'});
    IS_TRUE(f.mqtt.loop()); IS_EQUAL(calls, before + 3);
    END_IT
}

static int callback_publish(bool incremental) {
    IT("a callback can publish synchronously before the local ACK is sent");
    Fixture f(incremental); IS_TRUE(f.connect()); publishInCallback = true;
    f.socket.queue({0x32, 6, 0, 1, 't', 0, 7, 'x'});
    IS_TRUE(f.mqtt.loop()); IS_TRUE(callbackChecks);
    IS_EQUAL(f.socket.wire[f.socket.wire.size() - 4], MQTTPUBACK);
    IS_EQUAL(f.socket.wire.back(), 7u);
    END_IT
}

static int unfinished_callback_publish(bool incremental) {
    IT("an unfinished callback PUBLISH closes without inserting a PUBACK");
    Fixture f(incremental); IS_TRUE(f.connect()); incompleteInCallback = true;
    f.socket.queue({0x32, 6, 0, 1, 't', 0, 7, 'x'});
    IS_FALSE(f.mqtt.loop()); IS_TRUE(callbackChecks);
    IS_FALSE(f.mqtt.connected()); IS_EQUAL(f.mqtt.state(), MQTT_CONNECTION_LOST);
    IS_EQUAL(f.socket.wire.size(), 9u); // only header + topic 'reply'
    END_IT
}

static int manual_transport_loss() {
    IT("connect observes a transport loss during manual PUBLISH before starting a fresh session");
    Fixture f(true); IS_TRUE(f.connect());
    IS_TRUE(f.mqtt.beginPublish("t", 2, false));
    f.socket.stop();
    // Fixture.connect() intentionally observes connected(); exercise direct connect here instead.
    // An already-closed transport must be noticed even though a TX transaction was pending.
    IS_FALSE(f.mqtt.connect("test")); // no CONNACK: attempt fails, but must clear old TX state
    IS_EQUAL(f.mqtt.state(), MQTT_CONNECTION_TIMEOUT);
    IS_TRUE(f.connect());
    IS_TRUE(f.mqtt.publish("t", "ok"));
    END_IT
}

static int topic_limits() {
    IT("empty and over-65535 topics are rejected, including flash paths and CONNECT strings");
    Fixture f(false); IS_TRUE(f.connect());
    IS_FALSE(f.mqtt.subscribe("")); IS_FALSE(f.mqtt.unsubscribe(""));
    IS_FALSE(f.mqtt.subscribe(F(""))); IS_FALSE(f.mqtt.unsubscribe(F("")));
    IS_TRUE(f.mqtt.setBufferSize(66000));
    const std::string tooLong(65536, 'a');
    IS_FALSE(f.mqtt.subscribe(tooLong.c_str())); IS_FALSE(f.mqtt.unsubscribe(tooLong.c_str()));
    IS_FALSE(f.mqtt.beginPublish(tooLong.c_str(), 0, false));
    IS_FALSE(f.mqtt.subscribe(reinterpret_cast<const __FlashStringHelper*>(tooLong.c_str())));
    IS_TRUE(f.socket.wire.empty());
    const std::string largest(65535, 'a');
    IS_TRUE(f.mqtt.subscribe(largest.c_str()));
    f.mqtt.disconnect();
    IS_FALSE(f.connect(tooLong.c_str())); IS_TRUE(f.socket.wire.empty());
    END_IT
}

// A scoped clock keeps deadline checks deterministic without changing firmware code.
struct TestClock {
    TestClock() { setTestMillis(100, 1); }
    ~TestClock() { resetTestMillis(); }
};

static int control_response_writes(bool incremental) {
    IT("all five required replies use one attempt in incremental mode and retry in blocking mode");
    TestClock clock;
    for (int packet = 0; packet < 5; ++packet) {
        // Complete write, recoverable short write, stalled partial write, and zero-byte write.
        for (int outcome = 0; outcome < 4; ++outcome) {
            Fixture f(incremental); IS_TRUE(f.connect());
            const size_t length = packet == 2 ? 2u : 4u;
            const uint8_t responses[] = {0x62, 0x70, 0xd0, 0x40, 0x50};
            const uint8_t response = responses[packet];
            if (outcome == 1 || outcome == 2) f.socket.setWriteChunk(1);
            if (outcome >= 2) f.socket.setWriteLimit(outcome == 2 ? 1 : 0);
            if (packet == 0) f.socket.queue({0x50, 2, 0x12, 0x34});
            if (packet == 1) f.socket.queue({0x62, 2, 0x12, 0x34});
            if (packet == 2) f.socket.queue({0xc0, 0});
            if (packet == 3) f.socket.queue({0x32, 6, 0, 1, 't', 0x12, 0x34, 'x'});
            if (packet == 4) f.socket.queue({0x34, 6, 0, 1, 't', 0x12, 0x34, 'x'});
            const size_t writesBefore = f.socket.writeCalls;
            const unsigned long timeBefore = peekTestMillis();
            if (outcome == 0 || (outcome == 1 && !incremental)) {
                IS_TRUE(f.mqtt.loop()); IS_TRUE(f.mqtt.connected());
                const std::vector<uint8_t> expected = packet == 2
                    ? std::vector<uint8_t>{response, 0}
                    : std::vector<uint8_t>{response, 2, 0x12, 0x34};
                IS_TRUE(f.socket.wire == expected);
                IS_EQUAL(f.socket.writeCalls - writesBefore, (outcome == 0 ? 1u : length));
            } else {
                IS_FALSE(f.mqtt.loop()); IS_FALSE(f.mqtt.connected());
                IS_EQUAL(f.mqtt.state(), MQTT_CONNECTION_LOST);
                IS_EQUAL(f.socket.wire.size(), (outcome == 3 ? 0u : 1u));
                IS_EQUAL(f.socket.stopCalls, 1u); // helper and caller must not both close
                if (incremental) {
                    IS_EQUAL(peekTestMillis(), f.socket.timeAtWrite);
                    IS_EQUAL(getTestYieldCalls(), f.socket.yieldsAtWrite);
                }
                if (incremental || outcome == 3) {
                    IS_EQUAL(f.socket.writeCalls - writesBefore, 1u);
                } else {
                    IS_TRUE(f.socket.writeCalls - writesBefore > 1u);
                    IS_TRUE(peekTestMillis() - timeBefore >= 1000UL);
                }
                f.socket.setWriteLimit(1024);
                IS_TRUE(f.connect());
                const size_t callbacksBefore = calls;
                f.socket.queue({0x30, 4, 0, 1, 't', 'x'});
                IS_TRUE(f.mqtt.loop()); IS_EQUAL(calls, callbacksBefore + 1);
            }
        }
    }
    END_IT
}

static int keepalive_write_policy(bool incremental) {
    IT("PINGREQ follows receive mode; zero bytes defer the ping without arming its timeout");
    TestClock clock;
    for (int outcome = 0; outcome < 4; ++outcome) {
        Fixture f(incremental); IS_TRUE(f.connect());
        f.mqtt.setKeepAlive(1);
        if (outcome == 1 || outcome == 2) f.socket.setWriteChunk(1);
        if (outcome >= 2) f.socket.setWriteLimit(outcome == 2 ? 1 : 0);
        setTestMillis(peekTestMillis() + 2000UL, 1);
        const size_t writesBefore = f.socket.writeCalls;
        if (outcome == 0 || (outcome == 1 && !incremental)) {
            IS_TRUE(f.mqtt.loop()); IS_TRUE(f.mqtt.connected());
            const std::vector<uint8_t> expected = {0xc0, 0};
            IS_TRUE(f.socket.wire == expected);
            IS_EQUAL(f.socket.writeCalls - writesBefore, (outcome == 0 ? 1u : 2u));
            // This PINGRESP must clear the outstanding ping before the next deadline.
            f.socket.queue({0xd0, 0}); IS_TRUE(f.mqtt.loop());
            setTestMillis(peekTestMillis() + 2000UL, 1);
            IS_TRUE(f.mqtt.loop()); IS_TRUE(f.mqtt.connected());
        } else if (outcome == 3) {
            IS_TRUE(f.mqtt.loop()); IS_TRUE(f.mqtt.connected());
            IS_TRUE(f.socket.wire.empty());
            IS_EQUAL(f.socket.writeCalls - writesBefore, 1u);
            f.socket.setWriteLimit(1024);
            setTestMillis(peekTestMillis() + 2000UL, 1);
            // An incorrectly armed ping would close the connection here.
            IS_TRUE(f.mqtt.loop()); IS_TRUE(f.mqtt.connected());
            const std::vector<uint8_t> expected = {0xc0, 0};
            IS_TRUE(f.socket.wire == expected);
            IS_EQUAL(f.socket.writeCalls - writesBefore, 2u);
        } else {
            f.socket.queue({0x30, 4, 0, 1, 't', 'x'});
            IS_FALSE(f.mqtt.loop()); IS_FALSE(f.mqtt.connected());
            IS_EQUAL(f.mqtt.state(), MQTT_CONNECTION_LOST);
            IS_EQUAL(calls, 0u); // do not dispatch RX after failing to send the ping
            IS_EQUAL(f.socket.wire.size(), 1u);
            IS_EQUAL(f.socket.stopCalls, 1u);
            if (incremental) {
                IS_EQUAL(peekTestMillis(), f.socket.timeAtWrite);
                IS_EQUAL(getTestYieldCalls(), f.socket.yieldsAtWrite);
                IS_EQUAL(f.socket.writeCalls - writesBefore, 1u);
            } else {
                IS_TRUE(f.socket.writeCalls - writesBefore > 1u);
            }
            f.socket.setWriteLimit(1024);
            IS_TRUE(f.connect());
            f.socket.queue({0x30, 4, 0, 1, 't', 'x'});
            IS_TRUE(f.mqtt.loop()); IS_EQUAL(calls, 1u);
        }
    }
    END_IT
}

static int socket_timeout_defaults(bool incremental) {
    IT("socket timeout defaults, explicit values and zero fallback also bound packet reads");
    TestClock clock;
    for (int setting = 0; setting < 4; ++setting) {
        WireClient socket;
        PubSubClient mqtt("broker", 1883, received, socket);
        mqtt.setKeepAlive(0);
        if (incremental) IS_TRUE(mqtt.setRxBufferSize(64));
        if (setting == 1) mqtt.setSocketTimeout(2);
        if (setting == 2) mqtt.setSocketTimeout(2).setSocketTimeout(0);
        if (setting == 3) mqtt.setSocketTimeout(2).setPacketTimeout(1).setPacketTimeout(0);
        socket.queue({0x20, 2, 0, 0});
        IS_TRUE(mqtt.connect("timeout"));
        const unsigned long timeout = (setting == 1 || setting == 3) ? 2000UL : MQTT_SOCKET_TIMEOUT * 1000UL;
        setTestMillis(100, incremental ? 0 : 1);
        socket.queue({0x30, 0x80}); // incomplete remaining-length field
        if (incremental) {
            IS_TRUE(mqtt.loop());
            setTestMillis(100 + timeout - 1, 0);
            IS_TRUE(mqtt.loop());
            setTestMillis(100 + timeout, 0);
        }
        IS_FALSE(mqtt.loop());
        IS_EQUAL(mqtt.state(), MQTT_CONNECTION_TIMEOUT);
        if (!incremental) {
            IS_TRUE(peekTestMillis() >= 100 + timeout);
            IS_TRUE(peekTestMillis() < 100 + timeout + 20);
        }
        setTestMillis(100, 1);
    }
    // Zero must restore a positive retry budget, not immediately fail a short write.
    Fixture f(incremental); IS_TRUE(f.connect());
    f.mqtt.setSocketTimeout(2).setSocketTimeout(0);
    f.socket.setWriteChunk(1);
    IS_TRUE(f.mqtt.publish("t", "ok")); IS_TRUE(f.mqtt.connected());
    END_IT
}

int main(int argc, char** argv) {
    SUITE("Session, memory and packet integrity regressions");
    if (argc > 1 && strcmp(argv[1], "--callback-only") == 0) {
        callback_lifetime(false);
        FINISH
    }
    ownership(); blocking_timeout(); reconnect_resets_rx(); publish_lengths(); length_overflow(); topic_limits(); qos2_session(); manual_transport_loss();
    for (int mode = 0; mode < 2; ++mode) {
        if (argc == 1) callback_lifetime(mode != 0);
        malformed_qos_recovers(mode != 0); manual_publish(mode != 0);
        incoming_budget(mode != 0); qos2_dedup(mode != 0); qos2_capacity(mode != 0); callback_publish(mode != 0);
        unfinished_callback_publish(mode != 0);
        control_response_writes(mode != 0); keepalive_write_policy(mode != 0);
        socket_timeout_defaults(mode != 0);
    }
    FINISH
}
