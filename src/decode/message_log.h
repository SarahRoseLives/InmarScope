// Thread-safe log of decoded messages for the Messages panel.
#pragma once

#include "store/message_store.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
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
    uint8_t suType = 0;  // SU type byte (0x30=Call progress, 0x21=Call announce, etc.)
    std::string decoded; // libacars-decoded application text (CPDLC/ADS-C/...), empty if none
    bool   hasPos = false; // a position was extracted (ADS-C)
    double lat = 0.0;
    double lon = 0.0;
    int    alt = 0;        // altitude in feet (ADS-C)
    std::string icao;      // ICAO 24-bit hex (ADS-C airframe id), if present
    std::string flight;    // flight/callsign (ADS-C flight id), if present
};

// Decoders leave timeSec at 0; the log stamps the receive time (epoch seconds)
// so the Messages table, archive, feeds and dashboard all agree.
inline void stampReceived(DecodedMessage& m)
{
    if (m.timeSec <= 0.0)
        m.timeSec = std::chrono::duration<double>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
}

class MessageLog
{
public:
    void setStore(MessageStore* s) { store_ = s; }

    void add(DecodedMessage m)
    {
        stampReceived(m);
        std::lock_guard<std::mutex> lk(mtx_);
        msgs_.push_back(m);
        if (msgs_.size() > kMax)
            msgs_.erase(msgs_.begin(), msgs_.begin() + (msgs_.size() - kMax));
        ++count_;
    }

    // Called from the decode worker — persists to store and then in-memory.
    void addAndStore(DecodedMessage m, int baud)
    {
        stampReceived(m);
        if (store_)
            store_->storeAcars(m.timeSec, m.channelId, m.freqMHz, m.text, m.hex,
                              m.aesId, m.icao, m.reg, m.flight, m.label,
                              m.hasPos, m.lat, m.lon, m.alt, m.decoded,
                              (m.downlink != 0), baud);
        std::lock_guard<std::mutex> lk(mtx_);
        msgs_.push_back(m);
        if (msgs_.size() > kMax)
            msgs_.erase(msgs_.begin(), msgs_.begin() + (msgs_.size() - kMax));
        ++count_;
    }

    void addSuAndStore(DecodedMessage m, int baud)
    {
        stampReceived(m);
        if (store_)
            store_->storeSu(m.timeSec, m.channelId, m.freqMHz, m.text, m.hex,
                           m.aesId, m.suType, baud);
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
        if (viewArchive_)
            return archive_;
        return msgs_;
    }

    uint64_t count()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (viewArchive_)
            return (uint64_t)archive_.size();
        return count_;
    }

    void clear()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        msgs_.clear();
        archive_.clear();
        viewArchive_ = false;
    }

    // Load a past session into the archive buffer (replacing any prior archive).
    void setArchive(std::vector<DecodedMessage> items)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        archive_ = std::move(items);
        viewArchive_ = true;
    }

    void clearArchive()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        archive_.clear();
        viewArchive_ = false;
    }

    bool hasArchive() const { return viewArchive_; }

private:
    static constexpr size_t kMax = 2000;
    std::mutex mtx_;
    std::vector<DecodedMessage> msgs_;
    std::vector<DecodedMessage> archive_;
    bool viewArchive_ = false;
    uint64_t count_ = 0;
    MessageStore* store_ = nullptr;
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
    void setStore(MessageStore* s) { store_ = s; }

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
        // New message — persist to store first.
        if (store_)
        {
            double now = (double)std::time(nullptr);
            store_->storeEgc(now, m.channelId, m.freqMHz, m.text,
                            m.timeUtc, m.priority, m.messageId,
                            m.service, m.presentation);
        }
        msgs_.push_back(m);
        if (msgs_.size() > kMax)
            msgs_.erase(msgs_.begin(), msgs_.begin() + (msgs_.size() - kMax));
        ++count_;
    }
    std::vector<EgcMessage> snapshot()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (viewArchive_)
            return archive_;
        return msgs_;
    }
    uint64_t count()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (viewArchive_)
            return (uint64_t)archive_.size();
        return count_;
    }
    void clear()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        msgs_.clear();
        archive_.clear();
        viewArchive_ = false;
    }

    void setArchive(std::vector<EgcMessage> items)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        archive_ = std::move(items);
        viewArchive_ = true;
    }
    void clearArchive()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        archive_.clear();
        viewArchive_ = false;
    }
    bool hasArchive() const { return viewArchive_; }

private:
    static constexpr size_t kMax = 2000;
    std::mutex mtx_;
    std::vector<EgcMessage> msgs_;
    std::vector<EgcMessage> archive_;
    bool viewArchive_ = false;
    uint64_t count_ = 0;
    MessageStore* store_ = nullptr;
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
    void setStore(MessageStore* s) { store_ = s; }

    void add(uint32_t mesId, const char* action, const char* sat, int les,
             int channel, double freqMHz, double nowSec)
    {
        if (store_)
            store_->storeMes(mesId, action ? action : "", sat ? sat : "",
                            les, channel, freqMHz, nowSec);
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
    MessageStore* store_ = nullptr;
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
    std::string text;
    bool isEncrypted = false;
};

