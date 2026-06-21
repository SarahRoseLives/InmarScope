// Standalone Inmarsat-C / EGC decoder test (ported from scytaleC, GPL-3.0).
// Phase A: WAV IQ (48 kHz, 16-bit stereo) -> up-mix to ~2 kHz real audio ->
// RDemodulator (BPSK carrier+clock recovery) -> hard bitstream.
//
// Build (MSYS2 MINGW64):
//   g++ -O2 -std=gnu++17 tools/egc_decode.cpp -o build/egc_decode.exe
// Run:
//   ./build/egc_decode.exe "reference/Inmarsat-C TDM EGC.wav" bits.bin
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <string>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ---------------------------------------------------------------------------
// Minimal WAV reader (16-bit PCM, mono or stereo). Returns interleaved float
// channels in [-1,1]. For stereo IQ: ch0 = I, ch1 = Q.
// ---------------------------------------------------------------------------
struct Wav
{
    int sampleRate = 0;
    int channels = 0;
    int bits = 0;
    std::vector<float> data; // interleaved, channels per frame
    int frames() const { return channels ? (int)data.size() / channels : 0; }
};

static uint32_t rd32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }

static bool loadWav(const char* path, Wav& w)
{
    FILE* f = std::fopen(path, "rb");
    if (!f) { std::fprintf(stderr, "cannot open %s\n", path); return false; }
    uint8_t hdr[12];
    if (std::fread(hdr, 1, 12, f) != 12 || std::memcmp(hdr, "RIFF", 4) || std::memcmp(hdr + 8, "WAVE", 4))
    { std::fclose(f); std::fprintf(stderr, "not a WAV\n"); return false; }

    long dataOffset = -1, dataSize = 0;
    while (true)
    {
        uint8_t ch[8];
        if (std::fread(ch, 1, 8, f) != 8) break;
        uint32_t sz = rd32(ch + 4);
        if (!std::memcmp(ch, "fmt ", 4))
        {
            std::vector<uint8_t> fmt(sz);
            if (std::fread(fmt.data(), 1, sz, f) != sz) break;
            w.channels = rd16(&fmt[2]);
            w.sampleRate = (int)rd32(&fmt[4]);
            w.bits = rd16(&fmt[14]);
        }
        else if (!std::memcmp(ch, "data", 4))
        {
            dataOffset = std::ftell(f);
            dataSize = (long)sz;
            std::fseek(f, sz, SEEK_CUR);
        }
        else
        {
            std::fseek(f, sz, SEEK_CUR);
        }
        if (sz & 1) std::fseek(f, 1, SEEK_CUR); // pad byte
    }
    if (dataOffset < 0 || w.bits != 16)
    { std::fclose(f); std::fprintf(stderr, "need 16-bit PCM data chunk\n"); return false; }

    std::fseek(f, dataOffset, SEEK_SET);
    int n = (int)(dataSize / 2); // number of int16 samples
    std::vector<int16_t> raw(n);
    if ((int)std::fread(raw.data(), 2, n, f) != n) { /* tolerate short */ }
    std::fclose(f);
    w.data.resize(n);
    for (int i = 0; i < n; ++i)
        w.data[i] = raw[i] * (1.0f / 32768.0f);
    return true;
}

