#include "sdr/rtl_tcp_source.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
#define CLOSESOCK closesocket
inline void shutSock(socket_t s) { ::shutdown(s, SD_BOTH); }
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>
using socket_t = int;
#define INVALID_SOCKET (-1)
#define CLOSESOCK ::close
inline void shutSock(socket_t s) { ::shutdown(s, SHUT_RDWR); }
#endif

#include <cstdio>
#include <cstring>

#if defined(_WIN32)
namespace {
struct WsaInit
{
    WsaInit()
    {
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
    }
};

static WsaInit g_wsa;
}
#endif

#define INVALID_SOCK (~(uintptr_t)0)

RtlTcpSource::~RtlTcpSource()
{
    stop();
}


bool RtlTcpSource::recvAll(void* buf, int len)
{
    char* p = static_cast<char*>(buf);

    while (len > 0)
    {
        int n = ::recv((socket_t)sock_, p, len, 0);
        if (n <= 0)
            return false;

        p += n;
        len -= n;
    }

    return true;
}


bool RtlTcpSource::sendCmd(uint8_t opcode, const void* data, int dataLen)
{
    std::lock_guard<std::mutex> lk(sockMtx_);

    if (sock_ == INVALID_SOCK)
        return false;

    uint8_t buf[5]{};
    buf[0] = opcode;

    if (data && dataLen > 0 && dataLen <= 4)
        std::memcpy(buf + 1, data, dataLen);

    int total = 1 + dataLen;
    int sent = 0;

    while (sent < total)
    {
        int n = ::send(
            (socket_t)sock_,
            (const char*)(buf + sent),
            total - sent,
            0);

        if (n <= 0)
            return false;

        sent += n;
    }

    return true;
}


void RtlTcpSource::setCenterFreq(double hz)
{
    centerFreq_ = hz;

    uint32_t f = htonl((uint32_t)hz);

    sendCmd(0x01, &f, 4);
}


void RtlTcpSource::setSampleRate(double hz)
{
    sampleRate_ = hz;

    uint32_t r = htonl((uint32_t)hz);

    sendCmd(0x02, &r, 4);
}


void RtlTcpSource::setGain(double db)
{
    gainDb_ = db;

    if (db < 0.0)
    {
        uint32_t mode = htonl(0u);

        sendCmd(0x03, &mode, 4);
    }
    else
    {
        uint32_t mode = htonl(1u);

        sendCmd(0x03, &mode, 4);

        uint32_t gain = htonl((uint32_t)(db * 10.0));

        sendCmd(0x04, &gain, 4);
    }
}


void RtlTcpSource::setBiasTee(bool on)
{
    biasTee_ = on;

    uint32_t v = htonl(on ? 1u : 0u);

    // RTL-SDR Blog rtl_tcp uses 0x0e for bias tee
    sendCmd(0x0E, &v, 4);
}


void RtlTcpSource::setPpm(double ppm)
{
    ppm_ = ppm;

    int32_t v = htonl((int32_t)ppm);

    sendCmd(0x05, &v, 4);
}


