// In-app EGC self-test: feed the 48 kHz IQ WAV straight into the integrated
// EgcDecoder (the same class InmarScope uses) and print decoded messages.
// Build (MSYS2 MINGW64):
//   g++ -O2 -std=gnu++17 -Isrc tools/egc_inapp_test.cpp src/decode/egc/egc_decoder.cpp -o build/egc_inapp_test.exe
// Run:
//   ./build/egc_inapp_test.exe "reference/Inmarsat-C TDM EGC.wav"
#include "decode/egc/egc_decoder.h"
#include "decode/message_log.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

static uint32_t rd32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }

int main(int argc, char** argv)
{
    const char* path = (argc > 1) ? argv[1] : "reference/Inmarsat-C TDM EGC.wav";
    FILE* f = std::fopen(path, "rb");
    if (!f) { std::fprintf(stderr, "cannot open %s\n", path); return 1; }
    uint8_t hdr[12];
    std::fread(hdr, 1, 12, f);
    int channels = 0, rate = 0, bits = 0; long dataOff = -1, dataSz = 0;
    while (true)
    {
        uint8_t ch[8];
        if (std::fread(ch, 1, 8, f) != 8) break;
        uint32_t sz = rd32(ch + 4);
        if (!std::memcmp(ch, "fmt ", 4)) {
            std::vector<uint8_t> fmt(sz); std::fread(fmt.data(), 1, sz, f);
            channels = rd16(&fmt[2]); rate = (int)rd32(&fmt[4]); bits = rd16(&fmt[14]);
        } else if (!std::memcmp(ch, "data", 4)) {
            dataOff = std::ftell(f); dataSz = (long)sz; std::fseek(f, sz, SEEK_CUR);
        } else std::fseek(f, sz, SEEK_CUR);
        if (sz & 1) std::fseek(f, 1, SEEK_CUR);
    }
    if (dataOff < 0 || bits != 16) { std::fprintf(stderr, "need 16-bit PCM\n"); return 1; }
    std::printf("WAV: %d Hz, %d ch, %d-bit\n", rate, channels, bits);

    std::fseek(f, dataOff, SEEK_SET);
    int nSamp = (int)(dataSz / 2);
    std::vector<int16_t> raw(nSamp);
    std::fread(raw.data(), 2, nSamp, f);
    std::fclose(f);
    int frames = channels ? nSamp / channels : 0;

    EgcLog log;
    EgcDecoder dec(1, 1541.45, &log); // channelId, freqMHz (label), log

    std::vector<double> block;
    const int chunk = 4096;
    for (int off = 0; off < frames; off += chunk)
    {
        int len = std::min(chunk, frames - off);
        block.resize((size_t)len * 2);
        for (int i = 0; i < len; ++i)
        {
            block[i * 2] = raw[(size_t)(off + i) * channels] / 32768.0;
            block[i * 2 + 1] = (channels > 1) ? raw[(size_t)(off + i) * channels + 1] / 32768.0 : 0.0;
        }
        dec.process(block.data(), len);
    }

    std::printf("framesSynced=%d lastBER=%d messages=%llu locked=%d\n",
                dec.framesSynced(), dec.lastBer(),
                (unsigned long long)dec.messageCount(), (int)dec.locked());

    auto msgs = log.snapshot();
    std::printf("--- %zu EGC message(s) ---\n", msgs.size());
    for (auto& m : msgs)
    {
        std::printf("[%s] %s | %s msgId=%d\n", m.timeUtc.c_str(), m.service.c_str(),
                    m.priority.c_str(), m.messageId);
        std::printf("  %s\n", m.text.c_str());
    }
    return 0;
}