// ---------------------------------------------------------------------------
// RDemodulator - faithful port of scytaleC ScytaleC/Demodulator/RDemodulator.cs
// Operates on a real signal at 48 kHz with the BPSK carrier near 2 kHz.
// Emits hard bits (0/1) via the provided sink.
// ---------------------------------------------------------------------------
static constexpr double kSampleRate = 48000.0;
static constexpr int    kSymbolRate = 1200;
static constexpr double kCenterFreq = 2000.0;

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

    // Process a chunk of real samples. Appends hard bits (0/1) to outBits.
    void process(const float* real, int length, std::vector<uint8_t>& outBits)
    {
        double magnitude = 0.0;
        for (int i = 0; i < length; ++i)
            magnitude += std::fabs((double)real[i]);
        gaincontrol = magnitude / (length ? length : 1);
        int gaincontrolout = (int)gaincontrol;
        if (gaincontrol == 0.0) gaincontrol = 1.0;

        double locki = 0.0, lockq = 0.0;
        for (int i = 0; i < length; ++i)
        {
            thetacarrier = std::fmod(thetacarrier, 2 * M_PI);
            double icarrierosc = std::cos(thetacarrier);
            double qcarrierosc = std::sin(thetacarrier);
            double gcs = real[i] / gaincontrol;
            double isample = icarrierosc * gcs;
            double qsample = qcarrierosc * gcs;
            isb_[0] = isample;
            qsb_[0] = qsample;
            double cqsamplefilt = 0.0;
            for (int j = 0; j < order; ++j)
                cqsamplefilt += a_[j] * qsb_[j];

            std::memmove(&isb_[1], &isb_[0], (order - 1) * sizeof(double));
            std::memmove(&qsb_[1], &qsb_[0], (order - 1) * sizeof(double));
            isamplefilt = 0.9 * isamplefilt + 0.1 * isample;
            qsamplefilt = 0.9 * qsamplefilt + 0.1 * qsample;
            locki = isamplefilt * isamplefilt;
            lockq = qsamplefilt * qsamplefilt;
            double filteredphase = qsamplefilt * isamplefilt;
            wcarrier += beta * ncocontrolcarrier;
            thetacarrier += wcarrier + 0.03 * ncocontrolcarrier;
            ncocontrolcarrier = filteredphase;
            if (wcarrier < lowerafclimit) wcarrier = lowerafclimit;
            if (wcarrier > upperafclimit) wcarrier = upperafclimit;

            double thetaclock = thetaclockminusone + wclock + -0.16 * ncocontrolclock;
            thetaclock = std::fmod(thetaclock, 2 * M_PI);
            double sinclockosc = std::sin(thetaclock);
            wclock += betaclock * ncocontrolclock;
            int clock = (sinclockosc >= 0.0) ? 1 : -1;

            if (thetaclockminusone < M_PI && thetaclock >= M_PI)
            {
                int decision = (qsamplefiltminusone > 0.0) ? 1 : -1;
                double steepness = cqsamplefilt - cqsamplefiltminusthree;
                ncocontrolclock = decision * steepness;
            }

            charge += qsample;
            if (clock == 1 && clockminusone == -1)
            {
                if (charge > 0.0) databit = true;
                if (charge < 0.0) databit = false;
                charge = 0.0;
                outBits.push_back(databit ? 1 : 0);
            }
            thetaclockminusone = thetaclock;
            cqsamplefiltminusthree = qsamplefiltminustwo;
            qsamplefiltminustwo = qsamplefiltminusone;
            qsamplefiltminusone = cqsamplefilt;
            clockminusone = clock;
        }

        double lock = (lockq != 0.0) ? locki / lockq : 1.0;
        if (lock < lock_lp) lock_lp = lock_lp * 0.9 + lock * 0.1;
        else lock_lp = lock_lp * 0.99 + lock * 0.01;
        if (lock_lp < 0.4 && gaincontrolout > 50) Locked = true;
        if (lock_lp >= 1.0) Locked = false;
    }

    bool Locked = false;

private:
    static constexpr int order = 64;
    std::vector<double> a_, isb_, qsb_;
    double wcarrier = 0, wclock = 0;
    double charge = 0.0;
    bool databit = false;
    double beta = 0.00022499999999999999;
    double betaclock = 0.0;
    double thetacarrier = 0.0, isamplefilt = 0.0, qsamplefilt = 0.0, ncocontrolcarrier = 0.0;
    double thetaclockminusone = 0.0;
    int clockminusone = 0;
    double ncocontrolclock = 0.0;
    double qsamplefiltminustwo = 0.0, qsamplefiltminusone = 0.0;
    double gaincontrol = 1.0, cqsamplefiltminusthree = 0.0, lock_lp = 0.0;
    double lowerafclimit = 0, upperafclimit = 0;
};

