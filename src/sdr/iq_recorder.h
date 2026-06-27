// IQ recorder: writes live IQ samples to a WAV file in 8-bit stereo format.
// Includes an optional pre-buffer ring that captures the last N seconds of IQ
// so recording always gets the moments just before the user hit Record.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

class IqRecorder
{
public:
    IqRecorder() = default;
    ~IqRecorder() { stop(); }

    IqRecorder(const IqRecorder&) = delete;
    IqRecorder& operator=(const IqRecorder&) = delete;

    // Size the pre-buffer ring for bufferSec at sampleRate Hz.  Call whenever
    // the sample rate changes or the user tweaks the buffer duration.  If
    // bufferSec <= 0 or sampleRate > 3e6 the ring is freed entirely.
    void configurePrebuffer(double sampleRate, double bufferSec);

    // Feed the pre-buffer ring (call from the SDR callback on every IQ block
    // regardless of whether recording is active).  No-op when the ring is
    // disabled (sample rate > 3 Msps).
    void prebuffer(const float* iq, int nComplex);

    // Start recording.  sampleRate is the IQ sample rate (Hz).  If the
    // pre-buffer ring holds data it is drained into the file first.
    bool start(const std::string& path, double sampleRate);

    // Append nComplex interleaved I,Q float samples (2n floats).
    void write(const float* iq, int nComplex);

    // Finalize header and close.
    void stop();

    bool isRecording() const { return recording_.load(); }
    double elapsed() const; // seconds since start
    const std::string& path() const { return path_; }

    // Exposed for the UI memory-duty estimate.
    double prebufferSec() const { return bufferSec_; }
    size_t prebufferBytes() const { return ring_.capacity() * sizeof(float); }

private:
    std::atomic<bool> recording_{false};
    std::string path_;
    std::FILE* f_ = nullptr;
    double sampleRate_ = 0.0;
    double startTime_ = 0.0;
    uint32_t dataBytes_ = 0;

    // Pre-buffer ring (float interleaved I,Q).
    std::mutex ringMtx_;
    std::vector<float> ring_;
    size_t ringCap_ = 0;       // complex sample capacity
    size_t ringWrite_ = 0;     // total complex samples ever pushed
    double bufferSec_ = 0.0;
};
