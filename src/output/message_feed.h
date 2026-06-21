// Decoded-message output feed. Emits ACARS and EGC messages as newline-
// delimited JSON (JAERO JSONdump / inmarsat-sniffer compatible) and/or the
// JAERO text format, to a file and/or UDP endpoint. Thread-safe.
#pragma once

#include "decode/message_log.h"

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

class MessageFeed
{
public:
    enum Format { JSON = 0, JAERO_TEXT = 1 };

    ~MessageFeed();

    // Configuration (safe to call live).
    void setFileEnabled(bool on, const std::string& path);
    void setUdpEnabled(bool on, const std::string& host, int port);
    void setFormat(int fmt) { format_ = fmt; }
    void setStationId(const std::string& s) { station_ = s; }

    bool enabled() const { return fileEnabled_ || udpEnabled_; }
    uint64_t sent() const { return sent_; }

    void feedAcars(const DecodedMessage& m);
    void feedEgc(const EgcMessage& m);

private:
    void emit(const std::string& line);
    void openFileIfNeeded();
    void closeFile();
    void ensureUdp();
    void closeUdp();

    std::mutex mtx_;
    bool fileEnabled_ = false;
    std::string filePath_;
    std::FILE* file_ = nullptr;

    bool udpEnabled_ = false;
    std::string udpHost_;
    int udpPort_ = 0;
    uintptr_t sock_ = ~(uintptr_t)0;
    void* addr_ = nullptr; // sockaddr_in*

    int format_ = JSON;
    std::string station_;
    uint64_t sent_ = 0;
};
