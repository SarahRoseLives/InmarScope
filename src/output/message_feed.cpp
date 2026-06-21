#include "output/message_feed.h"

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
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
#define INVALID_SOCKET (-1)
#define CLOSESOCK ::close
#endif

#include <chrono>
#include <cstring>
#include <ctime>

namespace {
#if defined(_WIN32)
struct WsaInit { WsaInit() { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); } };
static WsaInit g_wsa;
#endif

std::string jsonEscape(const std::string& in)
{
    std::string out;
    out.reserve(in.size() + 8);
    for (char ch : in)
    {
        unsigned char c = (unsigned char)ch;
        switch (c)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c >= 0x20) out += (char)c;
            break;
        }
    }
    return out;
}

void nowUnix(long& sec, long& usec)
{
    auto now = std::chrono::system_clock::now().time_since_epoch();
    sec = (long)std::chrono::duration_cast<std::chrono::seconds>(now).count();
    usec = (long)(std::chrono::duration_cast<std::chrono::microseconds>(now).count() % 1000000);
}
} // namespace

MessageFeed::~MessageFeed()
{
    closeFile();
    closeUdp();
    closeSbs();
}

void MessageFeed::setFileEnabled(bool on, const std::string& path)
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (on && (path != filePath_ || !file_))
    {
        closeFile();
        filePath_ = path;
        file_ = std::fopen(filePath_.c_str(), "ab");
    }
    else if (!on)
    {
        closeFile();
    }
    fileEnabled_ = on && file_;
}

void MessageFeed::setUdpEnabled(bool on, const std::string& host, int port)
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (on && (host != udpHost_ || port != udpPort_ || sock_ == ~(uintptr_t)0))
    {
        closeUdp();
        udpHost_ = host;
        udpPort_ = port;
        ensureUdp();
    }
    else if (!on)
    {
        closeUdp();
    }
    udpEnabled_ = on && sock_ != ~(uintptr_t)0;
}

void MessageFeed::closeFile()
{
    if (file_) { std::fclose(file_); file_ = nullptr; }
}

void MessageFeed::ensureUdp()
{
    socket_t s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) return;
    auto* a = new sockaddr_in{};
    a->sin_family = AF_INET;
    a->sin_port = htons((uint16_t)udpPort_);
    if (::inet_pton(AF_INET, udpHost_.c_str(), &a->sin_addr) != 1)
    {
        // Allow hostname resolution.
        addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        if (getaddrinfo(udpHost_.c_str(), nullptr, &hints, &res) == 0 && res)
        {
            a->sin_addr = ((sockaddr_in*)res->ai_addr)->sin_addr;
            freeaddrinfo(res);
        }
        else
        {
            CLOSESOCK(s);
            delete a;
            return;
        }
    }
    sock_ = (uintptr_t)s;
    addr_ = a;
}

void MessageFeed::closeUdp()
{
    if (sock_ != ~(uintptr_t)0) { CLOSESOCK((socket_t)sock_); sock_ = ~(uintptr_t)0; }
    delete (sockaddr_in*)addr_;
    addr_ = nullptr;
}

static void setNonBlocking(socket_t s)
{
#if defined(_WIN32)
    u_long mode = 1;
    ioctlsocket(s, FIONBIO, &mode);
#else
    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, fl | O_NONBLOCK);
#endif
}

static bool sendWouldBlock()
{
#if defined(_WIN32)
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

void MessageFeed::setSbsEnabled(bool on, int port)
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (on && (port != sbsPort_ || sbsListen_ == ~(uintptr_t)0))
    {
        closeSbs();
        sbsPort_ = port;
        ensureSbsListen();
    }
    else if (!on)
    {
        closeSbs();
    }
    sbsEnabled_ = on && sbsListen_ != ~(uintptr_t)0;
}

void MessageFeed::ensureSbsListen()
{
    // Prefer a dual-stack IPv6 socket so clients reaching us as either ::1 or
    // 127.0.0.1 (Windows resolves "localhost" to IPv6 first) both connect.
    socket_t s = ::socket(AF_INET6, SOCK_STREAM, 0);
    if (s != INVALID_SOCKET)
    {
        int off = 0; // turn OFF v6-only -> accept IPv4-mapped connections too
        ::setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&off, sizeof(off));
        int yes = 1;
        ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
        sockaddr_in6 a6{};
        a6.sin6_family = AF_INET6;
        a6.sin6_addr = in6addr_any;
        a6.sin6_port = htons((uint16_t)sbsPort_);
        if (::bind(s, (sockaddr*)&a6, sizeof(a6)) == 0 && ::listen(s, 8) == 0)
        {
            setNonBlocking(s);
            sbsListen_ = (uintptr_t)s;
            return;
        }
        CLOSESOCK(s);
    }

    // Fallback: IPv4-only listener.
    s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return;
    int yes = 1;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((uint16_t)sbsPort_);
    if (::bind(s, (sockaddr*)&a, sizeof(a)) != 0 || ::listen(s, 8) != 0)
    {
        CLOSESOCK(s);
        return;
    }
    setNonBlocking(s);
    sbsListen_ = (uintptr_t)s;
}

