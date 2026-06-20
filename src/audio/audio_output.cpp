#include "audio/audio_output.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

struct AudioOutputImpl
{
    ma_device device{};
    bool started = false;

    std::mutex mtx;
    std::vector<int16_t> ring;
    size_t cap = 0;
    size_t rd = 0, wr = 0, count = 0;

    float levelRms = 0.0f;

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

static void dataCallback(ma_device* dev, void* out, const void*, ma_uint32 frames)
{
    auto* impl = static_cast<AudioOutputImpl*>(dev->pUserData);
    impl->pullInto(static_cast<int16_t*>(out), frames);
}

AudioOutput::AudioOutput() : impl_(new AudioOutputImpl) {}

AudioOutput::~AudioOutput()
{
    stop();
}

bool AudioOutput::start(int sampleRate)
{
    if (impl_->started)
        return true;

    impl_->cap = (size_t)sampleRate; // ~1 s buffer
    impl_->ring.assign(impl_->cap, 0);
    impl_->rd = impl_->wr = impl_->count = 0;

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_s16;
    cfg.playback.channels = 1;
    cfg.sampleRate = (ma_uint32)sampleRate;
    cfg.dataCallback = dataCallback;
    cfg.pUserData = impl_.get();

    if (ma_device_init(nullptr, &cfg, &impl_->device) != MA_SUCCESS)
        return false;
    if (ma_device_start(&impl_->device) != MA_SUCCESS)
    {
        ma_device_uninit(&impl_->device);
        return false;
    }
    impl_->started = true;
    return true;
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

void AudioOutput::push(const int16_t* pcm, int n)
{
    if (!impl_->started || n <= 0)
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
