// Inmarsat-C / EGC decoder implementation. Ported from scytaleC (GPL-3.0).
#include "decode/egc/egc_decoder.h"
#include "decode/message_log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int    kSymbolRate = 1200;
constexpr double kCenterFreq = 2000.0;
constexpr int    kUWFrameLen = 10368;

// --- RDemodulator (BPSK carrier + clock recovery on a real ~2 kHz signal) ---
class RDemodulator
{
public:
    RDemodulator()
    {
        wcarrier = (2 * M_PI * kCenterFreq) / kSampleRate;
        wclock = (2 * M_PI * kSymbolRate) / kSampleRate;
        lowerafclimit = (1800 * M_PI) / kSampleRate;
        upperafclimit = (4600 * M_PI) / kSampleRate;
        a_.resize(order);
        isb_.assign(order, 0.0);
        qsb_.assign(order, 0.0);
        for (int t = 0; t < order; ++t)
        {
            double dest = (t - (order - 1) / 2.0) / order;
            a_[t] = ((0.5 + std::cos(dest * 2.0 * M_PI) / 2.0) * 2.0) / order;
        }
    }

    void process(const float* real, int length, std::vector<uint8_t>& outBits)
    {
        double magnitude = 0.0;
        for (int i = 0; i < length; ++i) magnitude += std::fabs((double)real[i]);
        gaincontrol = magnitude / (length ? length : 1);
        int gco = (int)gaincontrol;
        if (gaincontrol == 0.0) gaincontrol = 1.0;

        double locki = 0.0, lockq = 0.0;
        for (int i = 0; i < length; ++i)
        {
            thetacarrier = std::fmod(thetacarrier, 2 * M_PI);
            double ic = std::cos(thetacarrier), qc = std::sin(thetacarrier);
            double gcs = real[i] / gaincontrol;
            double isample = ic * gcs, qsample = qc * gcs;
            isb_[0] = isample; qsb_[0] = qsample;
            double cqf = 0.0;
            for (int j = 0; j < order; ++j) cqf += a_[j] * qsb_[j];
            std::memmove(&isb_[1], &isb_[0], (order - 1) * sizeof(double));
            std::memmove(&qsb_[1], &qsb_[0], (order - 1) * sizeof(double));
            isamplefilt = 0.9 * isamplefilt + 0.1 * isample;
            qsamplefilt = 0.9 * qsamplefilt + 0.1 * qsample;
            locki = isamplefilt * isamplefilt;
            lockq = qsamplefilt * qsamplefilt;
            double fp = qsamplefilt * isamplefilt;
            wcarrier += beta * ncocontrolcarrier;
            thetacarrier += wcarrier + 0.03 * ncocontrolcarrier;
            ncocontrolcarrier = fp;
            if (wcarrier < lowerafclimit) wcarrier = lowerafclimit;
            if (wcarrier > upperafclimit) wcarrier = upperafclimit;

            double thetaclock = thetaclockminusone + wclock + -0.16 * ncocontrolclock;
            thetaclock = std::fmod(thetaclock, 2 * M_PI);
            double sc = std::sin(thetaclock);
            wclock += betaclock * ncocontrolclock;
            int clock = (sc >= 0.0) ? 1 : -1;
            if (thetaclockminusone < M_PI && thetaclock >= M_PI)
            {
                int decision = (qsamplefiltminusone > 0.0) ? 1 : -1;
                ncocontrolclock = decision * (cqf - cqsamplefiltminusthree);
            }
            charge += qsample;
            if (clock == 1 && clockminusone == -1)
            {
                if (charge > 0.0) databit = true;
                if (charge < 0.0) databit = false;
                charge = 0.0;
                outBits.push_back(databit ? 1 : 0);
                // Capture a BPSK soft point for the constellation display.
                scI_[scPos_] = (float)isamplefilt;
                scQ_[scPos_] = (float)qsamplefilt;
                scPos_ = (scPos_ + 1) & (kScat - 1);
            }
            thetaclockminusone = thetaclock;
            cqsamplefiltminusthree = qsamplefiltminustwo;
            qsamplefiltminustwo = qsamplefiltminusone;
            qsamplefiltminusone = cqf;
            clockminusone = clock;
        }
        double lock = (lockq != 0.0) ? locki / lockq : 1.0;
        if (lock < lock_lp) lock_lp = lock_lp * 0.9 + lock * 0.1;
        else lock_lp = lock_lp * 0.99 + lock * 0.01;
        if (lock_lp < 0.4 && gco > 50) Locked = true;
        if (lock_lp >= 1.0) Locked = false;
    }