void MessageFeed::acceptSbsClients()
{
    if (sbsListen_ == ~(uintptr_t)0) return;
    for (;;)
    {
        socket_t c = ::accept((socket_t)sbsListen_, nullptr, nullptr);
        if (c == INVALID_SOCKET)
            break; // no more pending connections
        setNonBlocking(c);
        sbsClients_.push_back((uintptr_t)c);
    }
}

void MessageFeed::closeSbs()
{
    for (uintptr_t c : sbsClients_)
        CLOSESOCK((socket_t)c);
    sbsClients_.clear();
    if (sbsListen_ != ~(uintptr_t)0) { CLOSESOCK((socket_t)sbsListen_); sbsListen_ = ~(uintptr_t)0; }
}

void MessageFeed::pollSbs()
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (sbsListen_ == ~(uintptr_t)0)
        return;
    acceptSbsClients();
    // Prune clients that have disconnected (non-blocking peek: 0 == closed).
    char tmp[8];
    for (size_t i = 0; i < sbsClients_.size();)
    {
        int r = ::recv((socket_t)sbsClients_[i], tmp, sizeof(tmp), MSG_PEEK);
        if (r == 0 || (r < 0 && !sendWouldBlock()))
        {
            CLOSESOCK((socket_t)sbsClients_[i]);
            sbsClients_.erase(sbsClients_.begin() + i);
        }
        else
        {
            ++i;
        }
    }
}

void MessageFeed::emitSbs(const DecodedMessage& m)
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (!sbsEnabled_ || sbsListen_ == ~(uintptr_t)0)
        return;

    acceptSbsClients(); // register any newly-connected VRS/tar1090 clients
    if (sbsClients_.empty())
        return;

    std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char dbuf[16], tbuf[16];
    std::snprintf(dbuf, sizeof(dbuf), "%04d/%02d/%02d", tm.tm_year + 1900, tm.tm_mon + 1,
                  tm.tm_mday);
    std::snprintf(tbuf, sizeof(tbuf), "%02d:%02d:%02d.000", tm.tm_hour, tm.tm_min, tm.tm_sec);

    char hex[8];
    if (!m.icao.empty())
        std::snprintf(hex, sizeof(hex), "%s", m.icao.c_str());
    else
        std::snprintf(hex, sizeof(hex), "%06X", m.aesId & 0xFFFFFF);

    std::string callsign = !m.flight.empty() ? m.flight : m.reg;

    // BaseStation MSG,3 (airborne position): HexIdent, Callsign, Altitude, Lat, Lon.
    char line[256];
    int n = std::snprintf(line, sizeof(line),
        "MSG,3,1,1,%s,1,%s,%s,%s,%s,%s,%d,,,%.5f,%.5f,,,,,,0\r\n",
        hex, dbuf, tbuf, dbuf, tbuf, callsign.c_str(), m.alt, m.lat, m.lon);

    // Broadcast to every connected client; drop those that have disconnected.
    for (size_t i = 0; i < sbsClients_.size();)
    {
        int r = ::send((socket_t)sbsClients_[i], line, n, 0);
        if (r < 0 && !sendWouldBlock())
        {
            CLOSESOCK((socket_t)sbsClients_[i]);
            sbsClients_.erase(sbsClients_.begin() + i);
        }
        else
        {
            ++i;
        }
    }
    ++sbsSent_;
}

void MessageFeed::emit(const std::string& line)
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (fileEnabled_ && file_)
    {
        std::fwrite(line.data(), 1, line.size(), file_);
        std::fputc('\n', file_);
        std::fflush(file_);
    }
    if (udpEnabled_ && sock_ != ~(uintptr_t)0 && addr_)
    {
        ::sendto((socket_t)sock_, line.data(), (int)line.size(), 0,
                 (sockaddr*)addr_, sizeof(sockaddr_in));
    }
    ++sent_;
}

