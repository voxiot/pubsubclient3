#include "BDDTest.h"
#include "PubSubClient.h"
#include "ShimClient.h"
#include <algorithm>
#include <deque>
#include <vector>

namespace {
using Bytes = std::vector<uint8_t>;

// The script describes accepted bytes per call, not requested lengths. stop()
// discards unread input, and a closed socket can never append to the old wire.
class ScriptedClient : public ShimClient {
 public:
    std::deque<size_t> accepts;
    std::deque<uint8_t> input;
    Bytes wire;
    size_t fallback = 4096;
    size_t stops = 0;
    size_t writes = 0;
    using ShimClient::write;
    using ShimClient::read;
    size_t write(const uint8_t* data, size_t length) override {
        ++writes;
        if (!ShimClient::connected()) return 0;
        const size_t cap = accepts.empty() ? fallback : accepts.front();
        if (!accepts.empty()) accepts.pop_front();
        const size_t n = std::min(length, cap);
        wire.insert(wire.end(), data, data + n);
        return n;
    }
    int available() override { return static_cast<int>(input.size()); }
    int read() override {
        if (input.empty()) return -1;
        const int b = input.front(); input.pop_front(); return b;
    }
    void stop() override { ++stops; input.clear(); ShimClient::stop(); }
    void queue(std::initializer_list<uint8_t> bytes) { input.insert(input.end(), bytes); }
    void varying(uint32_t seed) {
        accepts.clear(); fallback = 4096; accepts.push_back(1);
        for (unsigned i = 0; i < 80; ++i) {
            seed = seed * 1664525u + 1013904223u;
            accepts.push_back(i % 3u == 0u ? 0u : 1u + seed % 11u);
        }
    }
};

struct Fixture {
    ScriptedClient socket;
    PubSubClient mqtt;
    explicit Fixture(bool incremental) : mqtt(socket) {
        setTestMillis(100, 1);
        socket.setAllowConnect(true);
        mqtt.setServer("broker", 1883); mqtt.setSocketTimeout(1); mqtt.setKeepAlive(0);
#if MQTT_ENABLE_INCREMENTAL_RX
        if (incremental) mqtt.setRxBufferSize(64);
#else
        (void)incremental;
#endif
    }
    ~Fixture() { resetTestMillis(); }
    bool connect() {
        socket.accepts.clear(); socket.fallback = 4096;
        socket.queue({0x20, 2, 0, 0});
        const bool ok = mqtt.connect("sequence"); socket.wire.clear(); return ok;
    }
};

const Bytes subscription = {0x82, 8, 0, 2, 0, 3, 'c', 'm', 'd', 1};
const Bytes unsubscription = {0xa2, 7, 0, 3, 0, 3, 'c', 'm', 'd'};
const Bytes tail = {0x30, 8, 0, 4, 't', 'a', 'i', 'l', 'o', 'k'};
Bytes largePublish() {
    Bytes bytes = {0x30, 0xba, 1, 0, 4, 'd', 'a', 't', 'a'};
    bytes.insert(bytes.end(), 180, 'x'); return bytes;
}
void append(Bytes& into, const Bytes& bytes) { into.insert(into.end(), bytes.begin(), bytes.end()); }

int recovered_sequence(bool incremental) {
    IT("preserves the exact PUBLISH/SUBSCRIBE/PING/UNSUBSCRIBE stream under seeded short/zero writes");
    for (uint32_t seed = 1; seed <= 64; ++seed) {
        Fixture f(incremental); IS_TRUE(f.connect());
        Bytes expected = largePublish();
        const Bytes payload(180, 'x');
        f.socket.varying(seed);
        IS_TRUE(f.mqtt.publish("data", payload.data(), payload.size()));
        IS_TRUE(f.socket.wire == expected);
        f.socket.varying(seed + 64);
        IS_TRUE(f.mqtt.subscribe("cmd", MQTT_QOS1)); append(expected, subscription);
        IS_TRUE(f.socket.wire == expected);
        f.mqtt.setKeepAlive(1);
        setTestMillis(peekTestMillis() + 2000, 1);
        f.socket.varying(seed + 128);
        // Incremental automatic responses deliberately allow one transport attempt.
        if (incremental) { f.socket.accepts.clear(); f.socket.accepts.push_back(2); }
        IS_TRUE(f.mqtt.loop()); append(expected, Bytes{0xc0, 0});
        IS_TRUE(f.socket.wire == expected);
        f.socket.queue({0xd0, 0}); IS_TRUE(f.mqtt.loop());
        f.mqtt.setKeepAlive(0);
        f.socket.varying(seed + 192);
        IS_TRUE(f.mqtt.unsubscribe("cmd")); append(expected, unsubscription);
        f.socket.varying(seed + 256);
        IS_TRUE(f.mqtt.publish("tail", "ok")); append(expected, tail);
        IS_TRUE(f.socket.wire == expected);
        IS_EQUAL(f.socket.stops, 0u); IS_EQUAL(f.mqtt.state(), MQTT_CONNECTED);
    }
    END_IT
}

int interrupted_sequence(bool incremental) {
    IT("closes an interrupted packet before subsequent packets and resumes only on a fresh connection");
    for (unsigned stage = 0; stage < 5; ++stage) {
        Fixture f(incremental); IS_TRUE(f.connect());
        f.socket.fallback = 0;
        f.socket.accepts = stage == 1 ? std::deque<size_t>{4096, 2} : std::deque<size_t>{1};
        Bytes expected;
        bool ok;
        if (stage < 2) {
            const Bytes payload(180, 'x');
            ok = f.mqtt.publish("data", payload.data(), payload.size());
            expected = largePublish(); expected.resize(stage == 0 ? 1 : 11);
        } else if (stage == 2) {
            ok = f.mqtt.subscribe("cmd", MQTT_QOS1); expected = {0x82};
        } else if (stage == 3) {
            ok = f.mqtt.unsubscribe("cmd"); expected = {0xa2};
        } else {
            f.mqtt.setKeepAlive(1); setTestMillis(peekTestMillis() + 2000, 1);
            ok = f.mqtt.loop(); expected = {0xc0};
        }
        IS_FALSE(ok); IS_TRUE(f.socket.wire == expected);
        IS_EQUAL(f.mqtt.state(), MQTT_WRITE_INCOMPLETE); IS_EQUAL(f.socket.stops, 1u);
        const size_t writes = f.socket.writes;
        IS_FALSE(f.mqtt.publish("tail", "ok")); IS_FALSE(f.mqtt.subscribe("next"));
        IS_FALSE(f.mqtt.loop()); IS_EQUAL(f.socket.writes, writes);
        IS_TRUE(f.socket.wire == expected); IS_EQUAL(f.socket.stops, 1u);
        IS_EQUAL(f.mqtt.state(), MQTT_WRITE_INCOMPLETE);
        f.mqtt.setKeepAlive(0); IS_TRUE(f.connect());
        IS_TRUE(f.mqtt.publish("tail", "ok")); IS_TRUE(f.socket.wire == tail);
        IS_EQUAL(f.mqtt.state(), MQTT_CONNECTED);
    }
    END_IT
}
} // namespace

int main() {
    SUITE("Outgoing packet sequence integrity");
    recovered_sequence(false); interrupted_sequence(false);
#if MQTT_ENABLE_INCREMENTAL_RX
    recovered_sequence(true); interrupted_sequence(true);
#endif
    FINISH
}
