// Decoded-message output feed. Emits ACARS and EGC messages as newline-
// delimited JSON (JAERO JSONdump / inmarsat-sniffer compatible) and/or the
// JAERO text format, to a file and/or UDP endpoint. Thread-safe.
#pragma once

#include "decode/message_log.h"

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

class MessageFeed
{
public:
    enum Format { JSON = 0, JAERO_TEXT = 1 };

    ~MessageFeed();

    // Configuration (safe to call live).
    void setFileEnabled(bool on, const std::string& path);
    void setUdpEnabled(bool on, const std::string& host, int port);
    // SBS / BaseStation (port 30003) position feed. We act as a TCP *server*
    // (listener); Virtual Radar Server / tar1090 connect to us as clients. Each
    // ACARS message carrying an ADS-C position is broadcast as a MSG,3 line.
    void setSbsEnabled(bool on, int port);
    void setFormat(int fmt) { format_ = fmt; }
    void setStationId(const std::string& s) { station_ = s; }

    bool enabled() const { return fileEnabled_ || udpEnabled_ || sbsEnabled_; }
    uint64_t sent() const { return sent_; }
    uint64_t sbsSent() const { return sbsSent_; }
    int sbsClients() const { return (int)sbsClients_.size(); }
    bool sbsListening() const { return sbsListen_ != ~(uintptr_t)0; }
    // Accept newly-connected clients / prune dropped ones (call each frame).
    void pollSbs();

    void feedAcars(const DecodedMessage& m);
    void feedEgc(const EgcMessage& m);
    void feedLes(const LesMessage& m);

private:
    void emit(const std::string& line);
    void emitSbs(const DecodedMessage& m);
    void openFileIfNeeded();
    void closeFile();
    void ensureUdp();
    void closeUdp();
    void ensureSbsListen();
    void acceptSbsClients();
    void closeSbs();

    std::mutex mtx_;
    bool fileEnabled_ = false;
    std::string filePath_;
    std::FILE* file_ = nullptr;

    bool udpEnabled_ = false;
    std::string udpHost_;
    int udpPort_ = 0;
    uintptr_t sock_ = ~(uintptr_t)0;
    void* addr_ = nullptr; // sockaddr_in*

    bool sbsEnabled_ = false;
    int sbsPort_ = 0;
    uintptr_t sbsListen_ = ~(uintptr_t)0;       // listening TCP socket
    std::vector<uintptr_t> sbsClients_;         // connected client sockets
    uint64_t sbsSent_ = 0;

    int format_ = JSON;
    std::string station_;
    uint64_t sent_ = 0;
};