// ---------------------------------------------------------------------------
// UWFinder - faithful port of scytaleC ScytaleC.Decoder/Decoders/UWFinder.cs
// Finds the 10368-symbol frame via the 64-symbol unique word in the first two
// interleaver columns. Emits 10368-byte frames (polarity-corrected).
// ---------------------------------------------------------------------------
static constexpr int kUWFrameLen = 10368;

struct UWFrame
{
    std::vector<uint8_t> frame; // 10368 symbols
    bool reversedPolarity = false;
    int ber = 0;
};

class UWFinder
{
public:
    explicit UWFinder(int tol = 25) : tolerance_(tol)
    {
        reg_.assign(kUWFrameLen * 2, 0);
        for (int i = 0; i < 64; ++i) revPol_[i] = nrmPol_[i] ? 0 : 1;
    }

    // Feed one symbol (hard bit). Pushes a detected frame to 'out' if found.
    void push(uint8_t sym, std::vector<UWFrame>& out)
    {
        std::memmove(&reg_[1], &reg_[0], (reg_.size() - 1) * sizeof(uint8_t));
        reg_[0] = sym;
        ++symbolCount_;
        if (symbolCount_ < kUWFrameLen)
            return;

        int nUW, rUW;
        bool revPol;
        if (detect(true, nUW, rUW, revPol))
        {
            UWFrame f;
            f.frame.assign(reg_.begin(), reg_.begin() + kUWFrameLen);
            f.reversedPolarity = revPol;
            f.ber = std::min(nUW, rUW);
            if (revPol)
                for (auto& b : f.frame) b ^= 1;
            out.push_back(std::move(f));
            symbolCount_ = 0;
        }
    }

private:
    bool detect(bool lowest, int& nUW, int& rUW, bool& revPol)
    {
        nUW = 0; rUW = 0;
        int patternPos = 0;
        int symbolPos = kUWFrameLen - 1, minPos = 0;
        if (!lowest) { symbolPos = 2 * kUWFrameLen - 1; minPos = kUWFrameLen; }
        for (; symbolPos >= minPos; symbolPos -= 162)
        {
            nUW += nrmPol_[patternPos] ^ reg_[symbolPos];
            nUW += nrmPol_[patternPos] ^ reg_[symbolPos - 1];
            rUW += revPol_[patternPos] ^ reg_[symbolPos];
            rUW += revPol_[patternPos] ^ reg_[symbolPos - 1];
            ++patternPos;
        }
        revPol = rUW <= tolerance_;
        return nUW <= tolerance_ || rUW <= tolerance_;
    }

    const uint8_t nrmPol_[64] = {
        0,0,0,0, 0,1,1,1, 1,1,1,0, 1,0,1,0,
        1,1,0,0, 1,1,0,1, 1,1,0,1, 1,0,1,0,
        0,1,0,0, 1,1,1,0, 0,0,1,0, 1,1,1,1,
        0,0,1,0, 1,0,0,0, 1,1,0,0, 0,0,1,0 };
    uint8_t revPol_[64];
    std::vector<uint8_t> reg_;
    int symbolCount_ = kUWFrameLen - 1;
    int tolerance_;
};

// ---------------------------------------------------------------------------
// Depermuter: reverse the 10368-symbol frame then de-permute the 64 rows.
// ---------------------------------------------------------------------------
static std::vector<uint8_t> depermute(std::vector<uint8_t> frame /*10368*/)
{
    std::reverse(frame.begin(), frame.end());
    std::vector<uint8_t> dst(kUWFrameLen);
    int perm[64];
    for (int i = 0; i < 64; ++i) perm[i] = ((i * 23) % 64 & 0x3F) * 162;
    for (int i = 0; i < 64; ++i)
        std::memcpy(&dst[i * 162], &frame[perm[i]], 162);
    return dst;
}

