#include "stt/transcriber.h"

#ifdef HAS_WHISPER
#include "whisper.h"
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <sstream>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#elif defined(HAS_LIBCURL)
#include <curl/curl.h>
#endif

namespace stt {

namespace {

constexpr double kPi = 3.14159265358979323846;

const char *kFile[3] = {"ggml-small-q5_1.bin", "ggml-large-v3-turbo-q5_0.bin", "ggml-silero-v6.2.0.bin"};
const double kSizeMb[3] = {190.0, 574.0, 0.9};
const char *kUrl[3] = {"https://huggingface.co/ggerganov/whisper.cpp/resolve/main/",
                       "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/",
                       "https://huggingface.co/ggml-org/whisper-vad/resolve/main/"};

long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

#ifdef HAS_WHISPER
void quiet_log(ggml_log_level, const char *, void *) {}
#endif

// ---- download (URL -> file, with progress) --------------------------------
#if defined(_WIN32)
bool fetch_to_file(const std::string &url, const std::string &path, std::atomic<float> &prog,
                   std::atomic<bool> &cancel, double expect_bytes, std::string &err) {
    std::wstring wurl(url.begin(), url.end());
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256], urlpath[2048];
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = urlpath; uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) { err = "bad URL"; return false; }
    HINTERNET s = WinHttpOpen(L"InmarScope (speech model)", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) { err = "WinHttpOpen failed"; return false; }
    WinHttpSetTimeouts(s, 15000, 15000, 30000, 60000);
    HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", urlpath, nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    bool ok = r && WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(r, nullptr);   // redirects (to the CDN) are followed
    DWORD status = 0;
    if (ok) {
        DWORD sz = sizeof(status);
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
        if (status != 200) { ok = false; err = "HTTP " + std::to_string(status); }
    } else err = "connection failed (" + std::to_string(GetLastError()) + ")";
    double total = expect_bytes;
    if (ok) {
        wchar_t lenbuf[32]; DWORD lsz = sizeof(lenbuf);
        if (WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, lenbuf, &lsz,
                                WINHTTP_NO_HEADER_INDEX))
            total = std::max(1.0, _wtof(lenbuf));
    }
    FILE *f = ok ? std::fopen(path.c_str(), "wb") : nullptr;
    if (ok && !f) { ok = false; err = "can't write " + path; }
    double got = 0;
    std::vector<char> buf(1 << 16);
    while (ok) {
        if (cancel.load()) { ok = false; err = "cancelled"; break; }
        DWORD n = 0;
        if (!WinHttpReadData(r, buf.data(), (DWORD)buf.size(), &n)) { ok = false; err = "read failed"; break; }
        if (n == 0) break;
        if (std::fwrite(buf.data(), 1, n, f) != n) { ok = false; err = "disk write failed"; break; }
        got += n;
        prog.store((float)std::min(1.0, got / total));
    }
    if (f) std::fclose(f);
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok && got > 1e5;
}
#elif defined(HAS_LIBCURL)
struct CurlCtx { FILE *f; std::atomic<float> *prog; std::atomic<bool> *cancel; double expect; };
size_t curl_write(char *p, size_t sz, size_t n, void *u) {
    auto *c = (CurlCtx *)u;
    return std::fwrite(p, sz, n, c->f) * sz;
}
int curl_progress(void *u, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t) {
    auto *c = (CurlCtx *)u;
    const double t = dltotal > 0 ? (double)dltotal : c->expect;
    c->prog->store((float)std::min(1.0, (double)dlnow / std::max(1.0, t)));
    return c->cancel->load() ? 1 : 0;
}
bool fetch_to_file(const std::string &url, const std::string &path, std::atomic<float> &prog,
                   std::atomic<bool> &cancel, double expect_bytes, std::string &err) {
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) { err = "can't write " + path; return false; }
    CurlCtx cx{f, &prog, &cancel, expect_bytes};
    CURL *c = curl_easy_init();
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "InmarScope");
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &cx);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, curl_progress);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, &cx);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    const CURLcode rc = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);
    std::fclose(f);
    if (rc != CURLE_OK) { err = curl_easy_strerror(rc); return false; }
    if (code != 200) { err = "HTTP " + std::to_string(code); return false; }
    return true;
}
#else
bool fetch_to_file(const std::string &, const std::string &, std::atomic<float> &, std::atomic<bool> &,
                   double, std::string &err) {
    err = "no HTTP support in this build";
    return false;
}
#endif

