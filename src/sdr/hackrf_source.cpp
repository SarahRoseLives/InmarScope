#include "sdr/hackrf_source.h"

#include <libhackrf/hackrf.h>

#include <cmath>
#include <mutex>
#include <string>

namespace {
// libhackrf needs a single global init. Init once on first use; we don't call
// hackrf_exit() (process teardown handles it, and exit errors if a device is
// still open).
void ensureHackrfInit()
{
    static std::once_flag once;
    std::call_once(once, []() { hackrf_init(); });
}

// C-style RX trampoline. hackrf_transfer is a typedef to an anonymous struct,
// so this lives here (where the full type is visible) rather than as a member.
int rxTrampoline(hackrf_transfer* transfer)
{
    auto* self = static_cast<HackRfSource*>(transfer->rx_ctx);
    return self->handleRx((const int8_t*)transfer->buffer, transfer->valid_length);
}
} // namespace

HackRfSource::~HackRfSource()
{
    stop();
}

std::vector<SdrDeviceInfo> HackRfSource::listDevices()
{
    ensureHackrfInit();
    std::vector<SdrDeviceInfo> out;
    hackrf_device_list_t* list = hackrf_device_list();
    if (!list)
        return out;
    for (int i = 0; i < list->devicecount; ++i)
    {
        SdrDeviceInfo info;
        info.index = i;
        info.name = "HackRF";
        info.serial = list->serial_numbers[i] ? list->serial_numbers[i] : "";
        out.push_back(info);
    }
    hackrf_device_list_free(list);
    return out;
}

bool HackRfSource::start(int deviceIndex, SdrSampleCb cb, std::string& err)
{
    if (running_.load())
        return true;
    ensureHackrfInit();

    int r = HACKRF_SUCCESS;
    hackrf_device_list_t* list = hackrf_device_list();
    if (list && list->devicecount > 0)
    {
        if (deviceIndex < 0 || deviceIndex >= list->devicecount)
            deviceIndex = 0;
        r = hackrf_device_list_open(list, deviceIndex, &dev_);
    }
    else
    {
        r = hackrf_open(&dev_);
    }
    if (list)
        hackrf_device_list_free(list);

    if (r != HACKRF_SUCCESS || !dev_)
    {
        err = std::string("HackRF open failed: ") + hackrf_error_name((hackrf_error)r);
        dev_ = nullptr;
        return false;
    }

    hackrf_set_sample_rate(dev_, sampleRate_);
    hackrf_set_baseband_filter_bandwidth(
        dev_, hackrf_compute_baseband_filter_bw((uint32_t)sampleRate_));
    hackrf_set_freq(dev_, (uint64_t)centerFreq_);
    hackrf_set_lna_gain(dev_, (uint32_t)lnaGain_);
    hackrf_set_vga_gain(dev_, (uint32_t)vgaGain_);
    hackrf_set_amp_enable(dev_, amp_ ? 1 : 0);
    // Bias-tee / port power must be (re)enabled each RX session; the firmware
    // disables it whenever the device returns to IDLE.
    hackrf_set_antenna_enable(dev_, biasTee_ ? 1 : 0);

    cb_ = std::move(cb);
    dcOffRe_ = dcOffIm_ = 0.0f;
    dcRate_ = (float)(50.0 / sampleRate_);

    r = hackrf_start_rx(dev_, &rxTrampoline, this);
    if (r != HACKRF_SUCCESS)
    {
        err = std::string("HackRF start_rx failed: ") + hackrf_error_name((hackrf_error)r);
        hackrf_close(dev_);
        dev_ = nullptr;
        cb_ = nullptr;
        return false;
    }
    running_.store(true);
    return true;
}

void HackRfSource::stop()
{
    if (dev_)
    {
        hackrf_stop_rx(dev_);
        hackrf_set_antenna_enable(dev_, 0);
        hackrf_close(dev_);
        dev_ = nullptr;
    }
    running_.store(false);
    cb_ = nullptr;
}

int HackRfSource::handleRx(const int8_t* buf, int len)
{
    if (!running_.load() || !cb_)
        return 0;

    int num = len / 2; // interleaved I/Q int8
    if ((int)scratch_.size() < num * 2)
        scratch_.resize((size_t)num * 2);

    float* out = scratch_.data();
    bool dc = dcBlock_.load();
    float offRe = dcOffRe_, offIm = dcOffIm_;
    const float rate = dcRate_;
    for (int i = 0; i < num; ++i)
    {
        float re = buf[i * 2] * (1.0f / 128.0f);
        float im = buf[i * 2 + 1] * (1.0f / 128.0f);
        if (dc)
        {
            float ore = re - offRe;
            offRe += ore * rate;
            float oim = im - offIm;
            offIm += oim * rate;
            out[i * 2] = ore;
            out[i * 2 + 1] = oim;
        }
        else
        {
            out[i * 2] = re;
            out[i * 2 + 1] = im;
        }
    }
    dcOffRe_ = offRe;
    dcOffIm_ = offIm;

    cb_(out, num);
    return 0; // keep streaming
}

void HackRfSource::setCenterFreq(double hz)
{
    centerFreq_ = hz;
    if (dev_)
        hackrf_set_freq(dev_, (uint64_t)hz);
}

void HackRfSource::setSampleRate(double hz)
{
    sampleRate_ = hz;
    if (dev_)
    {
        hackrf_set_sample_rate(dev_, hz);
        hackrf_set_baseband_filter_bandwidth(
            dev_, hackrf_compute_baseband_filter_bw((uint32_t)hz));
    }
}

void HackRfSource::setGain(double db)
{
    // Single-knob fallback maps to the baseband (VGA) gain.
    setVgaGain((int)std::lround(db));
}

void HackRfSource::setLnaGain(int db)
{
    if (db < 0) db = 0;
    if (db > 40) db = 40;
    db = (db / 8) * 8; // 8 dB steps
    lnaGain_ = db;
    if (dev_)
        hackrf_set_lna_gain(dev_, (uint32_t)db);
}

void HackRfSource::setVgaGain(int db)
{
    if (db < 0) db = 0;
    if (db > 62) db = 62;
    db = (db / 2) * 2; // 2 dB steps
    vgaGain_ = db;
    if (dev_)
        hackrf_set_vga_gain(dev_, (uint32_t)db);
}

void HackRfSource::setAmpEnable(bool on)
{
    amp_ = on;
    if (dev_)
        hackrf_set_amp_enable(dev_, on ? 1 : 0);
}

void HackRfSource::setBiasTee(bool on)
{
    biasTee_ = on;
    if (dev_)
        hackrf_set_antenna_enable(dev_, on ? 1 : 0);
}
