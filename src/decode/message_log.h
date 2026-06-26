// Thread-safe log of decoded messages for the Messages panel.
#pragma once

#include <cmath>
#include <cstdint>
#include <map>
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
    std::string decoded; // libacars-decoded application text (CPDLC/ADS-C/...), empty if none
    bool   hasPos = false; // a position was extracted (ADS-C)
    double lat = 0.0;
    double lon = 0.0;
    int    alt = 0;        // altitude in feet (ADS-C)
    std::string icao;      // ICAO 24-bit hex (ADS-C airframe id), if present
    std::string flight;    // flight/callsign (ADS-C flight id), if present
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
        // Concatenate with an existing segment that shares the same message ID.
        for (auto it = msgs_.rbegin(); it != msgs_.rend(); ++it)
        {
            if (it->channelId == m.channelId && it->messageId == m.messageId)
            {
                if (!m.text.empty())
                {
                    if (!it->text.empty())
                        it->text += "\n";
                    it->text += m.text;
                }
                it->frameNumber = m.frameNumber;
                it->timeUtc = m.timeUtc;
                ++count_;
                return;
            }
        }
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

// Active Inmarsat-C terminal (MES) tracked from terminal activity messages.
struct MesEntry
{
    uint32_t mesId = 0;
    std::string action;
    std::string sat;
    int les = -1;
    int channel = -1;
    double lastSeen = 0.0;
    double freqMHz = 0.0;
    uint64_t msgs = 0;
};

class MesLog
{
public:
    void add(uint32_t mesId, const char* action, const char* sat, int les,
             int channel, double freqMHz, double nowSec)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto& e = entries_[mesId];
        e.mesId = mesId;
        e.action = action ? action : "";
        e.sat = sat ? sat : "";
        e.les = les;
        e.channel = channel;
        e.lastSeen = nowSec;
        e.freqMHz = freqMHz;
        ++e.msgs;
    }
    std::vector<MesEntry> snapshot()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        std::vector<MesEntry> out;
        for (auto& kv : entries_) out.push_back(kv.second);
        return out;
    }
    void clear() { std::lock_guard<std::mutex> lk(mtx_); entries_.clear(); }
    size_t size() const { return entries_.size(); }

private:
    mutable std::mutex mtx_;
    std::map<uint32_t, MesEntry> entries_;
};

// A decoded LES (Land Earth Station) private message — non-EGC Inmarsat-C
// ship-to-shore / shore-to-ship text extracted from 0xAA packets.
struct LesMessage
{
    int channelId = 0;
    double freqMHz = 0.0;
    int frameNumber = 0;
    std::string timeUtc;
    std::string satName;   // "AOR-W", "AOR-E", etc.
    int lesId = -1;
    std::string lesLabel;  // e.g. "AOR-E LES 02 (Stratos Global...)"
    int channel = -1;
    int pktNo = 0;
    std::string text;      // decoded IA5 message text
};

class LesLog
{
public:
    void add(const LesMessage& m)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        msgs_.push_back(m);
        if (msgs_.size() > kMax)
            msgs_.erase(msgs_.begin(), msgs_.begin() + (msgs_.size() - kMax));
        ++count_;
    }
    std::vector<LesMessage> snapshot()
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
    std::vector<LesMessage> msgs_;
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

// One tracked aircraft, keyed by Inmarsat AES ID, aggregated from ACARS/ADS-C.
struct AircraftEntry
{
    uint32_t aesId = 0;
    std::string icao;    // ICAO 24-bit hex (from ADS-C airframe id)
    std::string reg;     // registration (ACARS)
    std::string flight;  // flight / callsign
    bool   hasPos = false;
    double lat = 0.0;
    double lon = 0.0;
    int    alt = 0;
    double posTime = 0.0;  // epoch sec of last position
    double lastSeen = 0.0; // epoch sec of last message
    uint64_t msgs = 0;
    double freqMHz = 0.0;
};

// Thread-safe per-aircraft tracking table. Updated from decode worker threads,
// snapshotted by the UI thread.
class AircraftTable
{
public:
    void update(const DecodedMessage& m, double nowSec)
    {
        if (m.aesId == 0)
            return;
        std::lock_guard<std::mutex> lk(mtx_);
        AircraftEntry& e = byId_[m.aesId];
        e.aesId = m.aesId;
        if (!m.icao.empty())   e.icao = m.icao;
        if (!m.reg.empty())    e.reg = m.reg;
        if (!m.flight.empty()) e.flight = m.flight;
        if (m.hasPos)
        {
            e.hasPos = true;
            e.lat = m.lat;
            e.lon = m.lon;
            e.alt = m.alt;
            e.posTime = nowSec;
        }
        e.lastSeen = nowSec;
        e.freqMHz = m.freqMHz;
        ++e.msgs;
    }

    std::vector<AircraftEntry> snapshot()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        std::vector<AircraftEntry> out;
        out.reserve(byId_.size());
        for (auto& kv : byId_)
            out.push_back(kv.second);
        return out;
    }

    size_t count()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return byId_.size();
    }

    void clear()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        byId_.clear();
    }

    // Quick ICAO-only update from a C-channel data SU (no position/flight).
    void setIcao(uint32_t aesId, const std::string& icao, double nowSec)
    {
        if (aesId == 0 || icao.empty())
            return;
        std::lock_guard<std::mutex> lk(mtx_);
        AircraftEntry& e = byId_[aesId];
        e.aesId = aesId;
        e.icao = icao;
        if (e.lastSeen < nowSec)
            e.lastSeen = nowSec;
    }

    std::string icao(uint32_t aesId) const
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = byId_.find(aesId);
        if (it == byId_.end()) return {};
        return it->second.icao;
    }

private:
    mutable std::mutex mtx_;
    std::map<uint32_t, AircraftEntry> byId_;
};