std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

std::string norm(const std::string &s) {
    std::string o;
    for (unsigned char ch : s)
        if (std::isalnum(ch) || ch >= 0x80) o += (char)std::tolower(ch);
    return o;
}

} // namespace

// ---- signal helpers ---------------------------------------------------------

// Silence (exact zeros from missing / muted frames, or near-silent audio) is
// cut down to 0.3 s per gap: Whisper tends to invent words on long silences,
// and it's wasted compute. 20 ms blocks; "silent" = peak below ~-54 dBFS.
std::vector<int16_t> squeeze_silence(const std::vector<int16_t> &pcm, double *kept_s) {
    const size_t B = 160;              // 20 ms at 8 kHz
    const int kMaxSilentBlocks = 15;   // 0.3 s
    std::vector<int16_t> out;
    out.reserve(pcm.size());
    int run = 0;
    for (size_t i = 0; i < pcm.size(); i += B) {
        const size_t e = std::min(pcm.size(), i + B);
        int pk = 0;
        for (size_t k = i; k < e; k++) pk = std::max(pk, std::abs((int)pcm[k]));
        if (pk < 64) { if (++run > kMaxSilentBlocks) continue; }
        else run = 0;
        out.insert(out.end(), pcm.begin() + (long)i, pcm.begin() + (long)e);
    }
    if (kept_s) *kept_s = out.size() / 8000.0;
    return out;
}

// 8 -> 16 kHz: zero-stuff and low-pass at 4 kHz (63-tap Blackman-windowed sinc).
std::vector<float> upsample_8k_to_16k(const std::vector<int16_t> &pcm) {
    static std::vector<float> h;
    const int N = 63, M = N / 2;
    if (h.empty()) {
        h.resize(N);
        for (int i = 0; i < N; i++) {
            const double x = i - M;
            const double sinc = x == 0 ? 0.5 : std::sin(kPi * 0.5 * x) / (kPi * x);
            const double w = 0.42 - 0.5 * std::cos(2 * kPi * i / (N - 1)) + 0.08 * std::cos(4 * kPi * i / (N - 1));
            h[i] = (float)(2.0 * sinc * w);   // x2: zero-stuffing halves the gain
        }
    }
    const size_t n = pcm.size() * 2;
    std::vector<float> up(n, 0.0f), out(n, 0.0f);
    for (size_t i = 0; i < pcm.size(); i++) up[2 * i] = pcm[i] / 32768.0f;
    for (size_t i = 0; i < n; i++) {
        float acc = 0;
        // only even input samples are non-zero
        for (int k = (int)((i + M) & 1); k < N; k += 2) {
            const long j = (long)i + M - k;
            if (j >= 0 && j < (long)n) acc += h[k] * up[(size_t)j];
        }
        out[i] = acc;
    }
    return out;
}

// Whisper sometimes loops on a phrase. Runs of the same 3-12 word sequence
// repeated 3+ times in a row are cut to one copy.
std::string collapse_repeats(const std::string &text) {
    std::vector<std::string> w, wn;
    {
        std::istringstream is(text);
        std::string t;
        while (is >> t) { w.push_back(t); wn.push_back(norm(t)); }
    }
    std::vector<std::string> out, outn;
    size_t i = 0;
    while (i < w.size()) {
        bool cut = false;
        for (size_t L = 3; L <= 12 && !cut; L++) {
            if (i + 3 * L > w.size()) break;
            size_t reps = 1;
            while (i + (reps + 1) * L <= w.size() &&
                   std::equal(wn.begin() + (long)i, wn.begin() + (long)(i + L), wn.begin() + (long)(i + reps * L)))
                reps++;
            if (reps >= 3) {
                for (size_t k = 0; k < L; k++) out.push_back(w[i + k]);
                i += reps * L;
                cut = true;
            }
        }
        if (!cut) out.push_back(w[i++]);
    }
    std::string s;
    for (size_t k = 0; k < out.size(); k++) { if (k) s += ' '; s += out[k]; }
    return s;
}

// ---- Transcriber -----------------------------------------------------------

Transcriber &Transcriber::instance() {
    static Transcriber t;
    return t;
}

