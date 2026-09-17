// Native ADALM-Pluto / Pluto+ source backend using libiio.
#pragma once

#include "sdr/sdr_source.h"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

struct iio_buffer;
struct iio_channel;
struct iio_context;
struct iio_device;

class PlutoSource : public SdrSource
{
public:
    PlutoSource() = default;
    ~PlutoSource() override;

    std::vector<SdrDeviceInfo> listDevices() override;

    void setUri(const std::string& uri) { uri_ = uri; }
    const std::string& uri() const { return uri_; }

    void setCenterFreq(double hz) override;
    void setSampleRate(double hz) override;
    void setGain(double db) override; // <0 selects slow-attack AGC
    void setBiasTee(bool) override {} // Pluto hardware has no controllable bias tee
    void setPpm(double ppm) override;

    void setBandwidth(double hz);
    void setRfPort(const std::string& port);
    void setDcBlock(bool on) { dcBlock_.store(on); }

    double centerFreq() const override { return centerFreq_; }
    double sampleRate() const override { return sampleRate_; }

    bool start(int deviceIndex, SdrSampleCb cb, std::string& err) override;
    void stop() override;
    bool running() const override { return running_.load(); }

private:
    bool openContext(std::string& err);
    bool configure(std::string& err);
    void cleanup();
    void readLoop();
    void applyTune();
    void applyGain();

    std::string uri_ = "ip:192.168.2.1";
    std::string rfPort_ = "A_BALANCED";
    double centerFreq_ = 1545.0e6;
    double sampleRate_ = 2.4e6;
    double bandwidth_ = 2.0e6;
    double gainDb_ = 40.0;
    double ppm_ = 0.0;
    bool agc_ = false;

    iio_context* ctx_ = nullptr;
    iio_device* phy_ = nullptr;
    iio_device* rxDev_ = nullptr;
    iio_channel* rxCfg_ = nullptr;
    iio_channel* rxLo_ = nullptr;
    iio_channel* rxI_ = nullptr;
    iio_channel* rxQ_ = nullptr;
    iio_buffer* rxBuf_ = nullptr;

    std::atomic<bool> running_{false};
    std::atomic<bool> dcBlock_{true};
    std::thread readThread_;
    SdrSampleCb cb_;
    std::vector<float> scratch_;
    float dcOffRe_ = 0.0f;
    float dcOffIm_ = 0.0f;
    float dcRate_ = 2.0e-6f;
};
