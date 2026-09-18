// Abstract SDR source interface. Phase 1 implements RtlSdrSource.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct SdrDeviceInfo
{
    int index = 0;
    std::string name;
    std::string serial;
    bool hasTunerModes = false; // SDRplay RSPduo enumeration exposes a mode key.
};

// Called from the SDR read thread with interleaved float IQ (I,Q pairs),
// already converted to [-1,1] and DC-blocked.
using SdrSampleCb = std::function<void(const float* iq, int nComplex)>;

class SdrSource
{
public:
    virtual ~SdrSource() = default;

    virtual std::vector<SdrDeviceInfo> listDevices() = 0;

    // Configuration (applied on start; some apply live while running).
    virtual void setCenterFreq(double hz) = 0;
    virtual void setSampleRate(double hz) = 0;
    virtual void setGain(double db) = 0;      // <0 => auto/AGC
    virtual void setBiasTee(bool on) = 0;
    virtual void setPpm(double ppm) = 0;

    virtual double centerFreq() const = 0;
    virtual double sampleRate() const = 0;

    // Lifecycle.
    virtual bool start(int deviceIndex, SdrSampleCb cb, std::string& err) = 0;
    virtual void stop() = 0;
    virtual bool running() const = 0;
};
