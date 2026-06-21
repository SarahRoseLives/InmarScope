#include "sdr/sdrpp_server_source.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
#define CLOSESOCK closesocket
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
#define INVALID_SOCKET (-1)
#define CLOSESOCK ::close
#endif

#include <zstd.h>

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

// SDR++ server protocol constants.
namespace {
enum { PKT_COMMAND = 0, PKT_COMMAND_ACK = 1, PKT_BASEBAND = 2, PKT_BASEBAND_COMPRESSED = 3,
       PKT_VFO = 4, PKT_FFT = 5, PKT_ERROR = 6 };
enum { CMD_GET_UI = 0, CMD_UI_ACTION = 1, CMD_START = 2, CMD_STOP = 3, CMD_SET_FREQUENCY = 4,
       CMD_GET_SAMPLERATE = 5, CMD_SET_SAMPLE_TYPE = 6, CMD_SET_COMPRESSION = 7,
       CMD_SET_SAMPLERATE = 0x80, CMD_DISCONNECT = 0x81 };
enum { PCM_I8 = 0, PCM_I16 = 1, PCM_F32 = 2 };

#pragma pack(push, 1)
struct PacketHeader { uint32_t type; uint32_t size; };
#pragma pack(pop)

constexpr int kMaxPacket = 16 * 1024 * 1024;

#if defined(_WIN32)
struct WsaInit { WsaInit() { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); } };
static WsaInit g_wsa;
#endif
} // namespace

SdrppServerSource::~SdrppServerSource()
{
    stop();
    if (dctx_)
        ZSTD_freeDCtx((ZSTD_DCtx*)dctx_);
}

bool SdrppServerSource::recvAll(void* buf, int len)
{
    uint8_t* p = (uint8_t*)buf;
    int got = 0;
    while (got < len)
    {
        int n = ::recv((socket_t)sock_, (char*)p + got, len - got, 0);
        if (n <= 0)
            return false;
        got += n;
    }
    return true;
}

bool SdrppServerSource::sendCommand(uint32_t cmd, const void* args, uint32_t argLen)
{
    uint8_t buf[1024];
    if (argLen > sizeof(buf) - sizeof(PacketHeader) - 4)
        return false;
    PacketHeader* h = (PacketHeader*)buf;
    h->type = PKT_COMMAND;
    h->size = sizeof(PacketHeader) + 4 + argLen; // + CommandHeader(cmd) + args
    *(uint32_t*)(buf + sizeof(PacketHeader)) = cmd;
    if (argLen)
        std::memcpy(buf + sizeof(PacketHeader) + 4, args, argLen);
    int total = (int)h->size;
    int sent = 0;
    while (sent < total)
    {
        int n = ::send((socket_t)sock_, (const char*)buf + sent, total - sent, 0);
        if (n <= 0)
            return false;
        sent += n;
    }
    return true;
}

bool SdrppServerSource::start(int, SdrSampleCb cb, std::string& err)
{
    if (running_.load())
        return true;

    // Resolve + connect.
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    std::string portStr = std::to_string(port_);
    if (getaddrinfo(host_.c_str(), portStr.c_str(), &hints, &res) != 0 || !res)
    {
        err = "Cannot resolve host " + host_;
        return false;
    }
    socket_t s = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == INVALID_SOCKET)
    {
        freeaddrinfo(res);
        err = "socket() failed";
        return false;
    }
    if (::connect(s, res->ai_addr, (int)res->ai_addrlen) != 0)
    {
        freeaddrinfo(res);
        CLOSESOCK(s);
        err = "Cannot connect to " + host_ + ":" + portStr;
        return false;
    }
    freeaddrinfo(res);
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
    sock_ = (uintptr_t)s;

    if (!dctx_)
        dctx_ = ZSTD_createDCtx();
    rbuf_.resize(kMaxPacket);
    dbuf_.resize(kMaxPacket);
    cb_ = std::move(cb);

    // Handshake + config (fire-and-forget; the worker parses any ACKs,
    // including the GET_UI DrawList used to discover sample rates).
    sendCommand(CMD_GET_UI, nullptr, 0);
    uint8_t st = (uint8_t)sampleType_;
    sendCommand(CMD_SET_SAMPLE_TYPE, &st, 1);
    uint8_t comp = compression_ ? 1 : 0;
    sendCommand(CMD_SET_COMPRESSION, &comp, 1);
    double f = centerFreq_;
    sendCommand(CMD_SET_FREQUENCY, &f, sizeof(double));
    sendCommand(CMD_START, nullptr, 0);
    // Re-request the UI after START (the running menu) and the actual rate.
    sendCommand(CMD_GET_UI, nullptr, 0);
    sendCommand(CMD_GET_SAMPLERATE, nullptr, 0);

    running_.store(true);
    thread_ = std::thread([this]() { worker(); });
    return true;
}

