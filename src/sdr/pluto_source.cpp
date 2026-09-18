#include "sdr/pluto_source.h"

#include "util/log.h"

#include <iio.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>

namespace {
std::string iioError(int code)
{
    char buf[256] = {};
    iio_strerror(code < 0 ? -code : code, buf, sizeof(buf));
    return buf[0] ? std::string(buf) : std::string("unknown IIO error");
}

bool writeLongLong(iio_channel* ch, const char* attr, long long value,
                   std::string& err)
{
    int rc = iio_channel_attr_write_longlong(ch, attr, value);
    if (rc >= 0)
        return true;
    err = std::string("Could not set ") + attr + ": " + iioError(rc);
    return false;
}

int32_t decodeSample(const char* ptr, const iio_data_format* fmt)
{
    uint16_t raw = 0;
    std::memcpy(&raw, ptr, sizeof(raw));
    if (fmt->is_be)
        raw = (uint16_t)((raw >> 8) | (raw << 8));

    uint32_t value = (uint32_t)raw >> fmt->shift;
    unsigned bits = std::min(fmt->bits, 16u);
    if (bits == 0)
        return 0;
    uint32_t mask = bits == 16 ? 0xffffu : ((1u << bits) - 1u);
    value &= mask;
    if (fmt->is_signed && (value & (1u << (bits - 1))))
        value |= ~mask;
    return (int32_t)value;
}
} // namespace

PlutoSource::~PlutoSource()
{
    stop();
}

std::vector<SdrDeviceInfo> PlutoSource::listDevices()
{
    std::vector<SdrDeviceInfo> out;
    iio_context* ctx = iio_create_context_from_uri(uri_.c_str());
    if (!ctx)
        return out;

    iio_device* phy = iio_context_find_device(ctx, "ad9361-phy");
    if (!phy)
        phy = iio_context_find_device(ctx, "ad9364-phy");
    if (phy)
    {
        SdrDeviceInfo info;
        info.index = 0;
        const char* name = iio_device_get_name(phy);
        info.name = name ? name : "AD936x";
        info.serial = uri_;
        out.push_back(std::move(info));
    }
    iio_context_destroy(ctx);
    return out;
}

bool PlutoSource::openContext(std::string& err)
{
    ctx_ = iio_create_context_from_uri(uri_.c_str());
    if (!ctx_)
    {
        err = "Could not open IIO context '" + uri_ + "': " + iioError(errno);
        return false;
    }

    phy_ = iio_context_find_device(ctx_, "ad9361-phy");
    if (!phy_)
        phy_ = iio_context_find_device(ctx_, "ad9364-phy");
    rxDev_ = iio_context_find_device(ctx_, "cf-ad9361-lpc");
    if (!rxDev_)
        rxDev_ = iio_context_find_device(ctx_, "cf-ad9361-A");
    if (!phy_ || !rxDev_)
    {
        err = "The IIO context does not expose an AD936x PHY and RX streaming device";
        return false;
    }

    rxCfg_ = iio_device_find_channel(phy_, "voltage0", false);
    rxLo_ = iio_device_find_channel(phy_, "altvoltage0", true);
    rxI_ = iio_device_find_channel(rxDev_, "voltage0", false);
    if (!rxI_)
        rxI_ = iio_device_find_channel(rxDev_, "altvoltage0", false);
    rxQ_ = iio_device_find_channel(rxDev_, "voltage1", false);
    if (!rxQ_)
        rxQ_ = iio_device_find_channel(rxDev_, "altvoltage1", false);
    if (!rxCfg_ || !rxLo_ || !rxI_ || !rxQ_)
    {
        err = "The AD936x IIO channels required for RX were not found";
        return false;
    }
    return true;
}

bool PlutoSource::configure(std::string& err)
{
    int rc = iio_channel_attr_write(rxCfg_, "rf_port_select", rfPort_.c_str());
    if (rc < 0)
    {
        err = "Could not select Pluto RX port: " + iioError(rc);
        return false;
    }
    if (!writeLongLong(rxCfg_, "rf_bandwidth", (long long)std::llround(bandwidth_), err) ||
        !writeLongLong(rxCfg_, "sampling_frequency", (long long)std::llround(sampleRate_), err) ||
        !writeLongLong(rxLo_, "frequency",
                       (long long)std::llround(centerFreq_ * (1.0 + ppm_ / 1e6)), err))
        return false;

    rc = iio_channel_attr_write(rxCfg_, "gain_control_mode",
                                agc_ ? "slow_attack" : "manual");
    if (rc < 0)
    {
        err = "Could not set Pluto gain-control mode: " + iioError(rc);
        return false;
    }
    if (!agc_)
    {
        rc = iio_channel_attr_write_double(rxCfg_, "hardwaregain", gainDb_);
        if (rc < 0)
        {
            err = "Could not set Pluto RX gain: " + iioError(rc);
            return false;
        }
    }
    iio_channel_enable(rxI_);
    iio_channel_enable(rxQ_);

    const iio_data_format* fmtI = iio_channel_get_data_format(rxI_);
    const iio_data_format* fmtQ = iio_channel_get_data_format(rxQ_);
    if (!fmtI || !fmtQ || fmtI->length != 16 || fmtQ->length != 16 ||
        fmtI->bits == 0 || fmtI->bits > 16 || fmtQ->bits == 0 || fmtQ->bits > 16)
    {
        err = "Unsupported Pluto RX sample format (expected 16-bit I/Q containers)";
        return false;
    }

    rxBuf_ = iio_device_create_buffer(rxDev_, 32768, false);
    if (!rxBuf_)
    {
        err = "Could not create Pluto RX buffer: " + iioError(errno);
        return false;
    }
    return true;
}

