// Native HackRF source backend using libhackrf directly.
#pragma once

#include "sdr/sdr_source.h"

#include <atomic>
#include <cstdint>
#include <vector>

struct hackrf_device; // from <libhackrf/hackrf.h>

class HackRfSource : public SdrSource
{
public:
    HackRfSource() = default;
    ~HackRfSource() override;

    std::vector<SdrDeviceInfo> listDevices() override;

    void setCenterFreq(double hz) override;
    void setSampleRate(double hz) override;
    void setGain(double db) override; // maps to VGA (baseband) gain
    void setBiasTee(bool on) override; // HackRF antenna/port power
    void setPpm(double ppm) override { ppm_ = ppm; }

    void setDcBlock(bool on) { dcBlock_.store(on); }
    bool dcBlock() const { return dcBlock_.load(); }

    // HackRF-specific gain stages.
    void setLnaGain(int db);   // IF gain, 0..40 in 8 dB steps
    void setVgaGain(int db);   // baseband gain, 0..62 in 2 dB steps
    void setAmpEnable(bool on); // ~11 dB RF amp near the antenna

    double centerFreq() const override { return centerFreq_; }
    double sampleRate() const override { return sampleRate_; }

    bool start(int deviceIndex, SdrSampleCb cb, std::string& err) override;
    void stop() override;
    bool running() const override { return running_.load(); }

    // Called by the libhackrf RX trampoline (defined in the .cpp). Public so the
    // C-style callback can reach it; not part of the SdrSource interface.
    int handleRx(const int8_t* buf, int len);

private:
    hackrf_device* dev_ = nullptr;
    std::atomic<bool> running_{false};
    SdrSampleCb cb_;

    double centerFreq_ = 1545.0e6;
    double sampleRate_ = 10.0e6;
    double ppm_ = 0.0;
    int    lnaGain_ = 16;
    int    vgaGain_ = 16;
    bool   amp_ = false;
    bool   biasTee_ = false;

    std::atomic<bool> dcBlock_{true};
    float dcOffRe_ = 0.0f, dcOffIm_ = 0.0f;
    float dcRate_ = 2.0e-5f;

    std::vector<float> scratch_;
};
