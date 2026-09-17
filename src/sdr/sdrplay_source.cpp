#include "sdr/sdrplay_source.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <sstream>
#include <iomanip>
#include <cstdlib>
#if defined(_WIN32) && defined(SOAPY_ROOT_HINT)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <filesystem>
#endif
#ifdef HAS_SOAPYSDR
#include <SoapySDR/Device.h>
#endif

SdrplaySource::~SdrplaySource() { close(); }
std::string serializeRspConfig(const RspConfig& c) {
    std::ostringstream out;
    out << std::setprecision(17) << std::quoted(c.serial) << ' ' << std::quoted(c.mode) << ' '
        << std::quoted(c.antenna) << ' ' << c.rate << ' ' << c.bandwidth << ' ' << c.ppm << ' '
        << c.agc << ' ' << c.dc << ' ' << c.iq << ' ' << c.gains.size();
    for (const auto& g : c.gains) out << ' ' << std::quoted(g.first) << ' ' << g.second;
    out << ' ' << c.settings.size();
    for (const auto& s : c.settings) out << ' ' << std::quoted(s.first) << ' ' << std::quoted(s.second);
    return out.str();
}
bool parseRspConfig(const std::string& text, RspConfig& result) {
    RspConfig c; std::istringstream in(text); size_t count = 0;
    if (!(in >> std::quoted(c.serial) >> std::quoted(c.mode) >> std::quoted(c.antenna)
          >> c.rate >> c.bandwidth >> c.ppm >> c.agc >> c.dc >> c.iq >> count) || count > 128) return false;
    if (!std::isfinite(c.rate) || c.rate <= 0 || !std::isfinite(c.bandwidth) || c.bandwidth < 0 ||
        !std::isfinite(c.ppm) || std::abs(c.ppm) > 200) return false;
    if (c.mode != "ST" && c.mode != "MA" && c.mode != "MA8" && c.mode != "SL") return false;
    for (size_t i = 0; i < count; ++i) {
        std::string key; double value;
        if (!(in >> std::quoted(key) >> value) || !std::isfinite(value)) return false;
        c.gains[key] = value;
    }
    if (!(in >> count) || count > 128) return false;
    for (size_t i = 0; i < count; ++i) {
        std::string key, value;
        if (!(in >> std::quoted(key) >> std::quoted(value))) return false;
        c.settings[key] = value;
    }
    in >> std::ws; if (!in.eof()) return false;
    result = std::move(c); return true;
}
void SdrplaySource::fail(const std::string& message) {
    std::lock_guard<std::mutex> lock(errorMutex_); error_ = message;
}
std::string SdrplaySource::error() const {
    std::lock_guard<std::mutex> lock(errorMutex_); return error_;
}
#ifdef HAS_SOAPYSDR
namespace {
void initializeRuntime() {
#if defined(_WIN32) && defined(SOAPY_ROOT_HINT)
    static std::once_flag once;
    std::call_once(once, [] {
        // A copied Soapy DLL otherwise searches relative to the application.
        // Keep modules in their matching SDK installation, and honor overrides.
        const char* root = std::getenv("SOAPY_SDR_ROOT");
        std::string defaultRoot = SOAPY_ROOT_HINT;
        wchar_t executable[32768]{};
        if (GetModuleFileNameW(nullptr, executable, 32768)) {
            auto bundled = std::filesystem::path(executable).parent_path() / "soapy";
            if (std::filesystem::is_directory(bundled)) defaultRoot = bundled.u8string();
        }
        if (!root || !*root) { _putenv_s("SOAPY_SDR_ROOT", defaultRoot.c_str()); root = std::getenv("SOAPY_SDR_ROOT"); }
        auto api = std::filesystem::path(root) / "bin" / "sdrplay_api.dll";
        // Keep the vendor API loaded for the lifetime of the plugin registry.
        LoadLibraryExW(api.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    });
#endif
}
std::string str(const char* p) { return p ? p : ""; }
std::vector<std::string> strings(char** p, size_t n) {
    std::vector<std::string> result;
    for (size_t i = 0; i < n; ++i) result.push_back(str(p[i]));
    SoapySDRStrings_clear(&p, n); return result;
}
std::vector<double> numbers(double* p, size_t n) {
    std::vector<double> result;
    if (p) result.assign(p, p + n);
    SoapySDR_free(p); return result;
}
}
bool SdrplaySource::check(int status, const char* operation) {
    if (!status) return true;
    fail(std::string(operation) + ": " + SoapySDRDevice_lastError()); return false;
}
std::vector<SdrDeviceInfo> SdrplaySource::listDevices() {
    initializeRuntime();
    size_t count = 0;
    auto* devices = SoapySDRDevice_enumerateStrArgs("driver=sdrplay", &count);
    std::vector<SdrDeviceInfo> result;
    for (size_t i = 0; i < count; ++i) {
        auto serial = str(SoapySDRKwargs_get(&devices[i], "serial"));
        if (std::none_of(result.begin(), result.end(), [&](const SdrDeviceInfo& d) { return d.serial == serial; }))
            result.push_back({int(result.size()), str(SoapySDRKwargs_get(&devices[i], "label")), serial});
    }
    SoapySDRKwargsList_clear(devices, count);
    if (result.empty()) fail("No SDRplay devices found. Install a current SoapySDRPlay3 driver and SDRplay API 3 service, start the service, and close other receiver applications.");
    else fail("");
    return result;
}
void SdrplaySource::discover() {
    size_t n = 0;
    auto** a = SoapySDRDevice_listAntennas(device_, SOAPY_SDR_RX, 0, &n);
    antennas_ = strings(a, n);
    auto* r = SoapySDRDevice_listSampleRates(device_, SOAPY_SDR_RX, 0, &n);
    rates_ = numbers(r, n);
    auto* b = SoapySDRDevice_listBandwidths(device_, SOAPY_SDR_RX, 0, &n);
    bandwidths_ = numbers(b, n);
    hasAgc = SoapySDRDevice_hasGainMode(device_, SOAPY_SDR_RX, 0);
    hasDc = SoapySDRDevice_hasDCOffsetMode(device_, SOAPY_SDR_RX, 0);
    hasIq = SoapySDRDevice_hasIQBalanceMode(device_, SOAPY_SDR_RX, 0);
    hasPpm = SoapySDRDevice_hasFrequencyCorrection(device_, SOAPY_SDR_RX, 0);
    auto** g = SoapySDRDevice_listGains(device_, SOAPY_SDR_RX, 0, &n);
    gains_.clear();
    for (const auto& name : strings(g, n)) {
        auto range = SoapySDRDevice_getGainElementRange(device_, SOAPY_SDR_RX, 0, name.c_str());
        RspControl c; c.key = c.name = name; c.type = 2;
        c.minimum = range.minimum; c.maximum = range.maximum; c.step = range.step;
        c.value = std::to_string(SoapySDRDevice_getGainElement(device_, SOAPY_SDR_RX, 0, name.c_str()));
        gains_.push_back(c);
    }
    controls_.clear();
    for (bool channel : {false, true}) {
        auto* info = channel ? SoapySDRDevice_getChannelSettingInfo(device_, SOAPY_SDR_RX, 0, &n)
                             : SoapySDRDevice_getSettingInfo(device_, &n);
        for (size_t i = 0; i < n; ++i) {
            const auto& x = info[i]; RspControl c;
            c.key = str(x.key); c.name = str(x.name); c.description = str(x.description);
            c.type = int(x.type); c.minimum = x.range.minimum; c.maximum = x.range.maximum;
            c.step = x.range.step; c.channel = channel;
            char* value = channel ? SoapySDRDevice_readChannelSetting(device_, SOAPY_SDR_RX, 0, x.key)
                                  : SoapySDRDevice_readSetting(device_, x.key);
            c.value = value ? value : str(x.value); SoapySDR_free(value);
            for (size_t j = 0; j < x.numOptions; ++j) {
                c.options.push_back(str(x.options[j]));
                c.labels.push_back(x.optionNames ? str(x.optionNames[j]) : c.options.back());
            }
            controls_.push_back(c);
        }
        SoapySDRArgInfoList_clear(info, n);
    }
}
bool SdrplaySource::prepare(const RspConfig& config, std::string& err) {
    initializeRuntime();
    if (running_) { err = "Stop reception before selecting a device."; return false; }
    close(); fail("");
    if (config.serial.empty()) { err = "Select an SDRplay device first."; return false; }
    SoapySDRKwargs args{};
    SoapySDRKwargs_set(&args, "driver", "sdrplay");
    SoapySDRKwargs_set(&args, "serial", config.serial.c_str());
    SoapySDRKwargs_set(&args, "mode", config.mode.c_str());
    // The constructor's antenna argument selects an RSPduo tuner; other
    // models' input names must go through setAntenna after opening.
    if (config.mode != "SL" && config.antenna.rfind("Tuner ", 0) == 0)
        SoapySDRKwargs_set(&args, "antenna", config.antenna.c_str());
    device_ = SoapySDRDevice_make(&args);
    SoapySDRKwargs_clear(&args);
    if (!device_) { fail(std::string("Open SDRplay: ") + SoapySDRDevice_lastError()); err = error(); return false; }
    char* hardware = SoapySDRDevice_getHardwareKey(device_);
    const bool duo = str(hardware) == "RSPduo";
    SoapySDR_free(hardware);
    if (config.mode != "ST" && !duo) {
        err = "Master/slave modes require an RSPduo. Select single tuner mode or two distinct devices.";
        fail(err); close(); return false;
    }
    config_ = config;
    if (!check(SoapySDRDevice_setFrequency(device_, SOAPY_SDR_RX, 0, frequency_, nullptr), "Initial frequency")) {
        err = error(); close(); return false;
    }
    frequency_ = SoapySDRDevice_getFrequency(device_, SOAPY_SDR_RX, 0);
    discover();
    return true;
}
bool SdrplaySource::apply(const RspConfig& config, std::string& err) {
    if (!device_) { err = "Load the SDRplay device controls first."; return false; }
    if (running_) { err = "Stop reception before applying device settings."; return false; }
    fail("");
    auto require = [&](int status, const char* operation) {
        if (!check(status, operation)) throw std::runtime_error(error());
    };
    try {
        if (!std::isfinite(config.rate) || config.rate <= 0 || !std::isfinite(config.ppm))
            throw std::runtime_error("Invalid sample rate or PPM correction.");
        if (!config.antenna.empty()) {
            if (std::find(antennas_.begin(), antennas_.end(), config.antenna) == antennas_.end())
                throw std::runtime_error("The selected antenna is unavailable on this device/tuner.");
            require(SoapySDRDevice_setAntenna(device_, SOAPY_SDR_RX, 0, config.antenna.c_str()), "Antenna");
        }
        require(SoapySDRDevice_setSampleRate(device_, SOAPY_SDR_RX, 0, config.rate), "Sample rate");
        rate_ = SoapySDRDevice_getSampleRate(device_, SOAPY_SDR_RX, 0);
        if (!std::isfinite(rate_) || rate_ <= 0) throw std::runtime_error("Driver returned an invalid sample rate.");
        require(SoapySDRDevice_setBandwidth(device_, SOAPY_SDR_RX, 0, config.bandwidth), "Bandwidth");
        if (hasPpm && config.mode != "SL") require(SoapySDRDevice_setFrequencyCorrection(device_, SOAPY_SDR_RX, 0, config.ppm), "PPM");
        if (hasDc) require(SoapySDRDevice_setDCOffsetMode(device_, SOAPY_SDR_RX, 0, config.dc), "DC correction");
        if (hasIq) require(SoapySDRDevice_setIQBalanceMode(device_, SOAPY_SDR_RX, 0, config.iq), "IQ correction");
        if (hasAgc) require(SoapySDRDevice_setGainMode(device_, SOAPY_SDR_RX, 0, config.agc), "AGC");
        // Set frequency before gain: valid LNA states depend on the RF band.
        require(SoapySDRDevice_setFrequency(device_, SOAPY_SDR_RX, 0, frequency_, nullptr), "Frequency");
        frequency_ = SoapySDRDevice_getFrequency(device_, SOAPY_SDR_RX, 0);
        for (const auto& gain : config.gains) {
            if (config.agc && gain.first == "IFGR") continue;
            auto range = SoapySDRDevice_getGainElementRange(device_, SOAPY_SDR_RX, 0, gain.first.c_str());
            if (!std::isfinite(gain.second) || gain.second < range.minimum || gain.second > range.maximum)
                throw std::runtime_error("Gain outside current band range: " + gain.first);
            require(SoapySDRDevice_setGainElement(device_, SOAPY_SDR_RX, 0, gain.first.c_str(), gain.second), "Gain");
        }
        for (const auto& c : controls_) {
            auto it = config.settings.find((c.channel ? "channel:" : "device:") + c.key);
            if (it == config.settings.end()) continue;
            const auto& value = it->second;
            if (!c.options.empty() && std::find(c.options.begin(), c.options.end(), value) == c.options.end())
                throw std::runtime_error("Invalid option for " + c.name);
            if (c.type == 0 && value != "true" && value != "false") throw std::runtime_error("Invalid boolean for " + c.name);
            if (c.type == 1 || c.type == 2) {
                size_t used = 0; double v = std::stod(value, &used);
                if (used != value.size() || !std::isfinite(v) || (c.type == 1 && std::floor(v) != v) ||
                    (c.maximum > c.minimum && (v < c.minimum || v > c.maximum)))
                    throw std::runtime_error("Invalid value for " + c.name);
            }
            require(c.channel ? SoapySDRDevice_writeChannelSetting(device_, SOAPY_SDR_RX, 0, c.key.c_str(), value.c_str())
                              : SoapySDRDevice_writeSetting(device_, c.key.c_str(), value.c_str()), c.name.c_str());
        }
        config_ = config;
        return true;
    } catch (const std::exception& ex) { fail(ex.what()); err = error(); return false; }
}
void SdrplaySource::setCenterFreq(double hz) {
    if (!std::isfinite(hz) || hz < 1000 || hz > 2e9) { fail("SDRplay frequency must be between 1 kHz and 2 GHz."); return; }
    if (!device_) { frequency_ = hz; return; }
    if (check(SoapySDRDevice_setFrequency(device_, SOAPY_SDR_RX, 0, hz, nullptr), "Tune"))
        frequency_ = SoapySDRDevice_getFrequency(device_, SOAPY_SDR_RX, 0);
}
void SdrplaySource::setSampleRate(double hz) { if (!running_) config_.rate = hz; }
void SdrplaySource::setGain(double db) {
    if (!device_) return;
    if (hasAgc) check(SoapySDRDevice_setGainMode(device_, SOAPY_SDR_RX, 0, db < 0), "AGC");
    if (db >= 0) check(SoapySDRDevice_setGain(device_, SOAPY_SDR_RX, 0, db), "Gain");
}
void SdrplaySource::setBiasTee(bool on) {
    if (device_ && std::any_of(controls_.begin(), controls_.end(), [](const RspControl& c) { return c.key == "biasT_ctrl"; }))
        check(SoapySDRDevice_writeSetting(device_, "biasT_ctrl", on ? "true" : "false"), "Bias tee");
}
void SdrplaySource::setPpm(double ppm) {
    if (device_ && hasPpm && config_.mode != "SL") check(SoapySDRDevice_setFrequencyCorrection(device_, SOAPY_SDR_RX, 0, ppm), "PPM");
}
bool SdrplaySource::start(int, SdrSampleCb cb, std::string& err) {
    stop();
    if (!device_) { err = "Load the SDRplay device first."; return false; }
    callback_ = std::move(cb); overflows_ = 0; fail("");
    size_t channel = 0;
    stream_ = SoapySDRDevice_setupStream(device_, SOAPY_SDR_RX, "CF32", &channel, 1, nullptr);
    if (!stream_) { fail(std::string("Setup stream: ") + SoapySDRDevice_lastError()); err = error(); return false; }
    if (!check(SoapySDRDevice_activateStream(device_, stream_, 0, 0, 0), "Start stream")) {
        err = error(); stop(); return false;
    }
    running_ = true;
    try { reader_ = std::thread(&SdrplaySource::readLoop, this); }
    catch (const std::exception& ex) { err = ex.what(); stop(); return false; }
    return true;
}
void SdrplaySource::readLoop() {
    try {
        std::vector<float> buffer(32768 * 2); void* buffers[] = {buffer.data()};
        while (running_) {
            int flags = 0; long long time = 0;
            int n = SoapySDRDevice_readStream(device_, stream_, buffers, buffer.size() / 2, &flags, &time, 100000);
            if (n == SOAPY_SDR_TIMEOUT) continue;
            if (n == SOAPY_SDR_OVERFLOW) { ++overflows_; continue; }
            if (n < 0) { fail(std::string("SDRplay stream stopped: ") + SoapySDR_errToStr(n)); break; }
            if (n > 0 && callback_) callback_(buffer.data(), n);
        }
    } catch (const std::exception& ex) { fail(ex.what()); }
    running_ = false;
}
void SdrplaySource::stop() {
    running_ = false;
    if (reader_.joinable()) reader_.join();
    if (stream_) {
        SoapySDRDevice_deactivateStream(device_, stream_, 0, 0);
        SoapySDRDevice_closeStream(device_, stream_); stream_ = nullptr;
    }
    callback_ = {};
}
void SdrplaySource::close() {
    stop();
    if (device_) { SoapySDRDevice_unmake(device_); device_ = nullptr; }
    controls_.clear(); gains_.clear(); antennas_.clear(); rates_.clear(); bandwidths_.clear();
}
#else
std::vector<SdrDeviceInfo> SdrplaySource::listDevices() { fail("This build has no SoapySDR support. See COMPILE.md."); return {}; }
bool SdrplaySource::prepare(const RspConfig&, std::string& err) { listDevices(); err = error(); return false; }
bool SdrplaySource::apply(const RspConfig&, std::string& err) { return prepare({}, err); }
bool SdrplaySource::start(int, SdrSampleCb, std::string& err) { return prepare({}, err); }
void SdrplaySource::stop() {}
void SdrplaySource::close() {}
void SdrplaySource::setCenterFreq(double hz) { frequency_ = hz; }
void SdrplaySource::setSampleRate(double hz) { rate_ = hz; }
void SdrplaySource::setGain(double) {}
void SdrplaySource::setBiasTee(bool) {}
void SdrplaySource::setPpm(double) {}
#endif