    bool Locked = false;

    // Copy the most recent BPSK soft points (newest last), normalized to ~[-1,1].
    int scatter(double* out, int maxPairs) const
    {
        float peak = 1e-6f;
        for (int i = 0; i < kScat; ++i)
        {
            peak = std::max(peak, std::fabs(scI_[i]));
            peak = std::max(peak, std::fabs(scQ_[i]));
        }
        int n = std::min(maxPairs, kScat);
        for (int k = 0; k < n; ++k)
        {
            int idx = (scPos_ - n + k + kScat) & (kScat - 1);
            out[k * 2] = scI_[idx] / peak;
            out[k * 2 + 1] = scQ_[idx] / peak;
        }
        return n;
    }

private:
    static constexpr int order = 64;
    static constexpr int kScat = 512;
    float scI_[kScat] = {0};
    float scQ_[kScat] = {0};
    int scPos_ = 0;
    std::vector<double> a_, isb_, qsb_;
    double wcarrier = 0, wclock = 0, charge = 0.0;
    bool databit = false;
    double beta = 0.00022499999999999999, betaclock = 0.0;
    double thetacarrier = 0, isamplefilt = 0, qsamplefilt = 0, ncocontrolcarrier = 0;
    double thetaclockminusone = 0;
    int clockminusone = 0;
    double ncocontrolclock = 0, qsamplefiltminustwo = 0, qsamplefiltminusone = 0;
    double gaincontrol = 1.0, cqsamplefiltminusthree = 0, lock_lp = 0.0;
    double lowerafclimit = 0, upperafclimit = 0;
};

struct UWFrame { std::vector<uint8_t> frame; bool reversedPolarity = false; int ber = 0; };

class UWFinder
{
public:
    explicit UWFinder(int tol = 25) : tolerance_(tol)
    {
        reg_.assign(kUWFrameLen * 2, 0);
        for (int i = 0; i < 64; ++i) revPol_[i] = nrmPol_[i] ? 0 : 1;
    }
    void push(uint8_t sym, std::vector<UWFrame>& out)
    {
        std::memmove(&reg_[1], &reg_[0], (reg_.size() - 1) * sizeof(uint8_t));
        reg_[0] = sym;
        ++symbolCount_;
        if (symbolCount_ < kUWFrameLen) return;
        int nUW, rUW; bool rev;
        if (detect(nUW, rUW, rev))
        {
            UWFrame f;
            f.frame.assign(reg_.begin(), reg_.begin() + kUWFrameLen);
            f.reversedPolarity = rev;
            f.ber = std::min(nUW, rUW);
            if (rev) for (auto& b : f.frame) b ^= 1;
            out.push_back(std::move(f));
            symbolCount_ = 0;
        }
    }
private:
    bool detect(int& nUW, int& rUW, bool& rev)
    {
        nUW = 0; rUW = 0; int pp = 0;
        for (int sp = kUWFrameLen - 1; sp >= 0; sp -= 162)
        {
            nUW += nrmPol_[pp] ^ reg_[sp];
            nUW += nrmPol_[pp] ^ reg_[sp - 1];
            rUW += revPol_[pp] ^ reg_[sp];
            rUW += revPol_[pp] ^ reg_[sp - 1];
            ++pp;
        }
        rev = rUW <= tolerance_;
        return nUW <= tolerance_ || rUW <= tolerance_;
    }
    const uint8_t nrmPol_[64] = {
        0,0,0,0, 0,1,1,1, 1,1,1,0, 1,0,1,0, 1,1,0,0, 1,1,0,1, 1,1,0,1, 1,0,1,0,
        0,1,0,0, 1,1,1,0, 0,0,1,0, 1,1,1,1, 0,0,1,0, 1,0,0,0, 1,1,0,0, 0,0,1,0 };
    uint8_t revPol_[64];
    std::vector<uint8_t> reg_;
    int symbolCount_ = kUWFrameLen - 1;
    int tolerance_;
};

