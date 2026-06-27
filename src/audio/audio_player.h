// Playback of recorded WAV/OGG voice files via miniaudio.
#pragma once

#include <atomic>
#include <string>

class AudioPlayer
{
public:
    ~AudioPlayer() { stop(); }

    // Start playing a file (stops any current playback first).
    void play(const std::string& path);

    // Stop playback.
    void stop();

    bool isPlaying() const { return playing_.load(); }
    const std::string& currentPath() const { return path_; }

    // Current playback position in seconds (0 if not playing).
    float positionSec() const { return posSec_.load(); }

private:
    struct Impl;
    Impl* impl_ = nullptr;

    std::atomic<bool> playing_{false};
    std::atomic<float> posSec_{0.0f};
    std::string path_;
};
