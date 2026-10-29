#include "audio/audio_player.h"

#include "miniaudio.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <thread>

struct AudioPlayer::Impl
{
    ma_decoder decoder{};
    ma_device  device{};
    bool       decoderOpen = false;
    bool       deviceOpen  = false;
    std::atomic<bool>     finished{false};
    std::atomic<uint64_t> played{0};   // frames handed to the device
    std::atomic<bool>     paused{false};
    std::atomic<int64_t>  seekTo{-1};  // frame to jump to (done on the audio thread)
    uint32_t              rate = 8000;

    static void deviceCallback(ma_device* dev, void* out, const void*, ma_uint32 frames)
    {
        Impl* self = (Impl*)dev->pUserData;
        if (self) {
            const int64_t to = self->seekTo.exchange(-1);
            if (to >= 0 && ma_decoder_seek_to_pcm_frame(&self->decoder, (ma_uint64)to) == MA_SUCCESS) {
                self->played.store((uint64_t)to);
                self->finished = false;
            }
        }
        if (!self || self->finished || self->paused)
        {
            ma_silence_pcm_frames(out, frames, ma_format_s16, dev->playback.channels);
            return;
        }

        ma_uint64 read = 0;
        ma_result r = ma_decoder_read_pcm_frames(&self->decoder, out, frames, &read);
        self->played.fetch_add(read);
        if (read < frames)
        {
            // Fill remainder with silence
            ma_silence_pcm_frames((char*)out + read * ma_get_bytes_per_frame(dev->playback.format, dev->playback.channels),
                                  frames - read, dev->playback.format, dev->playback.channels);
        }
        if (r != MA_SUCCESS && r != MA_AT_END)
            self->finished = true;
        if (r == MA_AT_END)
            self->finished = true;
    }
};

void AudioPlayer::play(const std::string& path)
{
    stop();

    impl_ = new Impl;

    ma_decoder_config cfg = ma_decoder_config_init(ma_format_s16, 1, 8000);
    ma_result r = ma_decoder_init_file(path.c_str(), &cfg, &impl_->decoder);
    if (r != MA_SUCCESS)
    {
        delete impl_;
        impl_ = nullptr;
        return;
    }
    impl_->decoderOpen = true;

    ma_device_config devCfg = ma_device_config_init(ma_device_type_playback);
    devCfg.playback.format   = ma_format_s16;
    devCfg.playback.channels = 1;
    devCfg.sampleRate        = impl_->decoder.outputSampleRate;
    impl_->rate              = impl_->decoder.outputSampleRate ? impl_->decoder.outputSampleRate : 8000;
    devCfg.dataCallback      = Impl::deviceCallback;
    devCfg.pUserData         = impl_;

    r = ma_device_init(nullptr, &devCfg, &impl_->device);
    if (r != MA_SUCCESS)
    {
        ma_decoder_uninit(&impl_->decoder);
        delete impl_;
        impl_ = nullptr;
        return;
    }
    impl_->deviceOpen = true;

    ma_device_start(&impl_->device);
    path_ = path;
    playing_.store(true);
}

void AudioPlayer::stop()
{
    playing_.store(false);
    posSec_.store(0.0f);
    path_.clear();
    if (!impl_)
        return;

    if (impl_->deviceOpen)
    {
        ma_device_stop(&impl_->device);
        ma_device_uninit(&impl_->device);
    }
    if (impl_->decoderOpen)
        ma_decoder_uninit(&impl_->decoder);
    delete impl_;
    impl_ = nullptr;
}

bool AudioPlayer::isPlaying() const
{
    return playing_.load() && impl_ && !impl_->finished.load() && !impl_->paused.load();
}

void AudioPlayer::pause()  { if (impl_) impl_->paused.store(true); }
void AudioPlayer::resume() { if (impl_) impl_->paused.store(false); }
bool AudioPlayer::isPaused() const { return impl_ && impl_->paused.load(); }

void AudioPlayer::seek(float sec)
{
    if (!impl_) return;
    impl_->seekTo.store((int64_t)((double)std::max(0.0f, sec) * impl_->rate));
}

float AudioPlayer::positionSec() const
{
    if (!impl_) return 0.0f;
    // A seek not yet picked up by the audio thread shows where it will land.
    const int64_t to = impl_->seekTo.load();
    if (to >= 0) return (float)((double)to / (double)impl_->rate);
    return (float)((double)impl_->played.load() / (double)impl_->rate);
}

bool AudioPlayer::loadPcm(const std::string& path, std::vector<int16_t>& out)
{
    out.clear();
    ma_decoder dec;
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_s16, 1, 8000);
    if (ma_decoder_init_file(path.c_str(), &cfg, &dec) != MA_SUCCESS) return false;
    int16_t buf[4096];
    for (;;) {
        ma_uint64 got = 0;
        ma_result r = ma_decoder_read_pcm_frames(&dec, buf, 4096, &got);
        out.insert(out.end(), buf, buf + got);
        if (got == 0 || r != MA_SUCCESS || out.size() > 8000u * 3600u) break;
    }
    ma_decoder_uninit(&dec);
    return !out.empty();
}