std::vector<uint8_t> depermute(std::vector<uint8_t> frame)
{
    std::reverse(frame.begin(), frame.end());
    std::vector<uint8_t> dst(kUWFrameLen);
    int perm[64];
    for (int i = 0; i < 64; ++i) perm[i] = ((i * 23) % 64 & 0x3F) * 162;
    for (int i = 0; i < 64; ++i) std::memcpy(&dst[i * 162], &frame[perm[i]], 162);
    return dst;
}

std::vector<uint8_t> deinterleave(const std::vector<uint8_t>& perm)
{
    static uint8_t mat[64][160];
    int row = -1, col = 0;
    for (int i = 0; i < kUWFrameLen; ++i)
    {
        if (i % 162 == 0) { col = 0; ++row; i += 2; }
        if (row < 64 && col < 160) mat[row][col] = perm[i];
        ++col;
    }
    std::vector<uint8_t> dst(10240);
    int pos = 0; row = 0; col = 0;
    while (row < 64)
    {
        dst[pos] = mat[row][col];
        ++row;
        if (row % 64 == 0) { row = 0; ++col; if (col == 160) break; }
        ++pos;
    }
    return dst;
}

class Viterbi
{
public:
    Viterbi() { genMet(100, 5.0, 0.0, 4); }
    std::vector<uint8_t> decode(const std::vector<uint8_t>& deint)
    {
        const int length = (int)deint.size();
        const int nbits = length / 16;
        std::vector<uint8_t> input(length);
        for (int k = 0; k < length; ++k) input[k] = deint[k] == 0 ? 28 : 228;
        struct St { uint32_t path; int64_t metric; };
        std::vector<St> state(64), next(64);
        for (int i = 0; i < 64; ++i) { state[i].path = 0; state[i].metric = (i == 0) ? 0 : -999999; }
        std::vector<uint8_t> output(640, 0);
        int mets[4]; int inputCounter = 0, j = 0; uint32_t bitcnt = 0;
        for (bitcnt = 0; bitcnt < (uint32_t)nbits * 8; ++bitcnt)
        {
            int a = input[inputCounter], b = input[inputCounter + 1];
            mets[0] = mettab_[0][a] + mettab_[0][b];
            mets[1] = mettab_[0][a] + mettab_[1][b];
            mets[2] = mettab_[1][a] + mettab_[0][b];
            mets[3] = mettab_[1][a] + mettab_[1][b];
            inputCounter += 2;
            for (int i = 0; i < 32; ++i)
            {
                int sym = partabIdx_[i];
                int64_t m0 = state[i].metric + mets[sym];
                int64_t m1 = state[i + 32].metric + mets[3 ^ sym];
                if (m0 > m1) { next[2 * i].metric = m0; next[2 * i].path = state[i].path << 1; }
                else { next[2 * i].metric = m1; next[2 * i].path = (state[i + 32].path << 1) | 1; }
                m0 = state[i].metric + mets[3 ^ sym];
                m1 = state[i + 32].metric + mets[sym];
                if (m0 > m1) { next[2 * i + 1].metric = m0; next[2 * i + 1].path = state[i].path << 1; }
                else { next[2 * i + 1].metric = m1; next[2 * i + 1].path = (state[i + 32].path << 1) | 1; }
            }
            std::swap(state, next);
            if (bitcnt > (uint32_t)(length - 7))
                for (int i = 1; i < 64; i += 2) state[i].metric = -9999999;
            if ((bitcnt % 8) == 5 && bitcnt > 32)
            {
                int64_t best = state[0].metric; int bs = 0;
                for (int i = 1; i < 64; ++i) if (state[i].metric > best) { best = state[i].metric; bs = i; }
                output[j++] = (uint8_t)(state[bs].path >> 24);
            }
        }
        int i = (int)(bitcnt % 8);
        if (i != 6) state[0].path <<= (6 - i);
        output[j++] = (uint8_t)(state[0].path >> 24);
        output[j++] = (uint8_t)(state[0].path >> 16);
        output[j++] = (uint8_t)(state[0].path >> 8);
        output[j]   = (uint8_t)(state[0].path);
        return output;
    }
private:
    void genMet(int amp, double esn0, double bias, int scale)
    {
        const int off = 128;
        const double sq2 = std::sqrt(2.0), log2 = std::log(2.0);
        double metrics[2][256];
        esn0 = std::pow(10.0, esn0 / 10);
        double noise = std::sqrt(0.5 / esn0);
        auto ncdf = [&](double x) { return 0.5 + 0.5 * std::erf(x / sq2); };
        double p1 = ncdf(((0 - off + 0.5) / amp - 1) / noise);
        double p0 = ncdf(((0 - off + 0.5) / amp + 1) / noise);
        metrics[0][0] = std::log(2 * p0 / (p1 + p0)) * log2 - bias;
        metrics[1][0] = std::log(2 * p1 / (p1 + p0)) * log2 - bias;
        for (int s = 1; s < 255; ++s)
        {
            p1 = ncdf(((s - off + 0.5) / amp - 1) / noise) - ncdf(((s - off - 0.5) / amp - 1) / noise);
            p0 = ncdf(((s - off + 0.5) / amp + 1) / noise) - ncdf(((s - off - 0.5) / amp + 1) / noise);
            metrics[0][s] = std::log(2 * p0 / (p1 + p0)) * log2 - bias;
            metrics[1][s] = std::log(2 * p1 / (p1 + p0)) * log2 - bias;
        }
        p1 = 1 - ncdf(((255 - off - 0.5) / amp - 1) / noise);
        p0 = 1 - ncdf(((255 - off - 0.5) / amp + 1) / noise);
        metrics[0][255] = std::log(2 * p0 / (p1 + p0)) * log2 - bias;
        metrics[1][255] = std::log(2 * p1 / (p1 + p0)) * log2 - bias;
        for (int bit = 0; bit < 2; ++bit)
            for (int s = 0; s < 256; ++s)
                mettab_[bit][s] = (int)std::floor(metrics[bit][s] * scale + 0.5);
    }
    int mettab_[2][256];
    const uint8_t partabIdx_[32] = {
        0, 1, 3, 2, 3, 2, 0, 1, 0, 1, 3, 2, 3, 2, 0, 1,
        2, 3, 1, 0, 1, 0, 2, 3, 2, 3, 1, 0, 1, 0, 2, 3 };
};

