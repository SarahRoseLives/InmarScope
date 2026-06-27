// Native Airspy source backend using libairspy directly.
#pragma once

#include "sdr/sdr_source.h"

#include <atomic>
#include <cstdint>
#include <vector>

struct airspy_device;

class AirspySource : public SdrSource
{
public:
    AirspySource();
    ~AirspySource() override;

    std::vector<SdrDeviceInfo> listDevices() override;

    void setCenterFreq(double hz) override;
    void setSampleRate(double hz) override;
    void setGain(double db) override;
    void setBiasTee(bool on) override;
    void setPpm(double ppm) override { ppm_ = ppm; }

    void setDcBlock(bool on) { dcBlock_.store(on); }
    bool dcBlock() const { return dcBlock_.load(); }

    // Airspy-specific gain controls.
    // Gain mode: 0=Sensitivity, 1=Linear, 2=Free.
    void setGainMode(int mode);
    int  gainMode() const { return gainMode_; }

    // Sensitivity / Linear (single knob, 0-21).
    void setSenseGain(int val);
    void setLinearGain(int val);

    // Free mode (individual stages, 0-15 each).
    void setLnaGain(int val);
    void setMixerGain(int val);
    void setVgaGain(int val);
    void setLnaAgc(bool on);
    void setMixerAgc(bool on);

    double centerFreq() const override { return centerFreq_; }
    double sampleRate() const override { return sampleRate_; }

    bool start(int deviceIndex, SdrSampleCb cb, std::string& err) override;
    void stop() override;
    bool running() const override { return running_.load(); }

    // Called by the libairspy RX trampoline.
    int handleRx(float* buf, int nComplex);

private:
    void applyGain();

    airspy_device* dev_ = nullptr;
    uint64_t       selectedSerial_ = 0;
    std::atomic<bool> running_{false};
    SdrSampleCb    cb_;

    double centerFreq_ = 1545.0e6;
    double sampleRate_ = 10.0e6;
    double ppm_ = 0.0;

    int  gainMode_ = 0;       // 0=Sensitivity, 1=Linear, 2=Free
    int  senseGain_ = 10;     // 0-21
    int  linearGain_ = 10;    // 0-21
    int  lnaGain_ = 8;        // 0-15
    int  mixerGain_ = 8;      // 0-15
    int  vgaGain_ = 4;        // 0-15
    bool lnaAgc_ = false;
    bool mixerAgc_ = false;
    bool biasTee_ = false;

    std::atomic<bool> dcBlock_{true};
    float dcOffRe_ = 0.0f, dcOffIm_ = 0.0f;
    float dcRate_ = 2.0e-6f;

    std::vector<float> scratch_;
};