bool RtlTcpSource::start(int, SdrSampleCb cb, std::string& err)
{
    if (running_.load())
        stop();

    struct addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    struct addrinfo* ai = nullptr;

    char portStr[16];

    std::snprintf(
        portStr,
        sizeof(portStr),
        "%u",
        (unsigned)port_);

    int rc = getaddrinfo(
        host_.c_str(),
        portStr,
        &hints,
        &ai);

    if (rc != 0 || !ai)
    {
        err = "RTL-TCP: getaddrinfo failed";
        if (ai)
            freeaddrinfo(ai);

        return false;
    }


    socket_t s = ::socket(
        ai->ai_family,
        ai->ai_socktype,
        ai->ai_protocol);

    if (s == INVALID_SOCKET)
    {
        err = "RTL-TCP: socket failed";
        freeaddrinfo(ai);
        return false;
    }


    if (::connect(
            s,
            ai->ai_addr,
            (int)ai->ai_addrlen) != 0)
    {
        err = "RTL-TCP: connect failed";
        CLOSESOCK(s);
        freeaddrinfo(ai);
        return false;
    }

    freeaddrinfo(ai);


    int one = 1;

    setsockopt(
        s,
        IPPROTO_TCP,
        TCP_NODELAY,
        (const char*)&one,
        sizeof(one));


    {
        std::lock_guard<std::mutex> lk(sockMtx_);
        sock_ = (uintptr_t)s;
    }


    struct DongleInfo
    {
        char magic[4];
        uint32_t tunerType;
        uint32_t gainCount;
    };

    DongleInfo info{};

    if (!recvAll(&info, sizeof(info)))
    {
        err = "RTL-TCP: failed reading dongle info";
        CLOSESOCK(s);
        return false;
    }


    if (std::memcmp(info.magic, "RTL0", 4) != 0)
    {
        err = "RTL-TCP: bad dongle header";
        CLOSESOCK(s);
        return false;
    }


    info.tunerType = ntohl(info.tunerType);
    info.gainCount = ntohl(info.gainCount);


    cb_ = cb;

    running_.store(true);


    setCenterFreq(centerFreq_);
    setSampleRate(sampleRate_);
    setGain(gainDb_);
    setPpm(ppm_);
    setBiasTee(biasTee_);


    rbuf_.resize(65536);
    fbuf_.resize(65536);


    thread_ = std::thread(
        &RtlTcpSource::recvLoop,
        this);


    return true;
}

void RtlTcpSource::stop()
{
    running_.store(false);

    socket_t localSock = INVALID_SOCKET;

    {
        std::lock_guard<std::mutex> lk(sockMtx_);

        if (sock_ != INVALID_SOCK)
        {
            localSock = (socket_t)sock_;
            sock_ = INVALID_SOCK;
        }
    }


    if (localSock != INVALID_SOCKET)
    {
        shutSock(localSock);
        CLOSESOCK(localSock);
    }


    if (thread_.joinable())
        thread_.join();


    cb_ = nullptr;
}



void RtlTcpSource::recvLoop()
{
    socket_t s;

    {
        std::lock_guard<std::mutex> lk(sockMtx_);

        if (sock_ == INVALID_SOCK)
            return;

        s = (socket_t)sock_;
    }


    dcOffRe_ = 0.0f;
    dcOffIm_ = 0.0f;


    while (running_.load())
    {
        int n = ::recv(
            s,
            (char*)rbuf_.data(),
            (int)rbuf_.size(),
            0);


        if (n <= 0)
        {
            if (running_.load())
            {
#if defined(_WIN32)

                int e = WSAGetLastError();

                if (e == WSAEINTR ||
                    e == WSAEWOULDBLOCK)
                {
                    continue;
                }

#else

                if (errno == EINTR ||
                    errno == EAGAIN ||
                    errno == EWOULDBLOCK)
                {
                    continue;
                }

#endif
            }

            break;
        }


        /*
         * rtl_tcp sends raw RTL2832U format:
         *
         * byte 0 = I unsigned 8-bit
         * byte 1 = Q unsigned 8-bit
         *
         * Convert to float complex:
         *
         * I/Q range:
         * -1.0 ... +1.0
         */

        int nSamp = n / 2;


        if ((nSamp * 2) > (int)fbuf_.size())
            fbuf_.resize(nSamp * 2);



        for (int i = 0; i < nSamp; i++)
        {
            float re =
                ((int)rbuf_[i * 2] - 128) / 128.0f;

            float im =
                ((int)rbuf_[i * 2 + 1] - 128) / 128.0f;


            // DC removal
            dcOffRe_ =
                dcOffRe_ * (1.0f - dcRate_) +
                re * dcRate_;

            dcOffIm_ =
                dcOffIm_ * (1.0f - dcRate_) +
                im * dcRate_;


            fbuf_[i * 2] =
                re - dcOffRe_;

            fbuf_[i * 2 + 1] =
                im - dcOffIm_;
        }



        if (cb_)
            cb_(fbuf_.data(), nSamp);
    }


    running_.store(false);
}