std::vector<uint8_t> descramble(const std::vector<uint8_t>& vit)
{
    auto inv = [](uint8_t in) { uint8_t r = 0; for (int b = 0; b < 8; ++b) r |= ((in >> b) & 1) << (7 - b); return r; };
    std::vector<uint8_t> dst(640);
    for (int i = 0; i < 640; ++i) dst[i] = inv(vit[i]);
    uint8_t ds[160], reg = 0x80;
    for (int i = 0; i < 160; ++i)
    {
        uint8_t x7 = reg & 0x01; ds[i] = x7;
        uint8_t x5 = (reg & 0x04) >> 2, x4 = (reg & 0x08) >> 3, x3 = (reg & 0x10) >> 4;
        uint8_t nb = x7 ^ x5 ^ x4 ^ x3;
        reg >>= 1; reg |= (uint8_t)(nb << 7);
    }
    int jj = 0;
    for (int i = 0; i < 160; ++i)
    {
        if (ds[i] == 1) for (int k = 0; k < 4; ++k) dst[jj + k] = (uint8_t)~dst[jj + k];
        jj += 4;
    }
    return dst;
}

// --- packet layer ---
int egcCrc(const uint8_t* d, int pos, int length)
{
    int16_t C0 = 0, C1 = 0;
    for (int i = 0; i < length; ++i)
    {
        uint8_t B = (i < length - 2) ? d[pos + i] : 0;
        C0 = (int16_t)(C0 + B); C1 = (int16_t)(C1 + C0);
    }
    uint8_t CB1 = (uint8_t)(C0 - C1), CB2 = (uint8_t)(C1 - 2 * C0);
    return (CB1 << 8) | CB2;
}
int packetLength(const uint8_t* f, int pos, int flen)
{
    uint8_t d = f[pos];
    if ((d >> 7) == 0) return (d & 0x0F) + 1;
    if ((d >> 6) == 0x02 && pos + 1 < flen) return f[pos + 1] + 2;
    return flen - pos;
}
bool crcOk(const uint8_t* f, int pos, int plen)
{
    if (plen < 2) return false;
    int pktCrc = (f[pos + plen - 2] << 8) | f[pos + plen - 1];
    return pktCrc == 0 || pktCrc == egcCrc(f, pos, plen);
}
int addressLength(int mt)
{
    switch (mt) {
    case 0x00: return 3;
    case 0x11: case 0x31: return 4;
    case 0x02: case 0x72: return 5;
    case 0x13: case 0x23: case 0x33: case 0x73: return 6;
    case 0x04: case 0x14: case 0x24: case 0x34: case 0x44: return 7;
    default: return 3; }
}
const char* serviceName(int code)
{
    switch (code) {
    case 0x00: return "System, All ships (general call)";
    case 0x02: return "FleetNET, Group Call";
    case 0x04: return "SafetyNET, Nav/Met/Piracy Warning to Rectangular Area";
    case 0x11: return "System, Inmarsat System Message";
    case 0x13: return "SafetyNET, Nav/Met/Piracy Coastal Warning";
    case 0x14: return "SafetyNET, Shore-to-Ship Distress Alert to Circular Area";
    case 0x23: return "System, EGC System Message";
    case 0x24: return "SafetyNET, Nav/Met/Piracy Warning to Circular Area";
    case 0x31: return "SafetyNET, NAVAREA/METAREA Warning/Forecast";
    case 0x33: return "System, Download Group Identity";
    case 0x34: return "SafetyNET, SAR Coordination to Rectangular Area";
    case 0x44: return "SafetyNET, SAR Coordination to Circular Area";
    case 0x72: return "FleetNET, Chart Correction Service";
    case 0x73: return "SafetyNET, Chart Correction Service for Fixed Areas";
    default: return "Unknown"; }
}
const char* priorityName(int p)
{
    switch (p) { case 0: return "Routine"; case 1: return "Safety";
                 case 2: return "Urgency"; case 3: return "Distress"; default: return "?"; }
}
std::string ia5Text(const uint8_t* p, int n)
{
    std::string s;
    for (int i = 0; i < n; ++i)
    {
        uint8_t c = p[i] & 0x7F;
        if (c == '\r') continue;
        if (c == '\n') { s += '\n'; continue; }
        s += (c >= 32 && c < 127) ? (char)c : '.';
    }
    return s;
}

} // namespace

