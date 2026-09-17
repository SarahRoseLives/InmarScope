#pragma once
#include "sdr/sdr_source.h"
#include <atomic>
#include <map>
#include <mutex>
#include <thread>

struct SoapySDRDevice;
struct SoapySDRStream;

// Capabilities come from the installed SoapySDRPlay3 driver, not a model guess.
struct RspControl {
    std::string key, name, description, value;
    int type = 3; // bool, int, float, string
    double minimum = 0, maximum = 0, step = 0;
    std::vector<std::string> options, labels;
    bool channel = false;
};
struct RspConfig {
    std::string serial, mode = "ST", antenna;
    double rate = 2e6, bandwidth = 0, ppm = 0;
    bool agc = true, dc = true, iq = true;
    std::map<std::string, double> gains;
    std::map<std::string, std::string> settings;
};
std::string serializeRspConfig(const RspConfig& config);
bool parseRspConfig(const std::string& text, RspConfig& config);

class SdrplaySource : public SdrSource {
public:
    ~SdrplaySource() override;
    std::vector<SdrDeviceInfo> listDevices() override;
    bool prepare(const RspConfig& config, std::string& error);
    void close();
    bool apply(const RspConfig& config, std::string& error);
    bool prepared() const { return device_ != nullptr; }
    bool streamFailed() const { return stream_ != nullptr && !running_; }
    const std::vector<RspControl>& controls() const { return controls_; }
    const std::vector<RspControl>& gains() const { return gains_; }
    const std::vector<std::string>& antennas() const { return antennas_; }
    const std::vector<double>& rates() const { return rates_; }
    const std::vector<double>& bandwidths() const { return bandwidths_; }
    bool hasAgc = false, hasDc = false, hasIq = false, hasPpm = false;
    std::string error() const;
    uint64_t overflows() const { return overflows_; }
    void setCenterFreq(double hz) override;
    void setSampleRate(double hz) override;
    void setGain(double db) override;
    bool setGainElement(const std::string& key, double value);
    void setBiasTee(bool on) override;
    void setPpm(double ppm) override;
    double centerFreq() const override { return frequency_; }
    double sampleRate() const override { return rate_; }
    bool start(int deviceIndex, SdrSampleCb cb, std::string& error) override;
    void stop() override;
    bool running() const override { return running_; }
private:
    void readLoop();
    void discover();
    bool check(int status, const char* operation);
    void fail(const std::string& message);
    SoapySDRDevice* device_ = nullptr;
    SoapySDRStream* stream_ = nullptr;
    RspConfig config_;
    std::vector<RspControl> controls_, gains_;
    std::vector<std::string> antennas_;
    std::vector<double> rates_, bandwidths_;
    std::atomic<double> frequency_{1545e6}, rate_{2e6};
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> overflows_{0};
    std::thread reader_;
    SdrSampleCb callback_;
    mutable std::mutex errorMutex_;
    std::string error_;
};
