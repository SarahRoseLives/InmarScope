// A single-channel decoder: DDC -> JAERO continuous MSK demod -> AeroL.
// ACARS frames are pushed to a MessageLog.
#pragma once

#include "decode/message_log.h"
#include "dsp/ddc.h"
#include "voice/wav_writer.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct jaero_pmsk_demod;       // from jaero_demod.h
struct jaero_oqpsk_cont_demod; // from jaero_demod.h
struct jaero_acars_msg;        // from jaero_demod.h
class AmbeDecoder;
class AudioOutput;
class WavWriter;
class EgcDecoder;

// Special "baud" code selecting the Inmarsat-C / EGC decoder.
static constexpr int kEgcBaud = 1;

class Decoder
{
public:
    // subRate/subCenterHz describe the shared front-end sub-band stream this
    // decoder consumes; chanFreqHz is the absolute channel frequency.
    Decoder(double subRate, double subCenterHz, double chanFreqHz, int baud,
            int channelId, MessageLog* log, MessageLog* suLog, AudioOutput* audioSink,
            CassignLog* cassignLog, ChannelTable* netTable, EgcLog* egcLog = nullptr,
            AircraftTable* acTable = nullptr,
            MesLog* mesLog = nullptr, LesLog* lesLog = nullptr);
    ~Decoder();

    // Process a block of sub-band interleaved double IQ (decode thread).
    void process(const double* iq, int nComplex);

    // Retune to a new absolute channel frequency (Hz).
    void setFreq(double chanFreqHz);

    bool   locked() const;
    double ebno() const;
    double mse() const;
    // Copy up to maxPairs constellation points (interleaved I,Q doubles into
    // iqOut, capacity >= 2*maxPairs). Returns the number of pairs written.
    int    getConstellation(double* iqOut, int maxPairs) const;
    double freqMHz() const { return chanFreqHz_ / 1e6; }
    int    baud() const { return baud_; }
    int    channelId() const { return channelId_; }
    uint64_t msgCount() const { return msgCount_.load(); }
    uint64_t voiceFrames() const { return voiceFrames_.load(); } // decoded AMBE frames
    void   setVoiceAesId(uint32_t id) { voiceAesId_ = id; } // aircraft AES for recording tag
    uint32_t voiceAesId() const { return voiceAesId_; }
    bool   isVoice() const { return baud_ == 8400; }
    bool   isEgc() const { return baud_ == kEgcBaud; }
    int    egcBer() const;    // -1 if not EGC
    int    egcFrames() const; // 0 if not EGC
    void   setMonitored(bool on) { monitored_.store(on); }
    bool   monitored() const { return monitored_.load(); }

    // CPU reduction: slow down coarse frequency estimator (cuts demod CPU ~30%).
    void   setCpuReduce(bool on);

    // Voice call recording (8400 only). When enabled, each contiguous voice
    // call is written to its own WAV file in dir, regardless of monitoring.
    void   setRecording(bool on, const std::string& dir,
                        RecordFormat fmt = RecordFormat::WAV);
    bool   recordEnabled() const { return record_.load(); }
    bool   recordingNow() const { return recActive_.load(); }

private:
    static void acarsTrampoline(const uint8_t* data, int len, int channel_id,
                                uint32_t aes_id, uint8_t ges_id, uint8_t qno,
                                uint8_t refno, int downlink, void* user);
    void onAcars(const uint8_t* data, int len, uint32_t aes_id, uint8_t ges_id,
                 int downlink);
    static void acars2Trampoline(int channel_id, const jaero_acars_msg* msg,
                                 void* user);
    void onAcars2(const jaero_acars_msg* msg);
    static void decodedTrampoline(const uint8_t* data, int len, int channel_id,
                                  void* user);
    void onDecoded(const uint8_t* data, int len);
    static void cassignTrampoline(int channel_id, uint8_t type, uint32_t aes_id,
                                  uint8_t ges_id, double rx_mhz, double tx_mhz,
                                  void* user);
    void onCassign(uint8_t type, uint32_t aes_id, uint8_t ges_id, double rx_mhz,
                   double tx_mhz);
    static void voiceTrampoline(const uint8_t* frame, int len, int channel_id,
                                void* user);
    void onVoice(const uint8_t* frame, int len);

    Ddc ddc_;
    jaero_pmsk_demod* pmsk_ = nullptr;
    jaero_oqpsk_cont_demod* oqpsk_ = nullptr;
    std::vector<double> ddcOut_;
    MessageLog* log_;
    MessageLog* suLog_;
    CassignLog* cassignLog_;
    ChannelTable* netTable_;
    AircraftTable* acTable_ = nullptr;

    double subCenterHz_;
    double chanFreqHz_;
    int baud_;
    int channelId_;
    std::atomic<uint64_t> msgCount_{0};
    std::atomic<uint64_t> voiceFrames_{0};

    std::unique_ptr<AmbeDecoder> ambe_; // voice (8400) only
    AudioOutput* audioSink_ = nullptr;
    std::atomic<bool> monitored_{false};

    std::unique_ptr<EgcDecoder> egc_; // Inmarsat-C / EGC only
    EgcLog* egcLog_ = nullptr;

    // Recording state (worker thread only, except the atomics).
    void maintainRecording();
    void recordPcm(const int16_t* pcm, int n);
    std::atomic<bool> record_{false};   // recording requested
    std::atomic<bool> recActive_{false}; // a call file is currently open
    std::uint32_t voiceAesId_ = 0;      // AES id of the voice call (for recording tag)
    RecordFormat recordFmt_ = RecordFormat::WAV;
    std::string recordDir_ = "recordings";
    std::unique_ptr<WavWriter> rec_;
    std::chrono::steady_clock::time_point lastVoiceTime_;
    std::chrono::steady_clock::time_point firstPcmTime_;
    std::vector<int16_t> pcmBuf_;       // buffer early PCM before ICAO known
};