struct EgcDecoder::Impl
{
    int channelId;
    double freqMHz;
    EgcLog* log;
    double mixPhase = 0.0;
    RDemodulator demod;
    UWFinder uw{25};
    Viterbi viterbi;
    std::vector<float> realBuf;
    std::vector<uint8_t> bits;
    std::vector<UWFrame> frames;
    // current frame timing (from Bulletin Board)
    int curFrameNo = 0;
    std::string curTime;
    int framesSynced = 0;
    int lastBer = -1;
    uint64_t messageCount = 0;
    // multiframe assembly
    std::vector<uint8_t> mfaData;
    int mfaExpected = 0, mfaFilled = 0;
    bool mfaActive = false;

    void emitEgc(const uint8_t* f, int pos, int plen)
    {
        int mt = f[pos + 2];
        int priority = (f[pos + 3] & 0x60) >> 5;
        int msgId = f[pos + 4] << 8 | f[pos + 5];
        int pres = f[pos + 7];
        int alen = addressLength(mt);
        int payStart = pos + 8 + alen;
        int payLen = plen - 2 - 8 - alen;
        EgcMessage m;
        m.channelId = channelId;
        m.freqMHz = freqMHz;
        m.frameNumber = curFrameNo;
        m.timeUtc = curTime;
        m.service = serviceName(mt);
        m.priority = priorityName(priority);
        m.messageId = msgId;
        m.presentation = pres;
        if (payLen > 0 && pres == 0) m.text = ia5Text(&f[payStart], payLen);
        else if (payLen > 0) m.text = "(presentation " + std::to_string(pres) + ", " +
                                      std::to_string(payLen) + " bytes)";
        ++messageCount;
        if (log) log->add(m);
    }