// ---------------------------------------------------------------------------
// Deinterleaver: 64x162 (drop 2 UW cols) -> read transposed -> 10240 symbols.
// ---------------------------------------------------------------------------
static std::vector<uint8_t> deinterleave(const std::vector<uint8_t>& perm /*10368*/)
{
    static uint8_t mat[64][160];
    int row = -1, column = 0;
    for (int i = 0; i < kUWFrameLen; ++i)
    {
        if (i % 162 == 0) { column = 0; ++row; i += 2; }
        if (row < 64 && column < 160) mat[row][column] = perm[i];
        ++column;
    }
    std::vector<uint8_t> dst(10240);
    int pos = 0; row = 0; column = 0;
    while (row < 64)
    {
        dst[pos] = mat[row][column];
        ++row;
        if (row % 64 == 0) { row = 0; ++column; if (column == 160) break; }
        ++pos;
    }
    return dst;
}

// ---------------------------------------------------------------------------
// Viterbi: Phil Karn K=7 rate-1/2 soft-decision decoder (scytaleC port).
// ---------------------------------------------------------------------------
class Viterbi
{
public:
    Viterbi() { genMet(100, 5.0, 0.0, 4); }

    std::vector<uint8_t> decode(const std::vector<uint8_t>& deint /*10240, hard 0/1*/)
    {
        const int length = (int)deint.size();
        const int nbits = length / 16;
        std::vector<uint8_t> input(length);
        for (int k = 0; k < length; ++k)
            input[k] = deint[k] == 0 ? 28 : 228; // hard -> soft

        struct St { uint32_t path; int64_t metric; };
        std::vector<St> state(64), next(64);
        for (int i = 0; i < 64; ++i) { state[i].path = 0; state[i].metric = (i == 0) ? 0 : -999999; }

        std::vector<uint8_t> output(640, 0);
        int mets[4];
        int inputCounter = 0, j = 0;
        uint32_t bitcnt = 0;
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

// ---------------------------------------------------------------------------
// Descrambler: invert bits in each byte, then complement the 4-byte groups
// flagged by the G = X3+X4+X5+X7 LFSR sequence (init 0x80).
// ---------------------------------------------------------------------------
static std::vector<uint8_t> descramble(const std::vector<uint8_t>& vit /*640*/)
{
    auto invertBits = [](uint8_t in) {
        uint8_t r = 0;
        for (int b = 0; b < 8; ++b) r |= ((in >> b) & 1) << (7 - b);
        return r;
    };
    std::vector<uint8_t> dst(640);
    for (int i = 0; i < 640; ++i) dst[i] = invertBits(vit[i]);

    uint8_t descram[160];
    uint8_t reg = 0x80;
    for (int i = 0; i < 160; ++i)
    {
        uint8_t x7 = reg & 0x01;
        descram[i] = x7;
        uint8_t x5 = (reg & 0x04) >> 2, x4 = (reg & 0x08) >> 3, x3 = (reg & 0x10) >> 4;
        uint8_t nb = x7 ^ x5 ^ x4 ^ x3;
        reg >>= 1;
        reg |= (uint8_t)(nb << 7);
    }
    int jj = 0;
    for (int i = 0; i < 160; ++i)
    {
        if (descram[i] == 1)
            for (int k = 0; k < 4; ++k) dst[jj + k] = (uint8_t)~dst[jj + k];
        jj += 4;
    }
    return dst;
}

// ---------------------------------------------------------------------------
// Packet layer: walk a 640-byte descrambled frame, extract Bulletin Board and
// EGC messages (B1/B2 + BD/BE multiframe). Ported from scytaleC PacketDecoders.
// ---------------------------------------------------------------------------
static int egcCrc(const uint8_t* d, int pos, int length)
{
    int16_t C0 = 0, C1 = 0;
    for (int i = 0; i < length; ++i)
    {
        uint8_t B = (i < length - 2) ? d[pos + i] : 0;
        C0 = (int16_t)(C0 + B);
        C1 = (int16_t)(C1 + C0);
    }
    uint8_t CB1 = (uint8_t)(C0 - C1);
    uint8_t CB2 = (uint8_t)(C1 - 2 * C0);
    return (CB1 << 8) | CB2;
}

static int packetLength(const uint8_t* f, int pos, int flen)
{
    uint8_t d = f[pos];
    if ((d >> 7) == 0) return (d & 0x0F) + 1;             // short
    if ((d >> 6) == 0x02 && pos + 1 < flen) return f[pos + 1] + 2; // medium
    return flen - pos;                                    // fallback
}

static bool crcOk(const uint8_t* f, int pos, int plen)
{
    if (plen < 2) return false;
    int pktCrc = (f[pos + plen - 2] << 8) | f[pos + plen - 1];
    int comp = egcCrc(f, pos, plen);
    return pktCrc == 0 || pktCrc == comp;
}

static int addressLength(int mt)
{
    switch (mt) {
    case 0x00: return 3;
    case 0x11: case 0x31: return 4;
    case 0x02: case 0x72: return 5;
    case 0x13: case 0x23: case 0x33: case 0x73: return 6;
    case 0x04: case 0x14: case 0x24: case 0x34: case 0x44: return 7;
    default: return 3;
    }
}

static const char* serviceName(int code)
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
    default: return "Unknown";
    }
}

