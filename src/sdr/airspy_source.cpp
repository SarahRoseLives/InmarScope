#include "sdr/airspy_source.h"

#include <airspy.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <string>

namespace {
void ensureAirspyInit()
{
    static std::once_flag once;
    std::call_once(once, []() { airspy_init(); });
}

int rxTrampoline(airspy_transfer_t* transfer)
{
    auto* self = static_cast<AirspySource*>(transfer->ctx);
    int n = transfer->sample_count;
    // Airspy outputs float interleaved IQ when sample type is FLOAT32_IQ.
    return self->handleRx((float*)transfer->samples, n);
}
} // namespace

AirspySource::AirspySource()
{
    ensureAirspyInit();
}

AirspySource::~AirspySource()
{
    stop();
}

std::vector<SdrDeviceInfo> AirspySource::listDevices()
{
    ensureAirspyInit();
    std::vector<SdrDeviceInfo> out;
    uint64_t serials[256];
    int n = airspy_list_devices(serials, 256);
    for (int i = 0; i < n; ++i)
    {
        SdrDeviceInfo info;
        info.index = i;
        info.name = "Airspy";
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%016llX", (unsigned long long)serials[i]);
        info.serial = buf;
        out.push_back(info);
    }
    return out;
}

bool AirspySource::start(int deviceIndex, SdrSampleCb cb, std::string& err)
{
    if (running_.load())
        return true;
    ensureAirspyInit();

    uint64_t serials[256];
    int nDevs = airspy_list_devices(serials, 256);
    if (nDevs <= 0)
    {
        err = "No Airspy device found";
        return false;
    }
    if (deviceIndex < 0 || deviceIndex >= nDevs)
        deviceIndex = 0;
    selectedSerial_ = serials[deviceIndex];

    int r = airspy_open_sn(&dev_, selectedSerial_);
    if (r != AIRSPY_SUCCESS || !dev_)
    {
        err = "Airspy open failed: " + std::string(airspy_error_name((airspy_error)r));
        dev_ = nullptr;
        return false;
    }

    airspy_set_sample_type(dev_, AIRSPY_SAMPLE_FLOAT32_IQ);
    airspy_set_samplerate(dev_, (uint32_t)sampleRate_);
    airspy_set_freq(dev_, (uint32_t)centerFreq_);
    applyGain();
    airspy_set_rf_bias(dev_, biasTee_ ? 1 : 0);

    cb_ = std::move(cb);
    dcOffRe_ = dcOffIm_ = 0.0f;
    dcRate_ = (float)(50.0 / sampleRate_);

    r = airspy_start_rx(dev_, &rxTrampoline, this);
    if (r != AIRSPY_SUCCESS)
    {
        err = "Airspy start_rx failed: " + std::string(airspy_error_name((airspy_error)r));
        airspy_close(dev_);
        dev_ = nullptr;
        cb_ = nullptr;
        return false;
    }
    running_.store(true);
    return true;
}

void AirspySource::stop()
{
    if (dev_)
    {
        airspy_stop_rx(dev_);
        airspy_set_rf_bias(dev_, 0);
        airspy_close(dev_);
        dev_ = nullptr;
    }
    running_.store(false);
    cb_ = nullptr;
}

int AirspySource::handleRx(float* buf, int nComplex)
{
    if (!running_.load() || !cb_)
        return 0;

    if ((int)scratch_.size() < nComplex * 2)
        scratch_.resize((size_t)nComplex * 2);

    float* out = scratch_.data();
    if (dcBlock_.load())
    {
        float offRe = dcOffRe_, offIm = dcOffIm_;
        const float rate = dcRate_;
        for (int i = 0; i < nComplex; ++i)
        {
            float re = buf[i * 2];
            float im = buf[i * 2 + 1];
            float ore = re - offRe;
            offRe += ore * rate;
            float oim = im - offIm;
            offIm += oim * rate;
            out[i * 2] = ore;
            out[i * 2 + 1] = oim;
        }
        dcOffRe_ = offRe;
        dcOffIm_ = offIm;
    }
    else
    {
        for (int i = 0; i < nComplex * 2; ++i)
            out[i] = buf[i];
    }

    cb_(out, nComplex);
    return 0;
}

