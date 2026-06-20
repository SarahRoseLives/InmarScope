// Simple streaming audio output for decoded voice (8 kHz mono int16).
// Thread-safe push() from the decode thread; an internal ring feeds the
// audio device callback. Backed by miniaudio (cross-platform).
#pragma once

#include <cstdint>
#include <memory>

struct AudioOutputImpl; // defined in audio_output.cpp

class AudioOutput
{
public:
    AudioOutput();
    ~AudioOutput();

    bool start(int sampleRate = 8000);
    void stop();
    bool running() const;

    // Append PCM samples (mono int16). Safe from any thread.
    void push(const int16_t* pcm, int n);
    void clear();

    float level() const; // recent output RMS (0..1) for a UI meter

private:
    std::unique_ptr<AudioOutputImpl> impl_;
};
