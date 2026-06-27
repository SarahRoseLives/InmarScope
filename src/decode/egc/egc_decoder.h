// Inmarsat-C / EGC decoder. Consumes 48 kHz complex baseband (channel at DC,
// produced by the shared DDC), up-mixes to a ~2 kHz real carrier, and runs the
// scytaleC-derived chain: RDemodulator -> UW frame sync -> depermute ->
// deinterleave -> Viterbi (K=7 r1/2) -> descramble -> packet/EGC parse.
// Ported from scytaleC (GPL-3.0).
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

class EgcLog;
class MesLog;
class LesLog;
class LesFreqTable;

class EgcDecoder
{
public:
    EgcDecoder(int channelId, double freqMHz, double sampleRate, EgcLog* log,
               MesLog* mesLog = nullptr, LesLog* lesLog = nullptr,
               LesFreqTable* lesFreqTable = nullptr);
    ~EgcDecoder();

    // Feed a block of 48 kHz complex baseband (interleaved double I,Q).
    void process(const double* iq48, int nComplex);

    bool locked() const;
    int framesSynced() const;       // UW frames detected so far
    int lastBer() const;            // unique-word errors of last frame (0=perfect, -1=none)
    uint64_t messageCount() const;  // EGC messages emitted
    // Copy up to maxPairs BPSK soft symbols (interleaved I,Q doubles). Returns pairs.
    int getConstellation(double* iqOut, int maxPairs) const;

    // Channel type from Bulletin Board: 1=NCS, 2=LES TDM, 3=Joint, 4=Standby NCS, 0=unknown
    int channelType() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};