void Transcriber::set_models_dir(const std::string &dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::lock_guard<std::mutex> lk(mtx_);
    dir_ = dir;
#ifdef HAS_WHISPER
    whisper_log_set(quiet_log, nullptr);
    if (!th_.joinable()) th_ = std::thread(&Transcriber::worker, this);
#endif
}

void Transcriber::set_vocabulary(const std::string &v) {
    std::lock_guard<std::mutex> lk(mtx_);
    vocab_ = v;
}

std::string Transcriber::model_path(int which) const { return dir_ + "/" + kFile[which]; }
double Transcriber::model_size_mb(int which) const { return kSizeMb[which]; }

ModelState Transcriber::model_state(int which) const {
    if (dl_state_[which].load() == 1) return ModelState::Downloading;
    std::error_code ec;
    if (std::filesystem::exists(model_path(which), ec) &&
        std::filesystem::file_size(model_path(which), ec) > (uintmax_t)(kSizeMb[which] * 0.5e6))
        return ModelState::Ready;
    return dl_state_[which].load() == 2 ? ModelState::Failed : ModelState::Missing;
}

float Transcriber::download_progress(int which) const { return dl_prog_[which].load(); }

void Transcriber::download(int which) {
    if (which == 0) download(2);   // the speech detector comes with the small model
    if (which < 0 || which > 2 || dl_state_[which].load() == 1 || model_state(which) == ModelState::Ready) return;
    if (dl_th_[which].joinable()) dl_th_[which].join();
    dl_state_[which].store(1);
    dl_prog_[which].store(0.f);
    dl_cancel_[which].store(false);
    dl_th_[which] = std::thread(&Transcriber::download_main, this, which);
}

void Transcriber::download_main(int which) {
    if (thread_init_) thread_init_();
    const std::string path = model_path(which), part = path + ".part";
    std::string err;
    log(std::string("Downloading speech model ") + kFile[which] + " (~" + std::to_string((int)kSizeMb[which]) + " MB)...");
    const bool ok = fetch_to_file(std::string(kUrl[which]) + kFile[which], part, dl_prog_[which], dl_cancel_[which],
                                  kSizeMb[which] * 1e6, err);
    std::error_code ec;
    if (ok) {
        std::filesystem::rename(part, path, ec);
        if (ec) { log("Speech model: couldn't rename " + part + ": " + ec.message()); dl_state_[which].store(2); return; }
        log(std::string("Speech model ready: ") + path);
        dl_state_[which].store(0);
        cv_.notify_all();
    } else {
        std::filesystem::remove(part, ec);
        log(std::string("Speech model download failed (") + kFile[which] + "): " + err);
        dl_state_[which].store(2);
    }
}

void Transcriber::submit(int id, std::vector<int16_t> pcm8k, std::string txt_path, Done done) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (q_.size() > 500) q_.pop_front();   // never grow without bound
        q_.push_back(Job{id, std::move(pcm8k), std::move(txt_path), std::move(done), nullptr});
    }
    cv_.notify_all();
}

void Transcriber::submit_loader(int id, Loader load, std::string txt_path, Done done) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (q_.size() > 500) q_.pop_front();
        q_.push_back(Job{id, {}, std::move(txt_path), std::move(done), std::move(load)});
    }
    cv_.notify_all();
}

int Transcriber::queued() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return (int)q_.size();
}

void Transcriber::shutdown() {
    stop_.store(true);
    for (int i = 0; i < 3; i++) dl_cancel_[i].store(true);
    cv_.notify_all();
    if (th_.joinable()) th_.join();
    for (int i = 0; i < 3; i++) if (dl_th_[i].joinable()) dl_th_[i].join();
#ifdef HAS_WHISPER
    for (int i = 0; i < 2; i++) if (ctx_[i]) { whisper_free((whisper_context *)ctx_[i]); ctx_[i] = nullptr; }
    if (vad_) { whisper_vad_free((whisper_vad_context *)vad_); vad_ = nullptr; }
#endif
}

