#ifndef shimclient_h
#define shimclient_h

#include "Arduino.h"
#include "Buffer.h"
#include "Client.h"
#include "IPAddress.h"

class ShimClient : public Client {
   private:
    Buffer *responseBuffer;
    Buffer *expectBuffer;
    bool _allowConnect;
    bool _connected;
    bool expectAnything;
    bool _error;
    bool _writeLimited;
    size_t _writeLimit;
    size_t _writeChunk;
    uint16_t _received;
    IPAddress _expectedIP;
    uint16_t _expectedPort;
    const char *_expectedHost;

   public:
    ShimClient();
    virtual int connect(IPAddress ip, uint16_t port);
    virtual int connect(const char *host, uint16_t port);
    virtual size_t write(uint8_t);
    virtual size_t write(const uint8_t *buf, size_t size);
    virtual int available();
    virtual int read();
    virtual int read(uint8_t *buf, size_t size);
    virtual int peek();
    virtual void flush();
    virtual void stop();
    virtual uint8_t connected();
    virtual operator bool();

    virtual ShimClient *respond(uint8_t *buf, size_t size);
    virtual ShimClient *expect(uint8_t *buf, size_t size);

    virtual void expectConnect(IPAddress ip, uint16_t port);
    virtual void expectConnect(const char *host, uint16_t port);

    virtual uint16_t received();
    virtual bool error();

    virtual void setAllowConnect(bool b);
    virtual void setConnected(bool b);

    // Accept at most 'bytes' more bytes in total; any write beyond that returns short (or 0), as a
    // real socket does when the send times out mid-record. Used to test partial-write handling.
    virtual void setWriteLimit(size_t bytes);

    // Accept at most 'bytes' per write() call, with no total limit: every write returns short but the
    // data always gets through if the caller writes the remainder. Models a full transmit window.
    virtual void setWriteChunk(size_t bytes);
};

#endif