void SdrppServerSource::stop()
{
    if (!running_.load() && sock_ == ~(uintptr_t)0)
        return;
    if (sock_ != ~(uintptr_t)0)
        sendCommand(CMD_STOP, nullptr, 0);
    running_.store(false);
    if (sock_ != ~(uintptr_t)0)
    {
        CLOSESOCK((socket_t)sock_); // unblocks recv
        sock_ = ~(uintptr_t)0;
    }
    if (thread_.joinable())
        thread_.join();
    cb_ = nullptr;
}

void SdrppServerSource::setCenterFreq(double hz)
{
    centerFreq_ = hz;
    if (running_.load())
        sendCommand(CMD_SET_FREQUENCY, &hz, sizeof(double));
}

void SdrppServerSource::setSampleRate(double hz)
{
    reqSampleRate_ = hz;
    if (hz <= 0.0)
        return;

    // If connected and we've parsed the server's rate combo, drive it via a
    // UI action (the only way to change a server-side source's rate). The
    // server then confirms the actual rate via COMMAND_SET_SAMPLERATE.
    std::string id;
    int idx = -1;
    bool sync = false;
    {
        std::lock_guard<std::mutex> lk(uiMtx_);
        if (!srComboId_.empty() && !srValues_.empty())
        {
            double best = 1e30;
            for (size_t k = 0; k < srValues_.size(); ++k)
            {
                double d = std::fabs(srValues_[k] - hz);
                if (d < best) { best = d; idx = (int)k; }
            }
            id = srComboId_;
            sync = srForceSync_;
        }
    }
    if (running_.load() && idx >= 0)
        sendComboAction(id, idx, sync);
}

std::vector<std::string> SdrppServerSource::sampleRateLabels()
{
    std::lock_guard<std::mutex> lk(uiMtx_);
    return srLabels_;
}

std::vector<double> SdrppServerSource::sampleRateValues()
{
    std::lock_guard<std::mutex> lk(uiMtx_);
    return srValues_;
}

int SdrppServerSource::currentSampleRateIndex()
{
    std::lock_guard<std::mutex> lk(uiMtx_);
    return srCurrentIdx_;
}

std::string SdrppServerSource::uiDebug()
{
    std::lock_guard<std::mutex> lk(uiMtx_);
    return uiDebug_;
}

void SdrppServerSource::sendComboAction(const std::string& id, int index, bool sync)
{
    // UI_ACTION payload: [syncReq:1] storeItem(STRING id) storeItem(INT index)
    // STRING elem: [type=4][u16 len][bytes];  INT elem: [type=2][i32]
    uint8_t a[512];
    int n = 0;
    a[n++] = sync ? 1 : 0;
    a[n++] = 4; // DRAW_LIST_ELEM_TYPE_STRING
    uint16_t slen = (uint16_t)id.size();
    std::memcpy(a + n, &slen, 2); n += 2;
    std::memcpy(a + n, id.data(), slen); n += slen;
    a[n++] = 2; // DRAW_LIST_ELEM_TYPE_INT
    int32_t iv = index;
    std::memcpy(a + n, &iv, 4); n += 4;
    sendCommand(CMD_UI_ACTION, a, (uint32_t)n);
}

// Parse a rate label like "20MHz", "2.4 MHz", "250 kHz" into Hz.
static double parseRateLabel(const std::string& s)
{
    const char* p = s.c_str();
    char* end = nullptr;
    double v = std::strtod(p, &end);
    if (end == p)
        return 0.0;
    while (*end == ' ') ++end;
    char u = *end;
    if (u == 'G' || u == 'g') return v * 1e9;
    if (u == 'M' || u == 'm') return v * 1e6;
    if (u == 'k' || u == 'K') return v * 1e3;
    if (v > 0.0 && v < 1000.0) return v * 1e6; // bare number: assume MHz
    return v;                                   // already in Hz
}

