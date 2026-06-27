#include "audio/audio_output.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

#define MA_NO_DECODING
#define MA_NO_GENERATION
#include "miniaudio.h"

struct AudioOutputImpl
{
    ma_context context{};
    bool ctxInited = false;
    ma_device device{};
    bool started = false;

    std::vector<ma_device_info> playbackInfos; // cached enumeration
    int  selected = 0;        // 0 = system default, else index into playbackInfos+1
    bool hasId = false;
    ma_device_id id{};
    int  sampleRate = 8000;

    std::mutex mtx;
    std::vector<int16_t> ring;
    size_t cap = 0;
    size_t rd = 0, wr = 0, count = 0;

    float levelRms = 0.0f;

    bool ensureContext()
    {
        if (ctxInited)
            return true;
        if (ma_context_init(nullptr, 0, nullptr, &context) != MA_SUCCESS)
            return false;
        ctxInited = true;
        return true;
    }

    bool openDevice()
    {
        ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
        cfg.playback.format = ma_format_s16;
        cfg.playback.channels = 1;
        cfg.playback.pDeviceID = hasId ? &id : nullptr;
        cfg.sampleRate = (ma_uint32)sampleRate;
        cfg.pUserData = this;
        cfg.dataCallback = +[](ma_device* dev, void* out, const void*, ma_uint32 frames) {
            static_cast<AudioOutputImpl*>(dev->pUserData)->pullInto(static_cast<int16_t*>(out),
                                                                    frames);
        };
        ma_context* ctx = ensureContext() ? &context : nullptr;
        if (ma_device_init(ctx, &cfg, &device) != MA_SUCCESS)
            return false;
        if (ma_device_start(&device) != MA_SUCCESS)
        {
            ma_device_uninit(&device);
            return false;
        }
        started = true;
        return true;
    }

    void pullInto(int16_t* out, ma_uint32 frames)
    {
        std::lock_guard<std::mutex> lk(mtx);
        double pwr = 0.0;
        for (ma_uint32 i = 0; i < frames; ++i)
        {
            int16_t s = 0;
            if (count > 0)
            {
                s = ring[rd];
                rd = (rd + 1) % cap;
                --count;
            }
            out[i] = s;
            pwr += (double)s * s;
        }
        if (frames)
            levelRms = (float)(std::sqrt(pwr / frames) / 32768.0);
    }
};

AudioOutput::AudioOutput() : impl_(new AudioOutputImpl) {}

AudioOutput::~AudioOutput()
{
    stop();
    if (impl_->ctxInited)
    {
        ma_context_uninit(&impl_->context);
        impl_->ctxInited = false;
    }
}

bool AudioOutput::start(int sampleRate)
{
    if (impl_->started)
        return true;

    impl_->sampleRate = sampleRate;
    impl_->cap = (size_t)sampleRate; // ~1 s buffer
    impl_->ring.assign(impl_->cap, 0);
    impl_->rd = impl_->wr = impl_->count = 0;

    return impl_->openDevice();
}

void AudioOutput::stop()
{
    if (impl_->started)
    {
        ma_device_uninit(&impl_->device);
        impl_->started = false;
    }
}

bool AudioOutput::running() const { return impl_->started; }

std::vector<std::string> AudioOutput::listDevices()
{
    std::vector<std::string> names;
    names.push_back("Default (system)");
    if (!impl_->ensureContext())
        return names;

    ma_device_info* playback = nullptr;
    ma_uint32 playbackCount = 0;
    ma_device_info* capture = nullptr;
    ma_uint32 captureCount = 0;
    if (ma_context_get_devices(&impl_->context, &playback, &playbackCount, &capture,
                               &captureCount) != MA_SUCCESS)
        return names;

    impl_->playbackInfos.assign(playback, playback + playbackCount);
    for (ma_uint32 i = 0; i < playbackCount; ++i)
        names.push_back(impl_->playbackInfos[i].name);
    return names;
}

void AudioOutput::setDevice(int index)
{
    if (index == impl_->selected)
        return;
    impl_->selected = index;
    if (index <= 0 || index > (int)impl_->playbackInfos.size())
    {
        impl_->hasId = false;
        impl_->selected = 0;
    }
    else
    {
        impl_->id = impl_->playbackInfos[index - 1].id;
        impl_->hasId = true;
    }

    // Live-restart on the newly selected device.
    if (impl_->started)
    {
        ma_device_uninit(&impl_->device);
        impl_->started = false;
        std::lock_guard<std::mutex> lk(impl_->mtx);
        impl_->rd = impl_->wr = impl_->count = 0;
        impl_->openDevice();
    }
}

int AudioOutput::currentDevice() const { return impl_->selected; }

void AudioOutput::push(const int16_t* pcm, int n)
{
    if (muted_.load() || !impl_->started || n <= 0)
        return;
    std::lock_guard<std::mutex> lk(impl_->mtx);
    for (int i = 0; i < n; ++i)
    {
        if (impl_->count == impl_->cap)
        {
            // Buffer full: drop oldest to stay near real time.
            impl_->rd = (impl_->rd + 1) % impl_->cap;
            --impl_->count;
        }
        impl_->ring[impl_->wr] = pcm[i];
        impl_->wr = (impl_->wr + 1) % impl_->cap;
        ++impl_->count;
    }
}

void AudioOutput::clear()
{
    std::lock_guard<std::mutex> lk(impl_->mtx);
    impl_->rd = impl_->wr = impl_->count = 0;
}

float AudioOutput::level() const { return impl_->levelRms; }
