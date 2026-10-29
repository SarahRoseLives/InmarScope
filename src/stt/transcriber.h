// Speech-to-text for saved voice calls (whisper.cpp), shared by IridiScope
// and InmarScope. App-independent: no ImGui, no engine types.
//
// Calls are queued after they are saved and transcribed one at a time on a
// background thread (lowest priority; whisper's own compute threads too), so
// transcription never competes with the radio or the decoders.
//
// Two models, downloaded on request into <models dir>:
//   small  (ggml-small-q5_1.bin, ~180 MB)         - every call; detects language
//   large  (ggml-large-v3-turbo-q5_0.bin, ~550 MB) - re-transcribes non-English
//                                                    calls (and translations)
// Without the large model, non-English calls keep the small model's text.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace stt {

struct Result {
    bool ok = false;          // transcribed (text may still be empty: no speech)
    std::string text;         // in the spoken language
    std::string lang;         // ISO code, e.g. "en", "es"
    std::string text_en;      // English translation (non-English, option on)
    std::string model;        // "small" / "large"
    double speech_s = 0;      // audio seconds given to the model (silence removed)
    double proc_s = 0;        // wall time spent
    std::string error;
};

enum class ModelState { Missing, Downloading, Ready, Failed };

class Transcriber {
public:
    static Transcriber &instance();
    // False when built without whisper.cpp (the UI then says so).
    static constexpr bool available() {
#ifdef HAS_WHISPER
        return true;
#else
        return false;
#endif
    }

    // Folder for model files (created if needed). Call once at startup.
    void set_models_dir(const std::string &dir);
    void set_enabled(bool on) { enabled_.store(on); cv_.notify_all(); }
    bool enabled() const { return enabled_.load(); }
    void set_translate(bool on) { translate_.store(on); }
    // Every call on the large model (when downloaded); the small model then
    // only picks the language from the first 30 s.
    void set_large_for_all(bool on) { large_all_.store(on); }
    // Words the model should expect (names, jargon); passed as a prompt.
    void set_vocabulary(const std::string &v);
    // Run on every thread this module starts (e.g. to set its priority).
    void set_thread_init(std::function<void()> f) { thread_init_ = std::move(f); }
    // Diagnostics (one line each), from the worker / download threads.
    void set_logger(std::function<void(const std::string &)> f) { log_ = std::move(f); }

    // Models: 0 = small, 1 = large, 2 = speech detector (Silero VAD, ~1 MB,
    // fetched with the small model; keeps Whisper from inventing text on
    // noise or silence).
    ModelState model_state(int which) const;
    float download_progress(int which) const;    // 0..1 while downloading
    double model_size_mb(int which) const;       // expected size
    std::string model_path(int which) const;
    void download(int which);                    // background; no-op if ready/busy
    void cancel_download(int which) { dl_cancel_[which].store(true); }

    // Queue a call (8 kHz mono PCM). txt_path: where to write the transcript
    // ("" = don't). done() runs on the worker thread.
    using Done = std::function<void(int id, const Result &)>;
    void submit(int id, std::vector<int16_t> pcm8k, std::string txt_path, Done done);
    // Same, but the audio is produced on the worker thread (e.g. decoding a
    // recorded file), so the caller never blocks.
    using Loader = std::function<std::vector<int16_t>()>;
    void submit_loader(int id, Loader load, std::string txt_path, Done done);
    int queued() const;
    int busy_id() const { return busy_id_.load(); }   // call being transcribed, -1 = none

    void shutdown();   // stop the worker (app exit)

private:
    Transcriber() = default;
    ~Transcriber() { shutdown(); }
    struct Job { int id; std::vector<int16_t> pcm; std::string txt; Done done; Loader load; };
    void worker();
    Result run(const std::vector<int16_t> &pcm8k);
    void log(const std::string &s) { if (log_) log_(s); }
    void download_main(int which);

    std::string dir_;
    std::atomic<bool> enabled_{false}, translate_{false}, large_all_{false}, stop_{false};
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<Job> q_;
    std::thread th_;
    std::atomic<int> busy_id_{-1};
    std::string vocab_;
    std::function<void()> thread_init_;
    std::function<void(const std::string &)> log_;
    // Models
    void *ctx_[2] = {nullptr, nullptr};
    void *vad_ = nullptr;   // whisper_vad_context (single-threaded)
    std::atomic<int> dl_state_[3] = {{0}, {0}, {0}};   // 0 idle, 1 downloading, 2 failed
    std::atomic<float> dl_prog_[3] = {{0.f}, {0.f}, {0.f}};
    std::atomic<bool> dl_cancel_[3] = {{false}, {false}, {false}};
    std::thread dl_th_[3];
    long long large_last_used_ms_ = 0;
};

// Post-processing helpers (exposed for tests).
std::vector<int16_t> squeeze_silence(const std::vector<int16_t> &pcm8k, double *kept_s);
std::vector<float> upsample_8k_to_16k(const std::vector<int16_t> &pcm8k);
std::string collapse_repeats(const std::string &text);

} // namespace stt