    void decodeFrame(const uint8_t* f, int flen)
    {
        int pos = 0;
        while (pos < flen)
        {
            uint8_t d = f[pos];
            if (d == 0x00) break;
            int plen = packetLength(f, pos, flen);
            if (plen <= 0 || pos + plen > flen) break;
            bool ok = crcOk(f, pos, plen);
            if (d == 0x7D && ok)
            {
                curFrameNo = f[pos + 2] << 8 | f[pos + 3];
                double hr = curFrameNo * 8.64 / 3600.0;
                int h = (int)hr, mn = (int)((hr - h) * 60);
                int sc = (int)((((hr - h) * 60) - mn) * 60);
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", h, mn, sc);
                curTime = buf;
            }
            else if ((d == 0xB1 || d == 0xB2) && ok)
            {
                emitEgc(f, pos, plen);
            }
            else if (d == 0xBD && ok)
            {
                int md = f[pos + 2];
                int mlen = ((md >> 7) == 0) ? (md & 0x0F) + 1
                           : ((md >> 6) == 0x02) ? f[pos + 3] + 2 : 0;
                if (mlen > 0)
                {
                    mfaData.assign(mlen, 0);
                    mfaExpected = mlen;
                    mfaFilled = plen - 4;
                    if (mfaFilled > 0 && mfaFilled <= mlen)
                        std::memcpy(mfaData.data(), &f[pos + 2], mfaFilled);
                    mfaActive = true;
                }
            }
            else if (d == 0xBE && ok && mfaActive)
            {
                int cnt = plen - 4;
                if (cnt > 0 && mfaFilled + cnt <= (int)mfaData.size())
                {
                    std::memcpy(&mfaData[mfaFilled], &f[pos + 2], cnt);
                    mfaFilled += cnt;
                }
                if (mfaFilled == mfaExpected - 2)
                {
                    uint8_t ed = mfaData[0];
                    int ep = packetLength(mfaData.data(), 0, mfaExpected);
                    if ((ed == 0xB1 || ed == 0xB2) && crcOk(mfaData.data(), 0, ep))
                        emitEgc(mfaData.data(), 0, ep);
                }
                mfaActive = false;
            }
            pos += plen;
        }
    }
};

EgcDecoder::EgcDecoder(int channelId, double freqMHz, EgcLog* log)
    : p_(new Impl)
{
    p_->channelId = channelId;
    p_->freqMHz = freqMHz;
    p_->log = log;
    p_->mixPhase = 0.0;
}

EgcDecoder::~EgcDecoder() = default;

void EgcDecoder::process(const double* iq48, int nComplex)
{
    // Up-mix the DC-centred channel to a ~2 kHz real carrier (scaled to int16-ish
    // magnitude so the demod's lock detector behaves like scytaleC).
    p_->realBuf.clear();
    p_->realBuf.reserve(nComplex);
    const double inc = 2 * M_PI * kCenterFreq / kSampleRate;
    for (int i = 0; i < nComplex; ++i)
    {
        double I = iq48[i * 2], Q = iq48[i * 2 + 1];
        double c = std::cos(p_->mixPhase), s = std::sin(p_->mixPhase);
        p_->mixPhase += inc;
        if (p_->mixPhase > 2 * M_PI) p_->mixPhase -= 2 * M_PI;
        p_->realBuf.push_back((float)((I * c - Q * s) * 20000.0));
    }

    p_->bits.clear();
    p_->demod.process(p_->realBuf.data(), (int)p_->realBuf.size(), p_->bits);

    for (uint8_t b : p_->bits)
    {
        p_->frames.clear();
        p_->uw.push(b, p_->frames);
        for (auto& fr : p_->frames)
        {
            p_->lastBer = fr.ber;
            ++p_->framesSynced;
            auto perm = depermute(fr.frame);
            auto deint = deinterleave(perm);
            auto vit = p_->viterbi.decode(deint);
            auto frame = descramble(vit);
            p_->decodeFrame(frame.data(), (int)frame.size());
        }
    }
}

bool EgcDecoder::locked() const { return p_->demod.Locked; }
int EgcDecoder::framesSynced() const { return p_->framesSynced; }
int EgcDecoder::lastBer() const { return p_->lastBer; }
uint64_t EgcDecoder::messageCount() const { return p_->messageCount; }
int EgcDecoder::getConstellation(double* iqOut, int maxPairs) const
{
    return p_->demod.scatter(iqOut, maxPairs);
}
