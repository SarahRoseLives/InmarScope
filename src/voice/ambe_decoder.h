// Aero AMBE voice decoder: 12-byte AMBE+2 (4800x3600) frame -> 160 int16 PCM
// samples (8 kHz). De-Qt'd from libaeroambe (Jonti Olds); uses mbelib.
//
// PATENT NOTICE: AMBE voice coding may be covered by patents. Provided for
// educational use; check licensing before use.
#pragma once

#include <cstdint>
#include <memory>

class AmbeDecoder
{
public:
    AmbeDecoder();
    ~AmbeDecoder();

    static constexpr int kFrameBytes = 12;  // one AMBE frame
    static constexpr int kPcmSamples = 160; // 20 ms @ 8 kHz

    // Decode one 12-byte AMBE frame into 160 int16 PCM samples.
    void decode(const uint8_t* frame12, int16_t out[kPcmSamples]);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
