// Thread-safe log of decoded messages for the Messages panel.
#pragma once

#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct DecodedMessage
{
    double timeSec = 0.0;
    int channelId = 0;
    double freqMHz = 0.0;
    uint32_t aesId = 0;
    uint8_t gesId = 0;
    int downlink = 0;
    std::string reg;   // aircraft registration (ACARS)
    std::string label; // ACARS label
    char mode = 0;     // ACARS mode char
    char blockId = 0;  // ACARS block id char
    std::string text;  // printable rendering of the payload
    std::string hex;   // hex rendering
};

class MessageLog
{
public:
    void add(const DecodedMessage& m)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        msgs_.push_back(m);
        if (msgs_.size() > kMax)
            msgs_.erase(msgs_.begin(), msgs_.begin() + (msgs_.size() - kMax));
        ++count_;
    }

    // Copy a snapshot for rendering.
    std::vector<DecodedMessage> snapshot()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return msgs_;
    }

    uint64_t count()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return count_;
    }

    void clear()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        msgs_.clear();
    }

private:
    static constexpr size_t kMax = 2000;
    std::mutex mtx_;
    std::vector<DecodedMessage> msgs_;
    uint64_t count_ = 0;
};

// A decoded C-channel (voice/data) assignment.
struct CassignEntry
{
    int channelId = 0;
    uint8_t type = 0;      // 0x31 distress .. 0x34 non-safety
    uint32_t aesId = 0;
    uint8_t gesId = 0;
    double rxMHz = 0.0;    // aircraft receive (forward/downlink)
    double txMHz = 0.0;    // aircraft transmit (return/uplink)
};

class CassignLog
{
public:
    void add(const CassignEntry& e)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        items_.push_back(e);
        if (items_.size() > kMax)
            items_.erase(items_.begin(), items_.begin() + (items_.size() - kMax));
        ++count_;
    }

    std::vector<CassignEntry> snapshot()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return items_;
    }

    uint64_t count()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return count_;
    }

    void clear()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        items_.clear();
    }

private:
    static constexpr size_t kMax = 1000;
    std::mutex mtx_;
    std::vector<CassignEntry> items_;
    uint64_t count_ = 0;
};

// A decoded Inmarsat-C / EGC message (SafetyNET, FleetNET, system).
struct EgcMessage
{
    int channelId = 0;
    double freqMHz = 0.0;
    int frameNumber = 0;
    std::string timeUtc;  // HH:MM:SS from frame number
    std::string service;  // service code + address name
    std::string priority; // Routine/Safety/Urgency/Distress
    int messageId = 0;
    int presentation = 0; // 0=IA5, 6=ITA2, 7=8-bit
    std::string text;     // decoded message text
};

class EgcLog
{
public:
    void add(const EgcMessage& m)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        msgs_.push_back(m);
        if (msgs_.size() > kMax)
            msgs_.erase(msgs_.begin(), msgs_.begin() + (msgs_.size() - kMax));
        ++count_;
    }
    std::vector<EgcMessage> snapshot()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return msgs_;
    }
    uint64_t count()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return count_;
    }
    void clear()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        msgs_.clear();
    }

private:
    static constexpr size_t kMax = 2000;
    std::mutex mtx_;
    std::vector<EgcMessage> msgs_;
    uint64_t count_ = 0;
};

// A channel discovered from the network's own system-table broadcasts.
struct NetworkChannel
{
    double freqMHz = 0.0;
    std::string kind; // "Psmc (P-ch RX)", "Rsmc0 (R-ch TX)", "CAC", ...
    uint8_t ges = 0;
    uint64_t hits = 0;
    bool decodable = false; // forward-link channel we can demod
    int baud = 0;           // suggested decoder baud (0 if not decodable)
};

struct SatInfo
{
    bool valid = false;
    int satId = 0;
    std::string longitude;
};

// Network/channel map built from AES system-table SUs (0x05 Psmc/Rsmc, 0x0C
// satellite ID). Channels are de-duplicated by frequency + kind.
class ChannelTable
{
public:
    void addChannel(double freqMHz, const std::string& kind, uint8_t ges,
                    bool decodable, int baud)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto& c : chans_)
            if (std::abs(c.freqMHz - freqMHz) < 0.0005 && c.kind == kind)
            {
                c.hits++;
                c.ges = ges;
                return;
            }
        chans_.push_back({freqMHz, kind, ges, 1, decodable, baud});
    }

    void setSatellite(int satId, const std::string& longitude)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        sat_.valid = true;
        sat_.satId = satId;
        sat_.longitude = longitude;
    }

    std::vector<NetworkChannel> snapshot()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return chans_;
    }

    SatInfo satellite()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return sat_;
    }

    void clear()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        chans_.clear();
        sat_ = SatInfo{};
    }

private:
    std::mutex mtx_;
    std::vector<NetworkChannel> chans_;
    SatInfo sat_;
};
