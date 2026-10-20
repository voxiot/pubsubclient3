#include "ShimClient.h"

#include <Arduino.h>

#include <ctime>
#include <iostream>

#include "trace.h"

namespace {
bool testClockEnabled = false;
unsigned long testClockValue = 0;
unsigned long testClockStep = 0;
unsigned long testYieldCalls = 0;
}
extern "C" {
void setTestMillis(unsigned long value, unsigned long step) {
    testClockEnabled = true;
    testClockValue = value;
    testClockStep = step;
}
unsigned long peekTestMillis(void) { return testClockValue; }
void resetTestMillis(void) { testClockEnabled = false; }
void recordTestYield(void) { ++testYieldCalls; }
unsigned long getTestYieldCalls(void) { return testYieldCalls; }
unsigned long millis(void) {
    if (testClockEnabled) {
        const unsigned long value = testClockValue;
        testClockValue += testClockStep;
        return value;
    }
    return (unsigned long)time(0) * 1000UL;
}
}

ShimClient::ShimClient() {
    this->responseBuffer = new Buffer();
    this->expectBuffer = new Buffer();
    this->_allowConnect = true;
    this->_connected = false;
    this->_error = false;
    this->_writeLimited = false;
    this->_writeLimit = 0;
    this->_writeChunk = 0;
    this->expectAnything = true;
    this->_received = 0;
    this->_expectedPort = 0;
}

int ShimClient::connect(IPAddress ip, uint16_t port) {
    if (this->_allowConnect) {
        this->_connected = true;
    }
    if (this->_expectedPort != 0) {
        if (memcmp(&ip, &this->_expectedIP, 4) != 0) {
            TRACE("ip mismatch\n");
            this->_error = true;
        }
        if (port != this->_expectedPort) {
            TRACE("port mismatch\n");
            this->_error = true;
        }
    }
    return this->_connected;
}
int ShimClient::connect(const char *host, uint16_t port) {
    if (this->_allowConnect) {
        this->_connected = true;
    }
    if (this->_expectedPort != 0) {
        if (strcmp(host, this->_expectedHost) != 0) {
            TRACE("host mismatch\n");
            this->_error = true;
        }
        if (port != this->_expectedPort) {
            TRACE("port mismatch\n");
            this->_error = true;
        }
    }
    return this->_connected;
}
size_t ShimClient::write(uint8_t b) {
    if (this->_writeLimited) {
        if (this->_writeLimit == 0) {
            TRACE("write limit reached\n");
            return 0;
        }
        this->_writeLimit--;
    }
    this->_received += 1;
    TRACE(std::hex << (unsigned int)b);
    if (!this->expectAnything) {
        if (this->expectBuffer->available()) {
            uint8_t expected = this->expectBuffer->next();
            if (expected != b) {
                this->_error = true;
                TRACE("!=" << (unsigned int)expected);
            }
        } else {
            this->_error = true;
        }
    }
    TRACE("\n" << std::dec);
    return 1;
}
size_t ShimClient::write(const uint8_t *buf, size_t size) {
    if ((this->_writeChunk > 0) && (size > this->_writeChunk)) {
        TRACE("write chunked to " << std::dec << (unsigned int)(this->_writeChunk) << " of " << (unsigned int)(size) << "\n");
        size = this->_writeChunk;
    }
    if (this->_writeLimited) {
        if (size > this->_writeLimit) {
            TRACE("write limited to " << std::dec << (unsigned int)(this->_writeLimit) << " of " << (unsigned int)(size) << "\n");
            size = this->_writeLimit;
        }
        this->_writeLimit -= size;
        if (size == 0) {
            return 0;
        }
    }
    this->_received += size;
    TRACE("[" << std::dec << (unsigned int)(size) << "] ");
    size_t i = 0;
    for (; i < size; i++) {
        if (i > 0) {
            TRACE(":");
        }
        TRACE(std::hex << (unsigned int)(buf[i]));

        if (!this->expectAnything) {
            if (this->expectBuffer->available()) {
                uint8_t expected = this->expectBuffer->next();
                if (expected != buf[i]) {
                    this->_error = true;
                    TRACE("!=" << (unsigned int)expected);
                }
            } else {
                this->_error = true;
            }
        }
    }
    TRACE("\n" << std::dec);
    return size;
}
int ShimClient::available() {
    return this->responseBuffer->available();
}
int ShimClient::read() {
    return this->responseBuffer->next();
}
int ShimClient::read(uint8_t *buf, size_t size) {
    size_t i = 0;
    for (; i < size; i++) {
        buf[i] = this->read();
    }
    return size;
}
int ShimClient::peek() {
    return 0;
}
void ShimClient::flush() {}
void ShimClient::stop() {
    this->setConnected(false);
}
uint8_t ShimClient::connected() {
    return this->_connected;
}
ShimClient::operator bool() {
    return true;
}

ShimClient *ShimClient::respond(uint8_t *buf, size_t size) {
    this->responseBuffer->add(buf, size);
    return this;
}

ShimClient *ShimClient::expect(uint8_t *buf, size_t size) {
    this->expectAnything = false;
    this->expectBuffer->add(buf, size);
    return this;
}

void ShimClient::setConnected(bool b) {
    this->_connected = b;
}
void ShimClient::setAllowConnect(bool b) {
    this->_allowConnect = b;
}
void ShimClient::setWriteLimit(size_t bytes) {
    this->_writeLimited = true;
    this->_writeLimit = bytes;
}
void ShimClient::setWriteChunk(size_t bytes) {
    this->_writeChunk = bytes;
}

bool ShimClient::error() {
    return this->_error;
}

uint16_t ShimClient::received() {
    return this->_received;
}

void ShimClient::expectConnect(IPAddress ip, uint16_t port) {
    this->_expectedIP = ip;
    this->_expectedPort = port;
}

void ShimClient::expectConnect(const char *host, uint16_t port) {
    this->_expectedHost = host;
    this->_expectedPort = port;
}