void SdrppServerSource::parseUi(const uint8_t* data, int len)
{
    // Flatten the SmGui DrawList into typed elements.
    struct Elem { uint8_t type; uint8_t step; bool fsync; int i; std::string str; };
    std::vector<Elem> els;
    int p = 0;
    while (p < len)
    {
        Elem e{};
        e.type = data[p++];
        if (e.type == 0) // DRAW_STEP
        {
            if (p + 2 > len) break;
            e.step = data[p++];
            e.fsync = data[p++] != 0;
        }
        else if (e.type == 1) // BOOL
        {
            if (p + 1 > len) break;
            e.i = data[p++];
        }
        else if (e.type == 2) // INT
        {
            if (p + 4 > len) break;
            std::memcpy(&e.i, data + p, 4); p += 4;
        }
        else if (e.type == 3) // FLOAT
        {
            if (p + 4 > len) break;
            p += 4;
        }
        else if (e.type == 4) // STRING
        {
            if (p + 2 > len) break;
            uint16_t sl; std::memcpy(&sl, data + p, 2); p += 2;
            if (p + (int)sl > len) break;
            e.str.assign((const char*)data + p, (const char*)data + p + sl);
            p += sl;
        }
        else break;
        els.push_back(std::move(e));
    }

    // Find the sample-rate combo: a COMBO (step 0x80) whose label looks like a
    // sample-rate selector and whose items parse as rates.
    auto looksLikeRateId = [](std::string l) {
        for (auto& c : l) c = (char)std::tolower((unsigned char)c);
        if (l.find("type") != std::string::npos) return false;
        return l.find("_sr") != std::string::npos ||
               l.find("sr_sel") != std::string::npos ||
               l.find("samp_rate") != std::string::npos ||
               l.find("source_sr") != std::string::npos;
    };

    std::string foundId;
    std::vector<double> vals;
    std::vector<std::string> labels;
    int cur = -1;
    bool fsync = false;

    std::string dbg = "elems=" + std::to_string(els.size()) + " combos:";
    int comboCount = 0;

    for (size_t j = 0; j + 4 < els.size(); ++j)
    {
        if (els[j].type != 0 || els[j].step != 0x80) continue;       // COMBO
        if (els[j + 1].type != 4 || els[j + 2].type != 2 ||
            els[j + 3].type != 4) continue;
        const std::string& label = els[j + 1].str;

        // Count items for the debug summary.
        int itemCount = 0;
        {
            const std::string& it = els[j + 3].str;
            size_t s0 = 0;
            while (s0 < it.size())
            {
                size_t e0 = it.find('\0', s0);
                if (e0 == std::string::npos) e0 = it.size();
                if (e0 > s0) ++itemCount;
                s0 = e0 + 1;
            }
        }
        ++comboCount;
        dbg += " [" + label + " x" + std::to_string(itemCount) + "]";

        if (!looksLikeRateId(label)) continue;

        // Split the zero-separated items string into rate options.
        std::vector<double> v;
        std::vector<std::string> lab;
        const std::string& items = els[j + 3].str;
        size_t s = 0;
        while (s < items.size())
        {
            size_t e = items.find('\0', s);
            if (e == std::string::npos) e = items.size();
            std::string item = items.substr(s, e - s);
            s = e + 1;
            if (item.empty()) continue;
            double hz = parseRateLabel(item);
            if (hz <= 0.0) { v.clear(); break; }
            v.push_back(hz);
            lab.push_back(item);
        }
        if (v.empty()) continue;
        foundId = label;
        vals = std::move(v);
        labels = std::move(lab);
        cur = els[j + 2].i;
        fsync = els[j].fsync;
        // keep scanning to finish the debug summary
    }
    if (comboCount == 0)
        dbg += " (none)";

    bool apply = false;
    int applyIdx = -1;
    std::string applyId;
    bool applySync = false;
    {
        std::lock_guard<std::mutex> lk(uiMtx_);
        uiDebug_ = dbg;
        if (!foundId.empty())
        {
            srComboId_ = foundId;
            srValues_ = vals;
            srLabels_ = labels;
            srCurrentIdx_ = cur;
            srForceSync_ = fsync;

            // Apply a rate requested before the UI was known.
            if (reqSampleRate_ > 0.0)
            {
                double best = 1e30; int idx = -1;
                for (size_t k = 0; k < srValues_.size(); ++k)
                {
                    double d = std::fabs(srValues_[k] - reqSampleRate_);
                    if (d < best) { best = d; idx = (int)k; }
                }
                if (idx >= 0 && idx != srCurrentIdx_)
                {
                    apply = true; applyIdx = idx; applyId = srComboId_; applySync = srForceSync_;
                }
                reqSampleRate_ = 0.0; // one-shot, avoid resync loops
            }
        }
    }
    if (apply)
        sendComboAction(applyId, applyIdx, applySync);
}

