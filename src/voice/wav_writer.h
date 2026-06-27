// Voice call recording: writes 16-bit PCM to WAV or OGG Vorbis files.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

enum class RecordFormat { WAV = 0, OGG = 1 };
struct WavWriterOggImpl; // OGG Vorbis state (defined in the .cpp, uses libvorbis)

class WavWriter
{
public:
    WavWriter() = default;
    ~WavWriter() { close(); }

    WavWriter(const WavWriter&) = delete;
    WavWriter& operator=(const WavWriter&) = delete;

    void setFormat(RecordFormat fmt) { fmt_ = fmt; }

    // Create/overwrite a 16-bit PCM WAV or OGG Vorbis file.
    bool open(const std::string& path, int sampleRate = 8000, int channels = 1);

    // Append interleaved 16-bit samples (count = frames * channels).
    void write(const int16_t* data, int count);

    // Finalize and close. Safe to call multiple times.
    void close();

    bool isOpen() const { return isOpen_; }
    const std::string& path() const { return path_; }

private:
    void closeWav();
    void closeOgg();

    RecordFormat fmt_ = RecordFormat::WAV;
    bool isOpen_ = false;

    // WAV state
    std::FILE* f_ = nullptr;
    uint32_t dataBytes_ = 0;
    int sampleRate_ = 8000;
    int channels_ = 1;
    bool hasAudio_ = false;
    std::string path_;

    // OGG state
    WavWriterOggImpl* ogg_ = nullptr;
};
