// SDR++ Server source: connect to an `sdrpp_server` instance over TCP, remotely
// control the SDR, and stream baseband IQ (optionally zstd-compressed).
// Implements the SDR++ server protocol (see reference/SDRPlusPlus).
#pragma once

#include "sdr/sdr_source.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class SdrppServerSource : public SdrSource
{
public:
    SdrppServerSource() = default;
    ~SdrppServerSource() override;

    std::vector<SdrDeviceInfo> listDevices() override { return {}; }

    void setCenterFreq(double hz) override;
    void setSampleRate(double hz) override; // request a rate from the server
    void setGain(double db) override {}       // set on the server side
    void setBiasTee(bool on) override {}
    void setPpm(double ppm) override {}

    double centerFreq() const override { return centerFreq_; }
    double sampleRate() const override { return sampleRate_.load(); }

    bool start(int deviceIndex, SdrSampleCb cb, std::string& err) override;
    void stop() override;
    bool running() const override { return running_.load(); }

    // Server-specific configuration (set before start()).
    void setHost(const std::string& h) { host_ = h; }
    void setPort(uint16_t p) { port_ = p; }
    void setCompressionEnabled(bool on) { compression_ = on; }
    void setSampleTypeIndex(int t) { sampleType_ = t; } // 0=int8, 1=int16, 2=float
    std::string host() const { return host_; }
    uint16_t port() const { return port_; }
    bool compressionEnabled() const { return compression_; }
    int sampleTypeIndex() const { return sampleType_; }

    // Sample-rate selection discovered from the server's source-module UI
    // (e.g. the HackRF panel). Populated after connecting; selecting one sends
    // a UI action to the server to change the device rate.
    std::vector<std::string> sampleRateLabels();
    std::vector<double> sampleRateValues();
    int currentSampleRateIndex();
    std::string uiDebug(); // human-readable dump of the parsed server UI

private:
    void worker();
    bool sendCommand(uint32_t cmd, const void* args, uint32_t argLen);
    bool recvAll(void* buf, int len);
    void handleBaseband(const uint8_t* data, int len);
    void parseUi(const uint8_t* data, int len);
    void sendComboAction(const std::string& id, int index, bool sync);

    uintptr_t sock_ = ~(uintptr_t)0; // SOCKET / fd
    std::thread thread_;
    std::atomic<bool> running_{false};
    SdrSampleCb cb_;

    std::string host_ = "localhost";
    uint16_t port_ = 5259;
    bool compression_ = true;
    int sampleType_ = 1; // int16

    double centerFreq_ = 1545.0e6;
    std::atomic<double> sampleRate_{2.0e6};
    double reqSampleRate_ = 0.0; // desired rate to request (0 = leave to server)

    // Sample-rate combo parsed from the server UI (guarded by uiMtx_).
    std::mutex uiMtx_;
    std::vector<double> srValues_;
    std::vector<std::string> srLabels_;
    std::string srComboId_;
    bool srForceSync_ = false;
    int srCurrentIdx_ = -1;
    std::string uiDebug_; // summary of combos found in the last UI parse

    void* dctx_ = nullptr; // ZSTD_DCtx
    std::vector<uint8_t> rbuf_;
    std::vector<uint8_t> dbuf_;
    std::vector<float> fbuf_;
};