void SdrppServerSource::handleBaseband(const uint8_t* data, int len)
{
    if (len < 8)
        return;
    uint16_t type = *(const uint16_t*)(data + 2);
    float scaler = *(const float*)(data + 4);
    const uint8_t* pcm = data + 8;
    int pcmLen = len - 8;

    if (type == PCM_F32)
    {
        int n = pcmLen / (int)(2 * sizeof(float));
        if (n > 0 && cb_)
            cb_((const float*)pcm, n);
    }
    else if (type == PCM_I16)
    {
        int n = pcmLen / (int)(2 * sizeof(int16_t));
        if (n <= 0)
            return;
        if ((int)fbuf_.size() < n * 2)
            fbuf_.resize((size_t)n * 2);
        const int16_t* s = (const int16_t*)pcm;
        float g = scaler / 32768.0f;
        for (int i = 0; i < n * 2; ++i)
            fbuf_[i] = s[i] * g;
        if (cb_)
            cb_(fbuf_.data(), n);
    }
    else if (type == PCM_I8)
    {
        int n = pcmLen / 2;
        if (n <= 0)
            return;
        if ((int)fbuf_.size() < n * 2)
            fbuf_.resize((size_t)n * 2);
        const int8_t* s = (const int8_t*)pcm;
        float g = scaler / 128.0f;
        for (int i = 0; i < n * 2; ++i)
            fbuf_[i] = s[i] * g;
        if (cb_)
            cb_(fbuf_.data(), n);
    }
}

void SdrppServerSource::worker()
{
    while (running_.load())
    {
        PacketHeader hdr;
        if (!recvAll(&hdr, sizeof(hdr)))
            break;
        if (hdr.size < sizeof(PacketHeader) || hdr.size > (uint32_t)kMaxPacket)
            break;
        int payload = (int)hdr.size - (int)sizeof(PacketHeader);
        if (payload > 0 && !recvAll(rbuf_.data(), payload))
            break;

        if (hdr.type == PKT_COMMAND)
        {
            uint32_t cmd = (payload >= 4) ? *(uint32_t*)rbuf_.data() : 0;
            if (cmd == CMD_SET_SAMPLERATE && payload >= 12)
                sampleRate_.store(*(double*)(rbuf_.data() + 4));
            else if (cmd == CMD_DISCONNECT)
                break;
        }
        else if (hdr.type == PKT_COMMAND_ACK)
        {
            // The GET_UI / UI_ACTION ack carries the source-module DrawList,
            // from which we extract the sample-rate combo options.
            uint32_t cmd = (payload >= 4) ? *(uint32_t*)rbuf_.data() : 0;
            if ((cmd == CMD_GET_UI || cmd == CMD_UI_ACTION) && payload > 4)
                parseUi(rbuf_.data() + 4, payload - 4);
        }
        else if (hdr.type == PKT_BASEBAND)
        {
            handleBaseband(rbuf_.data(), payload);
        }
        else if (hdr.type == PKT_BASEBAND_COMPRESSED)
        {
            size_t out = ZSTD_decompressDCtx((ZSTD_DCtx*)dctx_, dbuf_.data(), dbuf_.size(),
                                             rbuf_.data(), payload);
            if (!ZSTD_isError(out) && out > 0)
                handleBaseband(dbuf_.data(), (int)out);
        }
        // COMMAND_ACK / VFO / FFT / ERROR: ignored.
    }
    running_.store(false);
}