bool PlutoSource::start(int, SdrSampleCb cb, std::string& err)
{
    if (running_.load())
        return true;
    if (readThread_.joinable())
        readThread_.join();
    cleanup();
    if (!openContext(err) || !configure(err))
    {
        cleanup();
        return false;
    }

    cb_ = std::move(cb);
    dcOffRe_ = dcOffIm_ = 0.0f;
    dcRate_ = (float)(50.0 / sampleRate_);
    running_.store(true);
    readThread_ = std::thread(&PlutoSource::readLoop, this);
    return true;
}

void PlutoSource::stop()
{
    running_.store(false);
    if (rxBuf_)
        iio_buffer_cancel(rxBuf_);
    if (readThread_.joinable())
        readThread_.join();
    cleanup();
}

void PlutoSource::cleanup()
{
    if (rxBuf_)
    {
        iio_buffer_destroy(rxBuf_);
        rxBuf_ = nullptr;
    }
    if (rxI_)
        iio_channel_disable(rxI_);
    if (rxQ_)
        iio_channel_disable(rxQ_);
    rxI_ = rxQ_ = rxCfg_ = rxLo_ = nullptr;
    phy_ = rxDev_ = nullptr;
    if (ctx_)
    {
        iio_context_destroy(ctx_);
        ctx_ = nullptr;
    }
    cb_ = nullptr;
}

void PlutoSource::readLoop()
{
    const iio_data_format* fmtI = iio_channel_get_data_format(rxI_);
    const iio_data_format* fmtQ = iio_channel_get_data_format(rxQ_);
    const float normI = (float)(1u << (std::min(fmtI->bits, 16u) - 1));
    const float normQ = (float)(1u << (std::min(fmtQ->bits, 16u) - 1));

    while (running_.load())
    {
        ssize_t nbytes = iio_buffer_refill(rxBuf_);
        if (nbytes < 0)
        {
            if (running_.load())
                logWrite("Pluto: RX refill failed: %s", iioError((int)nbytes).c_str());
            break;
        }

        ptrdiff_t step = iio_buffer_step(rxBuf_);
        ssize_t sampleBytes = iio_device_get_sample_size(rxDev_);
        if (step <= 0 || sampleBytes <= 0)
            break;
        size_t count = (size_t)nbytes / (size_t)sampleBytes;
        scratch_.resize(count * 2);
        const char* pI = static_cast<const char*>(iio_buffer_first(rxBuf_, rxI_));
        const char* pQ = static_cast<const char*>(iio_buffer_first(rxBuf_, rxQ_));

        float offRe = dcOffRe_;
        float offIm = dcOffIm_;
        const float rate = dcRate_;
        const bool blockDc = dcBlock_.load();
        for (size_t i = 0; i < count; ++i, pI += step, pQ += step)
        {
            float re = (float)decodeSample(pI, fmtI) / normI;
            float im = (float)decodeSample(pQ, fmtQ) / normQ;
            if (blockDc)
            {
                float outRe = re - offRe;
                offRe += outRe * rate;
                float outIm = im - offIm;
                offIm += outIm * rate;
                re = outRe;
                im = outIm;
            }
            scratch_[i * 2] = re;
            scratch_[i * 2 + 1] = im;
        }
        dcOffRe_ = offRe;
        dcOffIm_ = offIm;
        if (cb_ && count > 0)
            cb_(scratch_.data(), (int)count);
    }
    running_.store(false);
}

void PlutoSource::applyTune()
{
    if (rxLo_)
        iio_channel_attr_write_longlong(
            rxLo_, "frequency",
            (long long)std::llround(centerFreq_ * (1.0 + ppm_ / 1e6)));
}

void PlutoSource::applyGain()
{
    if (!rxCfg_)
        return;
    if (agc_)
        iio_channel_attr_write(rxCfg_, "gain_control_mode", "slow_attack");
    else
    {
        iio_channel_attr_write(rxCfg_, "gain_control_mode", "manual");
        iio_channel_attr_write_double(rxCfg_, "hardwaregain", gainDb_);
    }
}

void PlutoSource::setCenterFreq(double hz)
{
    centerFreq_ = hz;
    applyTune();
}

void PlutoSource::setSampleRate(double hz)
{
    sampleRate_ = hz;
    if (rxCfg_)
        iio_channel_attr_write_longlong(rxCfg_, "sampling_frequency",
                                        (long long)std::llround(hz));
}

void PlutoSource::setBandwidth(double hz)
{
    bandwidth_ = hz;
    if (rxCfg_)
        iio_channel_attr_write_longlong(rxCfg_, "rf_bandwidth",
                                        (long long)std::llround(hz));
}

void PlutoSource::setGain(double db)
{
    agc_ = db < 0.0;
    if (!agc_)
        gainDb_ = std::clamp(db, -3.0, 73.0);
    applyGain();
}

void PlutoSource::setRfPort(const std::string& port)
{
    rfPort_ = port;
    if (rxCfg_)
        iio_channel_attr_write(rxCfg_, "rf_port_select", rfPort_.c_str());
}

void PlutoSource::setPpm(double ppm)
{
    ppm_ = ppm;
    applyTune();
}