#ifdef HAS_WHISPER
void Transcriber::worker() {
    if (thread_init_) thread_init_();
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait_for(lk, std::chrono::seconds(30), [this] {
                return stop_.load() || (enabled_.load() && !q_.empty() && model_state(0) == ModelState::Ready);
            });
            if (stop_.load()) return;
            // The large model holds ~1 GB: free it after 3 idle minutes.
            if (ctx_[1] && now_ms() - large_last_used_ms_ > 180000) {
                whisper_free((whisper_context *)ctx_[1]);
                ctx_[1] = nullptr;
            }
            if (!enabled_.load() || q_.empty() || model_state(0) != ModelState::Ready) continue;
            // Small model from an older install, no speech detector yet: fetch it.
            if (model_state(2) == ModelState::Missing) { lk.unlock(); download(2); lk.lock(); }
            job = std::move(q_.front());
            q_.pop_front();
        }
        busy_id_.store(job.id);
        if (job.load) job.pcm = job.load();
        Result r = run(job.pcm);
        busy_id_.store(-1);
        if (r.ok && !job.txt.empty() && !r.text.empty()) {
            if (FILE *f = std::fopen(job.txt.c_str(), "wb")) {
                std::string s = "[" + r.lang + ", whisper " + r.model + "]\n" + r.text + "\n";
                if (!r.text_en.empty()) s += "\n[English translation]\n" + r.text_en + "\n";
                std::fwrite(s.data(), 1, s.size(), f);
                std::fclose(f);
            }
        }
        if (job.done) job.done(job.id, r);
    }
}

namespace {
bool abort_cb(void *u) { return ((std::atomic<bool> *)u)->load(); }

// One whisper pass; returns the joined text (segments that are probably not
// speech are dropped, as are consecutive duplicate segments).
// Keep only what the speech detector marks as speech (0.1 s gaps between
// stretches). whisper.cpp can do this inside whisper_full, but it then runs
// the detector on 4 threads stepping through thousands of tiny graphs, which
// is very slow on a busy CPU; one thread is much faster here.
std::vector<float> speech_only(whisper_vad_context *v, const std::vector<float> &pcm16) {
    whisper_vad_params vp = whisper_vad_default_params();
    vp.threshold = 0.5f;
    vp.min_speech_duration_ms = 250;
    vp.min_silence_duration_ms = 500;
    vp.speech_pad_ms = 200;
    whisper_vad_segments *segs = whisper_vad_segments_from_samples(v, vp, pcm16.data(), (int)pcm16.size());
    if (!segs) return pcm16;   // detector failed: use everything
    std::vector<float> out;
    const int ns = whisper_vad_segments_n_segments(segs);
    for (int i = 0; i < ns; i++) {
        const long a = std::max(0L, (long)(whisper_vad_segments_get_segment_t0(segs, i) / 100.0 * 16000));
        const long b = std::min((long)pcm16.size(), (long)(whisper_vad_segments_get_segment_t1(segs, i) / 100.0 * 16000));
        if (b <= a) continue;
        if (!out.empty()) out.insert(out.end(), 1600, 0.0f);
        out.insert(out.end(), pcm16.begin() + a, pcm16.begin() + b);
    }
    whisper_vad_free_segments(segs);
    return out;
}

int compute_threads() {
    // Fewer threads than cores: ggml's threads spin-wait at barriers, which
    // goes badly when they're at low priority and the decoders are busy.
    const unsigned hw = std::thread::hardware_concurrency();
    return (int)std::clamp(hw / 2, 1u, 4u);
}

bool pass(whisper_context *ctx, const std::vector<float> &pcm16, const char *lang, bool translate,
          const std::string &prompt, std::atomic<bool> &stop, std::string &text, std::string &lang_out) {
    whisper_full_params p = whisper_full_default_params(WHISPER_SAMPLING_BEAM_SEARCH);
    p.beam_search.beam_size = 5;
    p.n_threads = compute_threads();
    p.language = lang;
    p.detect_language = false;
    p.translate = translate;
    p.no_context = true;            // don't condition on earlier text (fewer loops)
    p.suppress_blank = true;
    p.suppress_nst = true;
    p.no_speech_thold = 0.6f;
    p.initial_prompt = prompt.empty() ? nullptr : prompt.c_str();
    p.print_progress = p.print_realtime = p.print_timestamps = p.print_special = false;
    p.abort_callback = abort_cb;
    p.abort_callback_user_data = &stop;
    if (whisper_full(ctx, p, pcm16.data(), (int)pcm16.size()) != 0) return false;
    lang_out = whisper_lang_str(whisper_full_lang_id(ctx));
    std::string prev;
    text.clear();
    const int n = whisper_full_n_segments(ctx);
    for (int i = 0; i < n; i++) {
        const std::string seg = trim(whisper_full_get_segment_text(ctx, i));
        if (seg.empty() || whisper_full_get_segment_no_speech_prob(ctx, i) > 0.6f) continue;
        const std::string sn = norm(seg);
        if (sn.empty() || sn == prev) continue;
        prev = sn;
        if (!text.empty()) text += ' ';
        text += seg;
    }
    text = collapse_repeats(text);
    return true;
}
} // namespace

