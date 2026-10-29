// Playback of recorded WAV/OGG voice files via miniaudio.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

class AudioPlayer
{
public:
    ~AudioPlayer() { stop(); }

    // Start playing a file (stops any current playback first).
    void play(const std::string& path);

    // Stop playback.
    void stop();

    // True while audio is still coming out (false once the file has ended or
    // while paused).
    bool isPlaying() const;

    // Pause / resume the current file (position kept). No-op if none loaded.
    void pause();
    void resume();
    bool isPaused() const;
    // A file is loaded (playing, paused or ended but not stopped).
    bool isLoaded() const { return impl_ != nullptr; }
    // Jump to a position in the loaded file; plays on from there unless
    // paused (also restarts a file that has reached its end).
    void seek(float sec);
    const std::string& currentPath() const { return path_; }

    // Current playback position in seconds (0 if nothing loaded).
    float positionSec() const;

    // Decodes a whole recording (WAV/OGG...) to 8 kHz mono int16 for display.
    static bool loadPcm(const std::string& path, std::vector<int16_t>& out);

private:
    struct Impl;
    Impl* impl_ = nullptr;

    std::atomic<bool> playing_{false};
    std::atomic<float> posSec_{0.0f};
    std::string path_;
};