void AirspySource::setCenterFreq(double hz)
{
    centerFreq_ = hz;
    if (dev_)
        airspy_set_freq(dev_, (uint32_t)hz);
}

void AirspySource::setSampleRate(double hz)
{
    sampleRate_ = hz;
    if (dev_)
        airspy_set_samplerate(dev_, (uint32_t)hz);
}

void AirspySource::setGain(double db)
{
    // Single-knob fallback: maps to VGA gain in free mode.
    int v = (int)std::lround(db);
    if (v < 0) v = 0;
    if (v > 15) v = 15;
    setVgaGain(v);
}

void AirspySource::setBiasTee(bool on)
{
    biasTee_ = on;
    if (dev_)
        airspy_set_rf_bias(dev_, on ? 1 : 0);
}

void AirspySource::setGainMode(int mode)
{
    if (mode < 0) mode = 0;
    if (mode > 2) mode = 2;
    gainMode_ = mode;
    if (dev_) applyGain();
}

void AirspySource::setSenseGain(int val)
{
    senseGain_ = std::clamp(val, 0, 21);
    if (dev_ && gainMode_ == 0)
        airspy_set_sensitivity_gain(dev_, senseGain_);
}

void AirspySource::setLinearGain(int val)
{
    linearGain_ = std::clamp(val, 0, 21);
    if (dev_ && gainMode_ == 1)
        airspy_set_linearity_gain(dev_, linearGain_);
}

void AirspySource::setLnaGain(int val)
{
    lnaGain_ = std::clamp(val, 0, 15);
    if (dev_ && gainMode_ == 2 && !lnaAgc_)
        airspy_set_lna_gain(dev_, lnaGain_);
}

void AirspySource::setMixerGain(int val)
{
    mixerGain_ = std::clamp(val, 0, 15);
    if (dev_ && gainMode_ == 2 && !mixerAgc_)
        airspy_set_mixer_gain(dev_, mixerGain_);
}

void AirspySource::setVgaGain(int val)
{
    vgaGain_ = std::clamp(val, 0, 15);
    if (dev_ && gainMode_ == 2)
        airspy_set_vga_gain(dev_, vgaGain_);
}

void AirspySource::setLnaAgc(bool on)
{
    lnaAgc_ = on;
    if (dev_ && gainMode_ == 2)
    {
        airspy_set_lna_agc(dev_, on ? 1 : 0);
        if (!on)
            airspy_set_lna_gain(dev_, lnaGain_);
    }
}

void AirspySource::setMixerAgc(bool on)
{
    mixerAgc_ = on;
    if (dev_ && gainMode_ == 2)
    {
        airspy_set_mixer_agc(dev_, on ? 1 : 0);
        if (!on)
            airspy_set_mixer_gain(dev_, mixerGain_);
    }
}

void AirspySource::applyGain()
{
    if (!dev_)
        return;

    switch (gainMode_)
    {
    case 0: // Sensitivity
        airspy_set_lna_agc(dev_, 0);
        airspy_set_mixer_agc(dev_, 0);
        airspy_set_sensitivity_gain(dev_, senseGain_);
        break;
    case 1: // Linear
        airspy_set_lna_agc(dev_, 0);
        airspy_set_mixer_agc(dev_, 0);
        airspy_set_linearity_gain(dev_, linearGain_);
        break;
    case 2: // Free
        if (lnaAgc_)
        {
            airspy_set_lna_agc(dev_, 1);
        }
        else
        {
            airspy_set_lna_agc(dev_, 0);
            airspy_set_lna_gain(dev_, lnaGain_);
        }
        if (mixerAgc_)
        {
            airspy_set_mixer_agc(dev_, 1);
        }
        else
        {
            airspy_set_mixer_agc(dev_, 0);
            airspy_set_mixer_gain(dev_, mixerGain_);
        }
        airspy_set_vga_gain(dev_, vgaGain_);
        break;
    }
}
