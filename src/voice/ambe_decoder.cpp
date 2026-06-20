#include "voice/ambe_decoder.h"

extern "C" {
#include "mbelib.h"
}

// Aero AMBE deinterleave tables (libaeroambe, Jonti Olds).
static const unsigned char rX[96] = {
    23, 11, 14, 2, 5, 8, 9, 11, 22, 10, 13, 1, 4, 7, 8, 10, 21, 9, 12, 0, 3, 6, 7, 9,
    20, 8, 11, 14, 2, 5, 6, 8, 19, 7, 10, 13, 1, 4, 5, 7, 18, 6, 9, 12, 0, 3, 4, 6,
    17, 5, 8, 11, 14, 2, 3, 5, 16, 4, 7, 10, 13, 1, 2, 4, 15, 3, 6, 9, 12, 0, 1, 3,
    14, 2, 5, 8, 11, 12, 0, 2, 13, 1, 4, 7, 10, 11, 13, 1, 12, 0, 3, 6, 9, 10, 12, 0};

static const unsigned char rW[96] = {
    0, 0, 1, 1, 2, 3, 4, 5, 0, 0, 1, 1, 2, 3, 4, 5, 0, 0, 1, 1, 2, 3, 4, 5,
    0, 0, 1, 2, 2, 3, 4, 5, 0, 0, 1, 2, 2, 3, 4, 5, 0, 0, 1, 2, 2, 3, 4, 5,
    0, 0, 1, 2, 3, 3, 4, 5, 0, 0, 1, 2, 3, 3, 4, 5, 0, 0, 1, 2, 3, 3, 4, 5,
    0, 0, 1, 2, 3, 4, 4, 5, 0, 0, 1, 2, 3, 4, 5, 5, 0, 0, 1, 2, 3, 4, 5, 5};

struct AmbeDecoder::Impl
{
    mbe_parms current;
    mbe_parms prev;
    mbe_parms prev_mp_enhanced;
    char ambe_fr[6][24];
    char ambe_d[72];
    char errbuf[1024];
};

AmbeDecoder::AmbeDecoder() : impl_(new Impl)
{
    mbe_initMbeParms(&impl_->current, &impl_->prev, &impl_->prev_mp_enhanced);
}

AmbeDecoder::~AmbeDecoder() = default;

void AmbeDecoder::decode(const uint8_t* data, int16_t out[kPcmSamples])
{
    unsigned char bits[96];
    for (int x = 0; x < kFrameBytes; ++x)
        for (int i = 0; i < 8; ++i)
            bits[x * 8 + i] = (unsigned char)((data[x] >> i) & 0x01); // reversed

    for (int i = 0; i < 96; ++i)
        impl_->ambe_fr[rW[i]][rX[i]] = (char)bits[i];

    int errs = 0, errs2 = 0;
    mbe_processAmbe4800x3600Frame(out, &errs, &errs2, impl_->errbuf, impl_->ambe_fr,
                                  impl_->ambe_d, &impl_->current, &impl_->prev,
                                  &impl_->prev_mp_enhanced, 1);
}