void MessageFeed::feedAcars(const DecodedMessage& m)
{
    if (sbsEnabled_ && m.hasPos)
        emitSbs(m);

    if (!fileEnabled_ && !udpEnabled_)
        return;

    if (format_ == JAERO_TEXT)
    {
        std::time_t t = std::time(nullptr);
        std::tm tm{};
#if defined(_WIN32)
        gmtime_s(&tm, &t);
#else
        gmtime_r(&t, &tm);
#endif
        char regp[8];
        int rlen = (int)m.reg.size();
        int pad = 7 - rlen; if (pad < 0) pad = 0;
        std::memset(regp, '.', pad);
        for (int i = 0; i + pad < 7 && i < rlen; ++i) regp[pad + i] = m.reg[i];
        regp[7] = 0;
        char l0 = m.label.size() > 0 ? m.label[0] : '_';
        char l1 = m.label.size() > 1 ? m.label[1] : '_';
        char buf[512];
        int n = std::snprintf(buf, sizeof(buf),
            "%02d:%02d:%02d %02d-%02d-%02d UTC AES:%06X GES:%02X %c %s %c %c%c %c",
            tm.tm_hour, tm.tm_min, tm.tm_sec, tm.tm_mday, tm.tm_mon + 1,
            tm.tm_year % 100, m.aesId & 0xFFFFFF, m.gesId & 0xFF,
            m.mode ? m.mode : '2', regp, ' ', l0, l1, m.blockId ? m.blockId : ' ');
        std::string out(buf, n);
        if (!m.text.empty())
        {
            out += "\n\t";
            for (char c : m.text) { if (c == '\r') continue; out += (c == '\n') ? "\n\t" : std::string(1, c); }
        }
        emit(out);
        return;
    }

    // JAERO JSONdump-compatible (Acarshub keys on app.name == "JAERO").
    char aes[8], ges[4];
    std::snprintf(aes, sizeof(aes), "%06X", m.aesId & 0xFFFFFF);
    std::snprintf(ges, sizeof(ges), "%02X", m.gesId & 0xFF);
    const char* srcAddr = m.downlink ? aes : ges;
    const char* srcType = m.downlink ? "Aircraft Earth Station" : "Ground Earth Station";
    const char* dstAddr = m.downlink ? ges : aes;
    const char* dstType = m.downlink ? "Ground Earth Station" : "Aircraft Earth Station";
    long sec, usec; nowUnix(sec, usec);

    char modeStr[2] = {m.mode ? m.mode : ' ', 0};
    char blkStr[2] = {m.blockId ? m.blockId : ' ', 0};

    std::string s = "{\"app\":{\"name\":\"JAERO\",\"ver\":\"InmarScope\"},\"isu\":{\"acars\":{";
    s += "\"mode\":\"" + jsonEscape(modeStr) + "\",";
    s += "\"ack\":\" \",";
    s += "\"blk_id\":\"" + jsonEscape(blkStr) + "\",";
    s += "\"label\":\"" + jsonEscape(m.label) + "\",";
    s += "\"reg\":\"" + jsonEscape(m.reg) + "\",";
    s += "\"msg_text\":\"" + jsonEscape(m.text) + "\"},";
    s += "\"refno\":\"00\",\"qno\":\"00\",";
    s += std::string("\"src\":{\"addr\":\"") + srcAddr + "\",\"type\":\"" + srcType + "\"},";
    s += std::string("\"dst\":{\"addr\":\"") + dstAddr + "\",\"type\":\"" + dstType + "\"}},";
    s += "\"t\":{\"sec\":" + std::to_string(sec) + ",\"usec\":" + std::to_string(usec) + "}";
    if (!station_.empty()) s += ",\"station\":\"" + jsonEscape(station_) + "\"";
    if (m.freqMHz > 0.0)
    {
        char f[16]; std::snprintf(f, sizeof(f), "%.4f", m.freqMHz);
        s += std::string(",\"freq\":") + f;
    }
    s += "}";
    emit(s);
}

void MessageFeed::feedEgc(const EgcMessage& m)
{
    if (!enabled()) return;
    long sec, usec; nowUnix(sec, usec);

    if (format_ == JAERO_TEXT)
    {
        std::string out = m.timeUtc + " UTC EGC " + m.service + " [" + m.priority +
                          "] msgId=" + std::to_string(m.messageId);
        if (!m.text.empty())
        {
            out += "\n\t";
            for (char c : m.text) { if (c == '\r') continue; out += (c == '\n') ? "\n\t" : std::string(1, c); }
        }
        emit(out);
        return;
    }

    // inmarsat-sniffer STD-C JSON schema.
    std::string s = "{\"source\":\"InmarScope\",\"type\":\"egc\"";
    s += ",\"service\":\"" + jsonEscape(m.service) + "\"";
    s += ",\"priority\":\"" + jsonEscape(m.priority) + "\"";
    s += ",\"msg_id\":" + std::to_string(m.messageId);
    s += ",\"presentation\":" + std::to_string(m.presentation);
    if (m.frameNumber > 0) s += ",\"frame\":" + std::to_string(m.frameNumber);
    if (!m.timeUtc.empty()) s += ",\"time_utc\":\"" + jsonEscape(m.timeUtc) + "\"";
    if (m.freqMHz > 0.0)
    {
        char f[16]; std::snprintf(f, sizeof(f), "%.4f", m.freqMHz);
        s += std::string(",\"downlink_mhz\":") + f;
    }
    s += ",\"text\":\"" + jsonEscape(m.text) + "\"";
    s += ",\"timestamp\":" + std::to_string(sec) + "}";
    emit(s);
}
