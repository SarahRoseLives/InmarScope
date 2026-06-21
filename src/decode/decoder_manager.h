// Owns the active decoders, grouped into sub-bands. A sub-band decimates the
// wideband stream ONCE (shared front-end) to a moderate IF; its decoders then
// run cheap per-channel DDCs from that IF. Decoders far apart in frequency get
// their own sub-band. Sub-bands are spread across a worker-thread pool.
#pragma once

#include "decode/decoder.h"
#include "decode/message_log.h"
#include "dsp/ddc.h"
#include "audio/audio_output.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class DecoderManager
{
public:
    struct Status
    {
        int channelId;
        double freqMHz;
        int baud;
        bool locked;
        double ebno;
        uint64_t msgs;
        int egcBer;     // -1 unless EGC
        int egcFrames;  // 0 unless EGC
    };

    ~DecoderManager() { stop(); }

    void configure(double Fs, double centerHz);
    void start();
    void stop();

    void feed(const float* iq, int nComplex);

    int  addDecoder(double freqHz, int baud);
    void removeDecoder(int channelId);
    void setDecoderFreq(int channelId, double freqHz);
    void removeAll();
    int  decoderCount();
    int  workerCount() const { return (int)workers_.size(); }
    int  subbandCount();

    std::vector<Status> status();
    int getConstellation(int channelId, std::vector<float>& out, int maxPairs);
    uint64_t drops() const { return drops_.load(); }
    MessageLog& log() { return log_; }
    MessageLog& suLog() { return suLog_; }
    CassignLog& cassignLog() { return cassign_; }
    ChannelTable& channelTable() { return netTable_; }
    EgcLog& egcLog() { return egcLog_; }

    // Voice: route one 8400 decoder's audio to the speakers.
    void setVoiceMonitor(int channelId);
    int  voiceMonitor() const { return voiceMonitorId_; }
    float audioLevel() { return audio_.level(); }

    // Voice call recording: every 8400 decoder writes its calls to WAV files
    // (one per call) in dir, independent of which channel is being monitored.
    void setRecording(bool on, const std::string& dir);
    bool recording() const { return recordOn_; }
    const std::string& recordDir() const { return recordDir_; }
    int  recordingCount(); // decoders with a call file currently open

private:
    struct SubBand
    {
        SubBand(double Fs, double wideCenterHz, double center, double rateTarget,
                double bw)
            : centerHz(center), frontEnd(Fs, center - wideCenterHz, rateTarget, bw)
        {
            subRate = frontEnd.outputRate();
        }
        double centerHz;
        double subRate = 0.0;
        Ddc frontEnd;                 // Fs -> subRate (shared by all decoders)
        std::vector<double> subIQ;    // scratch (worker thread only)
        std::vector<std::unique_ptr<Decoder>> decoders;
    };

    struct Worker
    {
        std::thread thread;
        std::mutex qMtx;
        std::condition_variable cv;
        std::deque<std::vector<float>> queue;

        std::mutex dMtx; // guards subbands
        std::vector<std::unique_ptr<SubBand>> subbands;
        std::atomic<int> count{0}; // total decoders on this worker
    };

    void workerLoop(Worker* w);

    double Fs_ = 0.0;
    double centerHz_ = 0.0;

    std::vector<std::unique_ptr<Worker>> workers_;
    std::atomic<bool> run_{false};

    std::mutex idMtx_;
    int nextId_ = 1;

    std::atomic<uint64_t> drops_{0};
    static constexpr size_t kMaxQueue = 64;
    MessageLog log_;
    MessageLog suLog_;
    CassignLog cassign_;
    ChannelTable netTable_;
    EgcLog egcLog_;
    AudioOutput audio_;
    int voiceMonitorId_ = -1;
    bool recordOn_ = false;
    std::string recordDir_ = "recordings";
};