Result Transcriber::run(const std::vector<int16_t> &pcm8k) {
    Result r;
    const auto t0 = std::chrono::steady_clock::now();
    double kept = 0;
    const std::vector<int16_t> sq = squeeze_silence(pcm8k, &kept);
    r.speech_s = kept;
    if (kept < 1.0) { r.ok = true; return r; }   // nothing to transcribe
    std::vector<float> pcm16 = upsample_8k_to_16k(sq);
    std::string prompt;
    { std::lock_guard<std::mutex> lk(mtx_); prompt = vocab_; }
    if (!vad_ && model_state(2) == ModelState::Ready) {
        whisper_vad_context_params vcp = whisper_vad_default_context_params();
        vcp.n_threads = 1;
        vcp.use_gpu = false;
        vad_ = whisper_vad_init_from_file_with_params(model_path(2).c_str(), vcp);
        if (!vad_) log("Speech detector failed to load: " + model_path(2));
    }
    if (vad_) {
        pcm16 = speech_only((whisper_vad_context *)vad_, pcm16);
        r.speech_s = pcm16.size() / 16000.0;
        if (r.speech_s < 1.0) { r.ok = true; return r; }   // detector found no speech
    }

    auto load = [&](int which) -> whisper_context * {
        if (!ctx_[which] && model_state(which) == ModelState::Ready) {
            whisper_context_params cp = whisper_context_default_params();
            cp.use_gpu = false;
            ctx_[which] = whisper_init_from_file_with_params(model_path(which).c_str(), cp);
            if (!ctx_[which]) log(std::string("Speech model failed to load: ") + model_path(which));
        }
        return (whisper_context *)ctx_[which];
    };
    whisper_context *small = load(0);
    if (!small) { r.error = "small model not available"; return r; }
    whisper_context *large_all = large_all_.load() ? load(1) : nullptr;
    if (large_all) {
        // Language from the small model (first 30 s), then one pass on large.
        const int nt = compute_threads();
        int lid = -1;
        if (whisper_pcm_to_mel(small, pcm16.data(), (int)pcm16.size(), nt) == 0)
            lid = whisper_lang_auto_detect(small, 0, nt, nullptr);
        r.lang = lid >= 0 ? whisper_lang_str(lid) : "en";
        std::string l;
        if (!pass(large_all, pcm16, r.lang.c_str(), false, r.lang == "en" ? prompt : "", stop_, r.text, l)) {
            r.error = "whisper failed"; return r;
        }
        r.model = "large";
        if (translate_.load() && r.lang != "en") {
            std::string t;
            if (pass(large_all, pcm16, r.lang.c_str(), true, "", stop_, t, l)) r.text_en = t;
        }
        large_last_used_ms_ = now_ms();
        r.ok = !stop_.load();
        r.proc_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        return r;
    }
    if (!pass(small, pcm16, "auto", false, prompt, stop_, r.text, r.lang)) { r.error = "whisper failed"; return r; }
    r.model = "small";
    if (r.lang != "en") {
        // Vocabulary is English jargon: no prompt for other languages.
        if (whisper_context *large = load(1)) {
            std::string t, l;
            if (pass(large, pcm16, r.lang.c_str(), false, "", stop_, t, l)) { r.text = t; r.model = "large"; }
            if (translate_.load() && pass(large, pcm16, r.lang.c_str(), true, "", stop_, t, l)) r.text_en = t;
            large_last_used_ms_ = now_ms();
        } else if (translate_.load()) {
            std::string t, l;
            if (pass(small, pcm16, r.lang.c_str(), true, "", stop_, t, l)) r.text_en = t;
        }
    }
    r.ok = !stop_.load();
    r.proc_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return r;
}

#else  // !HAS_WHISPER: built without whisper.cpp

void Transcriber::worker() {}
Result Transcriber::run(const std::vector<int16_t> &) {
    Result r;
    r.error = "this build has no speech-to-text (whisper.cpp not found at build time)";
    return r;
}

#endif // HAS_WHISPER

} // namespace stt