static const char* priorityName(int p)
{
    switch (p) { case 0: return "Routine"; case 1: return "Safety";
                 case 2: return "Urgency"; case 3: return "Distress"; default: return "?"; }
}

struct Mfa { std::vector<uint8_t> data; int expected = 0; int filled = 0; bool active = false; };

static void printIa5(const uint8_t* p, int n)
{
    for (int i = 0; i < n; ++i)
    {
        uint8_t c = p[i] & 0x7F;
        if (c == '\r') continue;
        if (c == '\n') { std::printf("\n        "); continue; }
        std::putchar((c >= 32 && c < 127) ? c : '.');
    }
}

// Decode an EGC header packet (B1/B2 or an assembled multiframe B1/B2).
static void decodeEgc(const uint8_t* f, int pos, int plen, const char* tag)
{
    int mt = f[pos + 2];
    int priority = (f[pos + 3] & 0x60) >> 5;
    int rep = f[pos + 3] & 0x1F;
    int msgId = f[pos + 4] << 8 | f[pos + 5];
    int packetNo = f[pos + 6];
    int pres = f[pos + 7];
    int alen = addressLength(mt);
    int payStart = pos + 8 + alen;
    int payLen = plen - 2 - 8 - alen;
    std::printf("  [%s] %s | prio=%s rep=%d msgId=%d pkt=%d pres=%d\n", tag,
                serviceName(mt), priorityName(priority), rep, msgId, packetNo, pres);
    if (payLen > 0)
    {
        std::printf("        ");
        if (pres == 0) printIa5(&f[payStart], payLen);     // IA5 text
        else std::printf("(presentation %d, %d bytes)", pres, payLen);
        std::printf("\n");
    }
}