class LesLog
{
public:
    void setStore(MessageStore* s) { store_ = s; }

    void add(const LesMessage& m)
    {
        if (store_)
        {
            double now = (double)std::time(nullptr);
            store_->storeLes(now, m.channelId, m.freqMHz, m.text, m.timeUtc,
                            m.satName, m.lesId, m.lesLabel, m.channel,
                            m.pktNo, m.isEncrypted);
        }
        std::lock_guard<std::mutex> lk(mtx_);
        msgs_.push_back(m);
        if (msgs_.size() > kMax)
            msgs_.erase(msgs_.begin(), msgs_.begin() + (msgs_.size() - kMax));
        ++count_;
    }
    std::vector<LesMessage> snapshot()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (viewArchive_)
            return archive_;
        return msgs_;
    }
    uint64_t count()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (viewArchive_)
            return (uint64_t)archive_.size();
        return count_;
    }
    void clear()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        msgs_.clear();
        archive_.clear();
        viewArchive_ = false;
    }

    void setArchive(std::vector<LesMessage> items)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        archive_ = std::move(items);
        viewArchive_ = true;
    }
    void clearArchive()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        archive_.clear();
        viewArchive_ = false;
    }
    bool hasArchive() const { return viewArchive_; }

private:
    static constexpr size_t kMax = 2000;
    std::mutex mtx_;
    std::vector<LesMessage> msgs_;
    std::vector<LesMessage> archive_;
    bool viewArchive_ = false;
    uint64_t count_ = 0;
    MessageStore* store_ = nullptr;
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

// A discovered LES TDM downlink frequency, extracted from 0xAB LES List
// or 0x81/0x83/0x92 channel assignment packets on the NCS common channel.
struct LesFreqEntry
{
    double freqMHz = 0.0;
    int satId = -1;
    int lesId = -1;
    std::string satName;
    std::string lesLabel;
    uint16_t services = 0;
    double lastSeen = 0.0;
    int hits = 0;
    bool hasDecoder = false;
};

class LesFreqTable
{
public:
    void add(double freqMHz, int satId, int lesId, const std::string& satName,
             const std::string& lesLabel, uint16_t services, double nowSec)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto& e : entries_)
        {
            if (std::fabs(e.freqMHz - freqMHz) < 0.001)
            {
                e.satId = satId;
                e.lesId = lesId;
                e.satName = satName;
                e.lesLabel = lesLabel;
                e.services = services;
                e.lastSeen = nowSec;
                ++e.hits;
                return;
            }
        }
        entries_.push_back({freqMHz, satId, lesId, satName, lesLabel, services, nowSec, 1, false});
    }

    void setHasDecoder(double freqMHz, bool on)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto& e : entries_)
            if (std::fabs(e.freqMHz - freqMHz) < 0.001)
                { e.hasDecoder = on; return; }
    }

    std::vector<LesFreqEntry> snapshot()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return entries_;
    }

    void clear()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        entries_.clear();
    }

private:
    std::mutex mtx_;
    std::vector<LesFreqEntry> entries_;
};
struct VoiceCallRecord
{
    double timeSec = 0.0;        // epoch of call start
    double durationSec = 0.0;    // 0 if still live
    double freqMHz = 0.0;
    int channelId = -1;
    uint32_t aesId = 0;
    std::string icao;
    std::string filename;        // just the filename, no directory
    bool recording = false;      // true = live call in progress
};

class VoiceCallLog
{
public:
    void setStore(MessageStore* s) { store_ = s; }

    void add(const VoiceCallRecord& r)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        items_.push_back(r);
        if (items_.size() > kMax)
            items_.erase(items_.begin(), items_.begin() + (items_.size() - kMax));
        ++count_;
    }

    // Called when a live recording ends: finds the entry by channelId and fills
    // in the duration + filename. If not found, adds a new completed record.
    void updateEnd(int channelId, double durationSec, const std::string& filename)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto it = items_.rbegin(); it != items_.rend(); ++it)
        {
            if (it->channelId == channelId && it->recording)
            {
                it->recording = false;
                it->durationSec = durationSec;
                if (!filename.empty())
                {
                    // Extract just the filename from the full path.
                    const char* s = filename.c_str();
                    const char* slash = std::strrchr(s, '/');
#ifdef _WIN32
                    const char* bslash = std::strrchr(s, '\\');
                    if (bslash && bslash > slash) slash = bslash;
#endif
                    it->filename = slash ? (slash + 1) : s;
                }
                // Persist to store with full info now available.
                if (store_ && it->durationSec > 0)
                    store_->storeVoice(it->timeSec, it->freqMHz, it->aesId,
                                      it->icao, it->durationSec, it->filename);
                return;
            }
        }
    }

    // Scan a directory for existing WAV/OGG files and populate.
    void scanDir(const std::string& dir);

    std::vector<VoiceCallRecord> snapshot()
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
    static constexpr size_t kMax = 500;
    std::mutex mtx_;
    std::vector<VoiceCallRecord> items_;
    uint64_t count_ = 0;
    MessageStore* store_ = nullptr;
};
