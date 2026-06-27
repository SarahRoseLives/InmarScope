#include "audio/audio_player.h"

#include "miniaudio.h"

#include <cstdio>
#include <thread>

struct AudioPlayer::Impl
{
    ma_decoder decoder{};
    ma_device  device{};
    bool       decoderOpen = false;
    bool       deviceOpen  = false;
    bool       finished    = false;

    static void deviceCallback(ma_device* dev, void* out, const void*, ma_uint32 frames)
    {
        Impl* self = (Impl*)dev->pUserData;
        if (!self || self->finished)
        {
            ma_silence_pcm_frames(out, frames, ma_format_s16, dev->playback.channels);
            return;
        }

        ma_uint64 read = 0;
        ma_result r = ma_decoder_read_pcm_frames(&self->decoder, out, frames, &read);
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