// Walk one frame (or an assembled multiframe buffer). mfa persists across frames.
static void decodeFrame(const uint8_t* f, int flen, Mfa& mfa)
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
            int frameNo = f[pos + 2] << 8 | f[pos + 3];
            double hr = frameNo * 8.64 / 3600.0;
            int h = (int)hr, m = (int)((hr - h) * 60), s = (int)((((hr - h) * 60) - m) * 60);
            std::printf("  [BB] frame#%d  %02d:%02d:%02dUTC\n", frameNo, h, m, s);
        }
        else if ((d == 0xB1 || d == 0xB2) && ok)
        {
            decodeEgc(f, pos, plen, d == 0xB1 ? "EGC-B1" : "EGC-B2");
        }
        else if (d == 0xBD && ok)
        {
            int md = f[pos + 2];
            int mlen = ((md >> 7) == 0) ? (md & 0x0F) + 1
                       : ((md >> 6) == 0x02) ? f[pos + 3] + 2 : 0;
            if (mlen > 0)
            {
                mfa.data.assign(mlen, 0);
                mfa.expected = mlen;
                mfa.filled = plen - 4;
                if (mfa.filled > 0 && mfa.filled <= mlen)
                    std::memcpy(mfa.data.data(), &f[pos + 2], mfa.filled);
                mfa.active = true;
            }
        }
        else if (d == 0xBE && ok)
        {
            if (mfa.active)
            {
                int cnt = plen - 4;
                if (cnt > 0 && mfa.filled + cnt <= (int)mfa.data.size())
                {
                    std::memcpy(&mfa.data[mfa.filled], &f[pos + 2], cnt);
                    mfa.filled += cnt;
                }
                if (mfa.filled == mfa.expected - 2)
                {
                    // Assembled: decode the encapsulated packet (usually B1/B2).
                    uint8_t ed = mfa.data[0];
                    int ep = packetLength(mfa.data.data(), 0, mfa.expected);
                    if ((ed == 0xB1 || ed == 0xB2) && crcOk(mfa.data.data(), 0, ep))
                        decodeEgc(mfa.data.data(), 0, ep, "EGC-MF");
                }
                mfa.active = false;
            }
        }
        pos += plen;
    }
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: %s <iq.wav> [bits_out.bin] [mixHz=2000]\n", argv[0]);
        return 1;
    }
    const char* wavPath = argv[1];
    const char* outPath = (argc > 2) ? argv[2] : nullptr;
    double mixHz = (argc > 3) ? std::atof(argv[3]) : 2000.0;

    Wav w;
    if (!loadWav(wavPath, w)) return 1;
    std::printf("WAV: %d Hz, %d ch, %d-bit, %d frames (%.1f s)\n", w.sampleRate,
                w.channels, w.bits, w.frames(), w.frames() / (double)w.sampleRate);
    if (w.sampleRate != 48000)
        std::printf("WARNING: expected 48000 Hz; got %d\n", w.sampleRate);

    // Build a real signal with the baseband carrier shifted to ~mixHz.
    int n = w.frames();
    std::vector<float> realSig(n);
    double w0 = 2 * M_PI * mixHz / w.sampleRate;
    for (int i = 0; i < n; ++i)
    {
        double I = w.data[(size_t)i * w.channels];
        double Q = (w.channels > 1) ? w.data[(size_t)i * w.channels + 1] : 0.0;
        double c = std::cos(w0 * i), s = std::sin(w0 * i);
        realSig[i] = (float)((I * c - Q * s) * 20000.0); // up-mix; int16-ish scale
    }

    RDemodulator demod;
    std::vector<uint8_t> bits;
    bits.reserve(n / 40 + 16);
    int lockChanges = 0;
    bool prevLock = false;
    const int chunk = 4096;
    for (int off = 0; off < n; off += chunk)
    {
        int len = std::min(chunk, n - off);
        demod.process(&realSig[off], len, bits);
        if (demod.Locked != prevLock) { ++lockChanges; prevLock = demod.Locked; }
    }

    std::printf("demodulated %zu hard bits (~%.0f symbols/s), final lock=%d, lockChanges=%d\n",
                bits.size(), bits.size() / (n / kSampleRate), (int)demod.Locked, lockChanges);

    // Frame sync via the unique word.
    UWFinder uw(25);
    std::vector<UWFrame> frames;
    for (uint8_t b : bits)
        uw.push(b, frames);
    std::printf("UWFinder: %zu frame(s) detected\n", frames.size());

    // Full decode chain: depermute -> deinterleave -> viterbi -> descramble.
    Viterbi viterbi;
    Mfa mfa;
    std::vector<std::vector<uint8_t>> outFrames;
    for (size_t i = 0; i < frames.size(); ++i)
    {
        auto perm = depermute(frames[i].frame);
        auto deint = deinterleave(perm);
        auto vit = viterbi.decode(deint);
        auto frame = descramble(vit);
        outFrames.push_back(frame);
        std::printf("frame %zu (BER=%d):\n", i, frames[i].ber);
        decodeFrame(frame.data(), (int)frame.size(), mfa);
    }

    if (outPath)
    {
        FILE* o = std::fopen(outPath, "wb");
        if (o)
        {
            for (auto& f : outFrames) std::fwrite(f.data(), 1, f.size(), o);
            std::fclose(o);
            std::printf("wrote %zu decoded frame(s) (%zu bytes) to %s\n",
                        outFrames.size(), outFrames.size() * 640, outPath);
        }
    }
    return 0;
}
