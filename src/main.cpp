// InmarScope - Inmarsat decoder
// Phase 1: RTL-SDR -> IQ ring -> FFT -> spectrum + scrolling waterfall.

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"

#include <GLFW/glfw3.h>

#include "dsp/iq_ring.h"
#include "dsp/jfft.h"
#include "sdr/rtl_sdr_source.h"
#include "sdr/hackrf_source.h"
#include "sdr/wav_file_source.h"
#include "sdr/sdrpp_server_source.h"
#include "decode/decoder_manager.h"
#include "output/message_feed.h"
#include "update/version_check.h"
#include "version.h"
#include "gui/waterfall.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
static bool openWavDialog(char* out, int outLen)
{
    char file[1024] = "";
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = "WAV / IQ files\0*.wav\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = sizeof(file);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.lpstrInitialDir = "D:\\";
    if (GetOpenFileNameA(&ofn))
    {
        std::strncpy(out, file, outLen - 1);
        out[outLen - 1] = 0;
        return true;
    }
    return false;
}
#else
static bool openWavDialog(char*, int) { return false; }
#endif

static void glfw_error_callback(int error, const char* description)
{
    std::fprintf(stderr, "GLFW error %d: %s\n", error, description);
}

// Per-stream spectrum/waterfall state. One per receive chain (primary + voice).
struct SpectrumView
{
    IqRing ring{1u << 21};
    JFFT fft;
    Waterfall waterfall;
    std::vector<std::complex<double>> iq;
    std::vector<double> window;
    std::vector<float> inst;    // instantaneous dB (shifted)
    std::vector<float> avg;     // averaged dB for the line plot
    std::vector<float> sortbuf; // scratch for percentile auto-scale
    std::vector<float> freqMHz;
    int curN = 0;
    float rmsDbfs = -120.0f;
    float frameDbMin = 0.0f, frameDbMax = -120.0f;
    double viewXminMHz = 0.0, viewXmaxMHz = 0.0;
    bool resetView = true;
    float specLeftInset = 0.0f, specRightInset = 0.0f;
};

struct App
{
    RtlSdrSource sdr;
    WavFileSource wav;
    SdrppServerSource server;
    HackRfSource hack;
    SdrSource* active = &sdr;   // currently selected/running source
    int  sourceMode = 0;        // 0 = RTL-SDR, 1 = WAV, 2 = SDR++ Server, 3 = HackRF
    char wavPath[512] = "";
    bool wavLoop = true;
    char serverHost[128] = "localhost";
    int  serverPort = 5259;
    bool serverCompression = true;
    int  serverSampleType = 1;  // 0=int8, 1=int16, 2=float
    double serverSampleRateMHz = 2.0; // rate requested from the server

    // HackRF (native) settings.
    double hackSampleRateMHz = 10.0;
    int    hackLna = 16;   // IF gain 0..40 (8 dB steps)
    int    hackVga = 16;   // baseband gain 0..62 (2 dB steps)
    bool   hackAmp = false; // ~11 dB RF amp
    bool   hackBias = false; // antenna/port power (bias tee)

    SpectrumView viewA;       // primary (P-channel) display
    SpectrumView viewB;       // voice SDR display (dual-SDR mode)
    DecoderManager decoders;  // primary decoders (SDR A)
    DecoderManager decodersB; // voice decoders (SDR B)
    RtlSdrSource sdrB;        // second RTL for dedicated voice follow

    // Dual-SDR voice follow (second RTL).
    bool   voiceSdrEnabled = false; // user enabled the 2nd RTL voice chain
    bool   dualMode = false;        // both A and B running
    int    deviceIndexB = 1;        // 2nd RTL device index
    double voiceCenterMHz = 1545.0; // SDR B park center (last voice heard)
    int    sampleRateIdxB = 0;      // index into kRates (low rate ok for voice)
    bool   autoGainB = false;
    float  gainDbB = 40.0f;
    bool   biasTeeB = false;
    float  ppmB = 0.0f;
    int newBaud = 1; // 0 = 600, 1 = 1200 (baud for click-added decoders)
    int selectedDecoder = -1;     // channelId shown in the constellation panel
    std::vector<float> constBuf;  // interleaved I,Q scratch for the plot
    double constLim = 1.0;        // cached constellation axis limit (held steady)
    std::chrono::steady_clock::time_point constLimTime; // last time the scale updated

    // Voice call recording (8400). Saves every decoded call to its own WAV.
    bool recordVoice = false;
    char recordDir[256] = "recordings";

    // Audio output device selection (index into audioDevs; 0 = system default).
    int audioDevice = 0;
    std::vector<std::string> audioDevs;

    // Message output feed (JSON / JAERO text -> file and/or UDP).
    MessageFeed feed;
    VersionCheck verCheck; // background update check against the server
    uint64_t lastAcarsFed = 0, lastEgcFed = 0;
    bool   outFile = false;
    char   outFilePath[512] = "messages.jsonl";
    bool   outUdp = false;
    char   outUdpHost[128] = "127.0.0.1";
    int    outUdpPort = 5556;
    int    outFormat = 0; // 0 = JSON, 1 = JAERO text
    char   outStation[64] = "";
    bool   outSbs = false; // SBS/BaseStation position feed over UDP
    char   outSbsHost[128] = "127.0.0.1";
    int    outSbsPort = 30003;

    // Voice follow: when a C-channel voice assignment appears, hop the SDR to
    // the assigned RX (forward) frequency, decode the 8400 voice call, then hop
    // back to the P-channel home when the call goes idle.
    bool   voiceFollow = false;
    float  followHoldSec = 6.0f;        // idle time before ending a follow
    bool   following = false;           // a follow is currently active
    bool   followRetuned = false;       // true if we had to move the SDR center
    bool   followEverLocked = false;    // the voice decoder has produced audio at least once
    int    followChannelId = -1;        // the spawned 8400 voice decoder
    double followHomeMHz = 0.0;         // P-channel center to return to
    uint64_t followSeenCount = 0;       // cassign entries already considered
    uint64_t followVoiceFrames = 0;     // last observed decoded-audio frame count
    std::vector<std::pair<double, int>> followHome; // home decoders (MHz, baud)
    std::chrono::steady_clock::time_point followActivity;

    // Device list.
    std::vector<SdrDeviceInfo> devices;
    int deviceIndex = 0;

    // Tuning / radio settings.
    double centerFreqMHz = 1545.0;
    int    sampleRateIdx = 9;  // index into kRates
    bool   autoGain = false;
    float  gainDb = 40.0f;
    bool   biasTee = false;
    float  ppm = 0.0f;

    // FFT / display settings (shared by both spectrum views).
    int   fftSizeIdx = 2; // index into kFftSizes
    float avgAlpha = 0.6f;
    float dbMin = -80.0f;
    float dbMax = 0.0f;
    bool  dcBlock = true;      // gentle DC blocker in the source path
    bool  autoScale = true;

    std::string status = "Idle";

    // Band browsing: panning/scrolling the spectrum past the captured window
    // retunes a live SDR so you can sweep the whole band (not for WAV files).
    bool   bandBrowse = true;
    std::chrono::steady_clock::time_point lastRetune;
    double lastRetuneCtr = 0.0; // view center at the last band-browse retune
    // Band-browse follow tuning (percentages are of the sample rate).
    float  browseEdgePct = 24.5f;    // recenter when view edge is within this % of band edge
    float  browseThrottleMs = 20.0f; // minimum time between retunes
    float  browseMinMovePct = 0.10f; // minimum view movement before retuning
    bool   acPosOnly = false;        // Aircraft panel: show only entries with a position
    bool   showEmptyMsgs = false;    // Messages panel: show ACARS msgs with no text/decoded body
    // Dock layout versioning: when the built-in default layout changes we bump
    // kLayoutVersion so saved older layouts get replaced by the new default.
    int    layoutVersion = 0;        // persisted; compared to kLayoutVersion
    bool   forceDefaultLayout = false; // request drawDockHost to rebuild the default

    // Last sample rate the decoder manager was configured with; if the source
    // rate changes (e.g. SDR++ server rate switch), the manager + FFT are
    // reconfigured to match.
    double lastConfiguredFs = 0.0;
};

static const double kRates[] = {
    0.25e6, 0.9e6, 1.024e6, 1.2e6, 1.4e6, 1.536e6,
    1.8e6, 1.92e6, 2.048e6, 2.4e6, 2.56e6, 2.88e6, 3.2e6};
static const char* kRateLabels[] = {
    "0.25", "0.9", "1.024", "1.2", "1.4", "1.536",
    "1.8", "1.92", "2.048", "2.4", "2.56", "2.88", "3.2"};
static const int kNumRates = (int)(sizeof(kRates) / sizeof(kRates[0]));

static const int kFftSizes[] = {1024, 2048, 4096, 8192, 16384, 32768, 65536};
static const char* kFftLabels[] = {"1024", "2048", "4096", "8192", "16384", "32768", "65536"};
static const int kNumFftSizes = (int)(sizeof(kFftSizes) / sizeof(kFftSizes[0]));

static void buildWindow(SpectrumView& v, int N, float initDb)
{
    // Blackman window: low sidelobes -> clean noise floor, signals stand out.
    v.window.resize(N);
    for (int i = 0; i < N; ++i)
    {
        double x = 2.0 * M_PI * i / (N - 1);
        v.window[i] = 0.42 - 0.5 * std::cos(x) + 0.08 * std::cos(2.0 * x);
    }
    int nf = N; // init() builds the twiddle tables; must run before fft().
    v.fft.init(nf);
    v.iq.resize(N);
    v.inst.assign(N, initDb);
    v.avg.assign(N, initDb);
    v.freqMHz.resize(N);
    v.curN = N;
}

static void updateFreqAxis(SpectrumView& v, double fc, double fs, int N)
{
    for (int i = 0; i < N; ++i)
        v.freqMHz[i] = (float)((fc + (i - N / 2) * fs / N) / 1e6);
}

// After DC removal the exact center (DC) bin is a deep null. Cover the few
// center bins with a copy of adjacent bins so it doesn't show as a hole.
static void patchDcBins(std::vector<float>& a, int N, int w)
{
    int c = N / 2;
    int span = 2 * w + 1;
    int src = c - w - span; // equal-width band just left of the gap
    if (src < 0 || c + w >= N)
        return;
    for (int k = 0; k < span; ++k)
        a[c - w + k] = a[src + k];
}

static void processFft(SpectrumView& v, App& app, double fc, double fs)
{
    int N = kFftSizes[app.fftSizeIdx];
    if (N != v.curN)
        buildWindow(v, N, app.dbMin);

    if (!v.ring.latest(v.iq.data(), (size_t)N))
        return;

    double pwr = 0.0;
    for (int i = 0; i < N; ++i)
        pwr += std::norm(v.iq[i]);
    double rms = std::sqrt(pwr / (double)N);
    v.rmsDbfs = (float)(20.0 * std::log10(rms + 1e-12));

    for (int i = 0; i < N; ++i)
        v.iq[i] *= v.window[i];

    v.fft.fft(v.iq.data(), N, JFFT::FORWARD);

    const double invN = 1.0 / (double)N;
    const float alpha = app.avgAlpha;
    for (int i = 0; i < N; ++i)
    {
        int src = (i + N / 2) % N; // fftshift: center DC
        double p = std::norm(v.iq[src]) * invN; // power spectrum
        float db = (float)(10.0 * std::log10(p + 1e-20));
        v.inst[i] = db;
        v.avg[i] = alpha * v.avg[i] + (1.0f - alpha) * db;
    }

    patchDcBins(v.inst, N, 4);
    patchDcBins(v.avg, N, 4);

    v.sortbuf.assign(v.inst.begin(), v.inst.end());
    int iFloor = (int)(0.30 * N);
    int iPeak = (int)(0.995 * N);
    std::nth_element(v.sortbuf.begin(), v.sortbuf.begin() + iFloor, v.sortbuf.end());
    float floorDb = v.sortbuf[iFloor];
    std::nth_element(v.sortbuf.begin(), v.sortbuf.begin() + iPeak, v.sortbuf.end());
    float peakDb = v.sortbuf[iPeak];
    v.frameDbMin = floorDb;
    v.frameDbMax = peakDb;
    if (app.autoScale)
    {
        float tgtMin = floorDb - 6.0f;
        float tgtMax = std::max(peakDb + 12.0f, floorDb + 20.0f);
        app.dbMin = 0.85f * app.dbMin + 0.15f * tgtMin;
        app.dbMax = 0.85f * app.dbMax + 0.15f * tgtMax;
    }

    updateFreqAxis(v, fc, fs, N);
    v.waterfall.addRow(v.inst.data(), N, app.dbMin, app.dbMax);
}

// C-channel assignment types that carry a voice call (0x31 distress .. 0x34
// non-safety). All of these point at a forward-link RX voice carrier.
static bool isVoiceAssign(uint8_t type)
{
    return type >= 0x31 && type <= 0x34;
}

// Retune the active (live) source to a new center and re-point the decoder
// manager there. Wipes all decoders -- callers restore what they need.
static void retuneActive(App& app, double centerMHz)
{
    double hz = centerMHz * 1e6;
    app.centerFreqMHz = centerMHz;
    app.viewA.resetView = true;
    if (app.sourceMode == 0)
        app.sdr.setCenterFreq(hz);
    else if (app.sourceMode == 2)
        app.server.setCenterFreq(hz);
    else if (app.sourceMode == 3)
        app.hack.setCenterFreq(hz);
    app.decoders.removeAll();
    app.decoders.configure(app.active->sampleRate(), hz);
    // Rebuild the frequency axis now (processFft already ran this frame with the
    // old center) so drawSpectrum fits the view to the NEW band and the decoder
    // marker stays on screen after a big follow jump.
    if (app.viewA.curN > 0)
        updateFreqAxis(app.viewA, hz, app.active->sampleRate(), app.viewA.curN);
}

// Retune a live source to a new center while keeping the current decoders
// (re-added at their same absolute frequencies) and the user's spectrum view.
// Used for band browsing, where we sweep the radio as the view is panned.
static void retunePreserving(App& app, double centerMHz)
{
    std::vector<std::pair<double, int>> keep;
    for (auto& s : app.decoders.status())
        keep.push_back({s.freqMHz, s.baud});

    double hz = centerMHz * 1e6;
    app.centerFreqMHz = centerMHz;
    if (app.sourceMode == 0)
        app.sdr.setCenterFreq(hz);
    else if (app.sourceMode == 2)
        app.server.setCenterFreq(hz);
    else if (app.sourceMode == 3)
        app.hack.setCenterFreq(hz);
    app.decoders.removeAll();
    app.decoders.configure(app.active->sampleRate(), hz);
    for (auto& k : keep)
        app.decoders.addDecoder(k.first * 1e6, k.second);
}

// If the source's sample rate changes (e.g. an SDR++ server rate switch),
// reconfigure the decoder manager + FFT axis to match, keeping the decoders.
static void updateRateChange(App& app)
{
    if (!app.active->running() || app.following)
        return;
    double fs = app.active->sampleRate();
    if (fs <= 1.0 || std::fabs(fs - app.lastConfiguredFs) < 1.0)
        return;

    std::vector<std::pair<double, int>> keep;
    for (auto& s : app.decoders.status())
        keep.push_back({s.freqMHz, s.baud});
    double center = app.active->centerFreq();
    app.decoders.removeAll();
    app.decoders.configure(fs, center);
    for (auto& k : keep)
        app.decoders.addDecoder(k.first * 1e6, k.second);
    app.lastConfiguredFs = fs;
    app.viewA.resetView = true;
    if (app.viewA.curN > 0)
        updateFreqAxis(app.viewA, center, fs, app.viewA.curN);
}

// Drives the voice-follow state machine once per frame while a source runs.
static void updateVoiceFollow(App& app)
{
    using clock = std::chrono::steady_clock;

    // Dual-SDR path: SDR B does voice, SDR A keeps decoding the P-channel.
    if (app.dualMode)
    {
        const uint64_t total = app.decoders.cassignLog().count();
        if (!app.following)
        {
            if (!app.voiceFollow) { app.followSeenCount = total; return; }
            if (total <= app.followSeenCount) return;
            auto items = app.decoders.cassignLog().snapshot();
            const CassignEntry* pick = nullptr;
            for (auto it = items.rbegin(); it != items.rend(); ++it)
                if (isVoiceAssign(it->type) && it->rxMHz > 1.0) { pick = &*it; break; }
            app.followSeenCount = total;
            if (!pick) return;
            double rx = pick->rxMHz;
            // Offset SDR B's center off the voice carrier so it avoids the RTL
            // DC spike (centering exactly on it gives a bad constellation / no
            // audio). The decoder still sits at the absolute rx frequency.
            double fsBMHz = app.sdrB.sampleRate() / 1e6;
            double offB = std::min(0.2, 0.25 * fsBMHz);
            double bctr = rx - offB;
            app.sdrB.setCenterFreq(bctr * 1e6);
            app.decodersB.removeAll();
            app.decodersB.configure(app.sdrB.sampleRate(), bctr * 1e6);
            app.viewB.resetView = true;
            if (app.viewB.curN > 0)
                updateFreqAxis(app.viewB, bctr * 1e6, app.sdrB.sampleRate(), app.viewB.curN);
            app.followChannelId = app.decodersB.addDecoder(rx * 1e6, 8400);
            if (app.followChannelId < 0) return;
            app.decodersB.setVoiceMonitor(app.followChannelId);
            app.voiceCenterMHz = bctr; // park B here when the call ends
            app.following = true;
            app.followRetuned = true;
            app.followEverLocked = false;
            app.followVoiceFrames = 0;
            app.followActivity = clock::now();
            char buf[64];
            std::snprintf(buf, sizeof(buf), "Voice SDR following %.4f MHz", rx);
            app.status = buf;
            return;
        }
        // End the follow based on decoded audio, not lock: an 8400 carrier can
        // stay "locked" on idle after a call, so revert when no AMBE voice frames
        // have been decoded for the hold time (longer grace before the first one).
        constexpr double kAcquireSec = 12.0;
        const auto now = clock::now();
        uint64_t vf = app.decodersB.voiceFrames(app.followChannelId);
        if (vf > app.followVoiceFrames)
        {
            app.followVoiceFrames = vf;
            app.followActivity = now;
            app.followEverLocked = true; // "ever had audio"
        }
        double grace = app.followEverLocked ? (double)app.followHoldSec
                                            : std::max((double)app.followHoldSec, kAcquireSec);
        double idle = std::chrono::duration<double>(now - app.followActivity).count();
        if (idle > grace || !app.voiceFollow)
        {
            app.decodersB.removeDecoder(app.followChannelId);
            app.followChannelId = -1;
            app.following = false;
            app.status = "Running (dual SDR)";
            // SDR B stays parked + streaming at voiceCenterMHz for its waterfall.
        }
        return;
    }

    const bool canRetune = (app.sourceMode != 1); // WAV center is just a label
    const uint64_t total = app.decoders.cassignLog().count();

    if (!app.following)
    {
        if (!app.voiceFollow)
        {
            app.followSeenCount = total; // stay in sync while disabled
            return;
        }
        if (total <= app.followSeenCount)
            return; // no new assignment since we last looked

        // Pick the most recent voice assignment with a usable RX frequency.
        auto items = app.decoders.cassignLog().snapshot();
        const CassignEntry* pick = nullptr;
        for (auto it = items.rbegin(); it != items.rend(); ++it)
            if (isVoiceAssign(it->type) && it->rxMHz > 1.0)
            {
                pick = &*it;
                break;
            }
        app.followSeenCount = total;
        if (!pick)
            return;

        const double rx = pick->rxMHz;
        const double centerMHz = app.active->centerFreq() / 1e6;
        const double halfMHz = (app.active->sampleRate() / 1e6) * 0.45;
        const bool inBand = std::fabs(rx - centerMHz) <= halfMHz;

        app.followHomeMHz = app.centerFreqMHz;
        app.followRetuned = false;

        if (!inBand)
        {
            if (!canRetune)
                return; // out of band and can't move the SDR -> skip
            app.followHome.clear();
            for (auto& s : app.decoders.status())
                app.followHome.push_back({s.freqMHz, s.baud});
            // Don't center the SDR exactly on the voice carrier: that puts it on
            // the RTL DC spike (kills the OQPSK demod). Offset the center so the
            // signal sits clear of DC, like a manual decode does.
            double fsMHz = app.active->sampleRate() / 1e6;
            double off = std::min(0.2, 0.25 * fsMHz);
            retuneActive(app, rx - off);
            app.followRetuned = true;
        }

        app.followChannelId = app.decoders.addDecoder(rx * 1e6, 8400);
        if (app.followChannelId < 0)
            return; // failed to spawn (e.g. manager not configured)
        app.decoders.setVoiceMonitor(app.followChannelId);
        app.selectedDecoder = app.followChannelId;
        app.following = true;
        app.followEverLocked = false;
        app.followVoiceFrames = 0;
        app.followActivity = clock::now();
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Following voice %.4f MHz", rx);
        app.status = buf;
        return;
    }

    // A follow is active. End it based on decoded audio rather than lock: the
    // 8400 carrier can stay "locked" on idle/noise after the call ends, which
    // would strand the SDR away from home. Instead, revert when no AMBE voice
    // frames have been decoded for the hold time. A longer grace applies before
    // the first frame so we don't bail during acquisition.
    constexpr double kAcquireSec = 12.0;
    const auto now = clock::now();
    uint64_t vf = app.decoders.voiceFrames(app.followChannelId);
    if (vf > app.followVoiceFrames)
    {
        app.followVoiceFrames = vf;
        app.followActivity = now;
        app.followEverLocked = true; // "ever had audio"
    }

    double grace = app.followEverLocked
                       ? (double)app.followHoldSec
                       : std::max((double)app.followHoldSec, kAcquireSec);
    double idle = std::chrono::duration<double>(now - app.followActivity).count();

    if (idle > grace || !app.voiceFollow)
    {
        app.decoders.removeDecoder(app.followChannelId);
        app.followChannelId = -1;
        if (app.followRetuned)
        {
            retuneActive(app, app.followHomeMHz);
            for (auto& h : app.followHome)
                app.decoders.addDecoder(h.first * 1e6, h.second);
            app.followHome.clear();
        }
        app.following = false;
        app.status = "Running";
    }
}

// Push the UI's output settings into the feed (idempotent; guarded internally)
// and forward any newly-logged ACARS/EGC messages to it.
static void updateFeed(App& app)
{
    app.feed.setFormat(app.outFormat);
    app.feed.setStationId(app.outStation);
    app.feed.setFileEnabled(app.outFile, app.outFilePath);
    app.feed.setUdpEnabled(app.outUdp, app.outUdpHost, app.outUdpPort);
    app.feed.setSbsEnabled(app.outSbs, app.outSbsPort);
    app.feed.pollSbs();

    auto& alog = app.decoders.log();
    uint64_t at = alog.count();
    if (at > app.lastAcarsFed)
    {
        auto snap = alog.snapshot();
        uint64_t newN = at - app.lastAcarsFed;
        if (newN > snap.size()) newN = snap.size();
        for (size_t i = snap.size() - (size_t)newN; i < snap.size(); ++i)
            app.feed.feedAcars(snap[i]);
        app.lastAcarsFed = at;
    }
    auto& elog = app.decoders.egcLog();
    uint64_t et = elog.count();
    if (et > app.lastEgcFed)
    {
        auto snap = elog.snapshot();
        uint64_t newN = et - app.lastEgcFed;
        if (newN > snap.size()) newN = snap.size();
        for (size_t i = snap.size() - (size_t)newN; i < snap.size(); ++i)
            app.feed.feedEgc(snap[i]);
        app.lastEgcFed = et;
    }
}

static void startActive(App& app)
{
    app.viewA.ring.clear();
    app.viewA.waterfall.clear();
    app.viewA.resetView = true;

    IqRing* ring = &app.viewA.ring;
    DecoderManager* mgr = &app.decoders;
    auto cb = [ring, mgr](const float* iq, int n) {
        ring->push(iq, (size_t)n);
        mgr->feed(iq, n);
    };
    std::string err;
    bool ok = false;

    if (app.sourceMode == 0)
    {
        app.active = &app.sdr;
        app.sdr.setSampleRate(kRates[app.sampleRateIdx]);
        app.sdr.setCenterFreq(app.centerFreqMHz * 1e6);
        app.sdr.setGain(app.autoGain ? -1.0 : (double)app.gainDb);
        app.sdr.setBiasTee(app.biasTee);
        app.sdr.setPpm((double)app.ppm);
        app.sdr.setDcBlock(app.dcBlock);
        ok = app.sdr.start(app.deviceIndex, cb, err);
    }
    else if (app.sourceMode == 1)
    {
        app.active = &app.wav;
        app.wav.setPath(app.wavPath);
        app.wav.setLoop(app.wavLoop);
        app.wav.setCenterFreq(app.centerFreqMHz * 1e6);
        ok = app.wav.start(0, cb, err);
    }
    else if (app.sourceMode == 2)
    {
        app.active = &app.server;
        app.server.setHost(app.serverHost);
        app.server.setPort((uint16_t)app.serverPort);
        app.server.setCompressionEnabled(app.serverCompression);
        app.server.setSampleTypeIndex(app.serverSampleType);
        app.server.setCenterFreq(app.centerFreqMHz * 1e6);
        ok = app.server.start(0, cb, err);
        if (ok)
        {
            // The server reports its sample rate asynchronously after START;
            // wait briefly so the decoders are configured at the right rate.
            for (int i = 0; i < 40; ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                if (app.server.sampleRate() > 1.0)
                    break;
            }
        }
    }
    else
    {
        app.active = &app.hack;
        app.hack.setSampleRate(app.hackSampleRateMHz * 1e6);
        app.hack.setCenterFreq(app.centerFreqMHz * 1e6);
        app.hack.setLnaGain(app.hackLna);
        app.hack.setVgaGain(app.hackVga);
        app.hack.setAmpEnable(app.hackAmp);
        app.hack.setBiasTee(app.hackBias);
        app.hack.setDcBlock(app.dcBlock);
        ok = app.hack.start(app.deviceIndex, cb, err);
    }

    if (ok)
    {
        // Optional dedicated voice SDR (2nd RTL). Start it first so we know
        // whether to run in dual mode before configuring the primary's audio.
        bool startedB = false;
        if (app.voiceSdrEnabled && app.deviceIndexB != app.deviceIndex)
        {
            app.viewB.ring.clear();
            app.viewB.waterfall.clear();
            app.viewB.resetView = true;
            IqRing* ringB = &app.viewB.ring;
            DecoderManager* mgrB = &app.decodersB;
            auto cbB = [ringB, mgrB](const float* iq, int n) {
                ringB->push(iq, (size_t)n);
                mgrB->feed(iq, n);
            };
            app.sdrB.setSampleRate(kRates[app.sampleRateIdxB]);
            app.sdrB.setCenterFreq(app.voiceCenterMHz * 1e6);
            app.sdrB.setGain(app.autoGainB ? -1.0 : (double)app.gainDbB);
            app.sdrB.setBiasTee(app.biasTeeB);
            app.sdrB.setPpm((double)app.ppmB);
            app.sdrB.setDcBlock(app.dcBlock);
            std::string errB;
            startedB = app.sdrB.start(app.deviceIndexB, cbB, errB);
            if (startedB)
            {
                app.decodersB.removeAll();
                app.decodersB.configure(app.sdrB.sampleRate(), app.sdrB.centerFreq());
                app.decodersB.setAudioEnabled(true);   // voice audio comes from B
                app.decodersB.setMaxWorkers(2);
                app.decodersB.setRecording(app.recordVoice, app.recordDir);
                app.decodersB.start();
            }
            else
                app.status = "Voice SDR error: " + errB;
        }
        app.dualMode = startedB;

        app.decoders.removeAll();
        app.decoders.configure(app.active->sampleRate(), app.active->centerFreq());
        app.decoders.setAudioEnabled(!app.dualMode); // A is silent in dual mode
        app.decoders.start();
        app.lastConfiguredFs = app.active->sampleRate();
        // Don't auto-follow assignments left over from a previous session.
        app.followSeenCount = app.decoders.cassignLog().count();
        app.following = false;
        app.followChannelId = -1;
        app.followHome.clear();
    }

    if (ok)
        app.status = app.dualMode ? "Running (dual SDR)" : "Running";
    else
        app.status = "Error: " + err;
}

static void drawControls(App& app)
{
    ImGui::Begin("Control");

    bool running = app.active->running();

    ImGui::BeginDisabled(running);
    const char* modes[] = {"RTL-SDR", "WAV file", "SDR++ Server", "HackRF"};
    ImGui::Combo("Source", &app.sourceMode, modes, 4);
    ImGui::EndDisabled();

    ImGui::Separator();

    if (!running)
    {
        bool canStart = (app.sourceMode == 1) ? (app.wavPath[0] != '\0') : true;
        ImGui::BeginDisabled(!canStart);
        if (ImGui::Button("Start", ImVec2(120, 0)))
            startActive(app);
        ImGui::EndDisabled();
    }
    else
    {
        if (ImGui::Button("Stop", ImVec2(120, 0)))
        {
            app.active->stop();
            app.decoders.stop();
            app.decoders.removeAll();
            if (app.dualMode)
            {
                app.sdrB.stop();
                app.decodersB.stop();
                app.decodersB.removeAll();
            }
            app.dualMode = false;
            app.following = false;
            app.followChannelId = -1;
            app.followHome.clear();
            app.status = "Idle";
        }
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(app.status.c_str());

    // Version + update banner.
    ImGui::TextDisabled("InmarScope v" INMARSCOPE_VERSION);
    {
        VersionCheck::State st = app.verCheck.state();
        if (st == VersionCheck::UpdateAvailable)
        {
            std::string latest = app.verCheck.latestVersion();
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "  Update available: v%s",
                               latest.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Get update"))
            {
#if defined(_WIN32)
                std::string url = app.verCheck.productUrl();
                if (url.empty()) url = "https://sarahsforge.dev/products/inmarscope";
                ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#endif
            }
        }
        else if (st == VersionCheck::UpToDate)
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "  (up to date)");
        }
        else if (st == VersionCheck::Checking)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("  checking for updates...");
        }
    }

    ImGui::Separator();

    if (app.sourceMode == 0)
    {
        // ---- RTL-SDR ----
        if (ImGui::Button("Refresh devices"))
            app.devices = app.sdr.listDevices();
        ImGui::SameLine();
        ImGui::Text("(%d found)", (int)app.devices.size());

        if (!app.devices.empty())
        {
            std::string preview = app.devices[std::min(app.deviceIndex, (int)app.devices.size() - 1)].name;
            if (ImGui::BeginCombo("Device", preview.c_str()))
            {
                for (int i = 0; i < (int)app.devices.size(); ++i)
                {
                    bool sel = (app.deviceIndex == i);
                    std::string label = std::to_string(i) + ": " + app.devices[i].name +
                                        " [" + app.devices[i].serial + "]";
                    if (ImGui::Selectable(label.c_str(), sel))
                        app.deviceIndex = i;
                }
                ImGui::EndCombo();
            }
        }
        else
        {
            ImGui::TextDisabled("No RTL-SDR devices. Click Refresh.");
        }

        if (ImGui::InputDouble("Center (MHz)", &app.centerFreqMHz, 0.1, 1.0, "%.4f"))
        {
            app.viewA.resetView = true;
            if (running)
                app.sdr.setCenterFreq(app.centerFreqMHz * 1e6);
        }
        if (ImGui::Combo("Sample rate (MHz)", &app.sampleRateIdx, kRateLabels, kNumRates))
        {
            app.viewA.resetView = true;
            if (running)
                app.sdr.setSampleRate(kRates[app.sampleRateIdx]);
        }
        if (ImGui::Checkbox("Auto gain (AGC)", &app.autoGain))
        {
            if (running)
                app.sdr.setGain(app.autoGain ? -1.0 : (double)app.gainDb);
        }
        if (!app.autoGain)
        {
            if (ImGui::SliderFloat("Gain (dB)", &app.gainDb, 0.0f, 50.0f, "%.1f"))
            {
                if (running)
                    app.sdr.setGain((double)app.gainDb);
            }
        }
        if (ImGui::Checkbox("Bias-T", &app.biasTee))
        {
            if (running)
                app.sdr.setBiasTee(app.biasTee);
        }
        if (ImGui::InputFloat("PPM", &app.ppm, 0.1f, 1.0f, "%.2f"))
        {
            if (running)
                app.sdr.setPpm((double)app.ppm);
        }
        if (ImGui::Checkbox("DC block", &app.dcBlock))
        {
            if (running)
                app.sdr.setDcBlock(app.dcBlock);
        }
    }
    else if (app.sourceMode == 1)
    {
        // ---- WAV file ----
        ImGui::SetNextItemWidth(-90.0f);
        ImGui::InputText("##wavpath", app.wavPath, sizeof(app.wavPath));
        ImGui::SameLine();
        if (ImGui::Button("Browse..."))
            openWavDialog(app.wavPath, sizeof(app.wavPath));

        if (ImGui::Checkbox("Loop", &app.wavLoop))
        {
            if (running)
                app.wav.setLoop(app.wavLoop);
        }
        if (ImGui::InputDouble("Center label (MHz)", &app.centerFreqMHz, 0.1, 1.0, "%.4f"))
            app.viewA.resetView = true;

        if (running)
        {
            ImGui::ProgressBar((float)app.wav.progress(), ImVec2(-1, 0));
            ImGui::Text("WAV: %d ch, %d-bit, %.1f kHz",
                        app.wav.channels(), app.wav.bits(), app.wav.sampleRate() / 1e3);
        }
    }
    else if (app.sourceMode == 2)
    {
        // ---- SDR++ Server ----
        ImGui::BeginDisabled(running);
        ImGui::SetNextItemWidth(-60.0f);
        ImGui::InputText("Host", app.serverHost, sizeof(app.serverHost));
        ImGui::InputInt("Port", &app.serverPort);
        const char* sampleTypes[] = {"int8 (low BW)", "int16", "float32 (high BW)"};
        ImGui::Combo("Sample type", &app.serverSampleType, sampleTypes, 3);
        ImGui::Checkbox("Compression (zstd)", &app.serverCompression);
        ImGui::EndDisabled();

        if (ImGui::InputDouble("Center (MHz)", &app.centerFreqMHz, 0.1, 1.0, "%.4f"))
        {
            app.viewA.resetView = true;
            if (running)
                app.server.setCenterFreq(app.centerFreqMHz * 1e6);
        }

        // Sample-rate combo, populated from the server's source-module UI once
        // connected. Selecting one drives the server's rate via a UI action.
        if (running)
        {
            auto labels = app.server.sampleRateLabels();
            auto values = app.server.sampleRateValues();
            int curIdx = app.server.currentSampleRateIndex();
            if (!labels.empty())
            {
                const char* preview = (curIdx >= 0 && curIdx < (int)labels.size())
                                          ? labels[curIdx].c_str()
                                          : "(select)";
                if (ImGui::BeginCombo("Sample rate", preview))
                {
                    for (int i = 0; i < (int)labels.size(); ++i)
                    {
                        bool sel = (i == curIdx);
                        if (ImGui::Selectable(labels[i].c_str(), sel) && i < (int)values.size())
                        {
                            app.server.setSampleRate(values[i]);
                            app.viewA.resetView = true;
                        }
                    }
                    ImGui::EndCombo();
                }
            }
            else
            {
                ImGui::TextDisabled("Sample rate: (no rate control exposed by server)");
            }
            ImGui::Text("Server sample rate: %.4f MHz", app.server.sampleRate() / 1e6);
        }
        else
        {
            ImGui::TextDisabled("Connect to list the server's sample rates.");
        }
        ImGui::TextDisabled("Gain and device are configured on the SDR++ server.");
    }
    else
    {
        // ---- HackRF (native) ----
        if (ImGui::Button("Refresh devices"))
            app.devices = app.hack.listDevices();
        ImGui::SameLine();
        ImGui::Text("(%d found)", (int)app.devices.size());
        if (!app.devices.empty())
        {
            std::string preview = app.devices[std::min(app.deviceIndex, (int)app.devices.size() - 1)].name;
            if (ImGui::BeginCombo("Device", preview.c_str()))
            {
                for (int i = 0; i < (int)app.devices.size(); ++i)
                {
                    bool sel = (app.deviceIndex == i);
                    std::string label = std::to_string(i) + ": " + app.devices[i].name +
                                        " [" + app.devices[i].serial + "]";
                    if (ImGui::Selectable(label.c_str(), sel))
                        app.deviceIndex = i;
                }
                ImGui::EndCombo();
            }
        }

        if (ImGui::InputDouble("Center (MHz)", &app.centerFreqMHz, 0.1, 1.0, "%.4f"))
        {
            app.viewA.resetView = true;
            if (running)
                app.hack.setCenterFreq(app.centerFreqMHz * 1e6);
        }
        if (ImGui::InputDouble("Sample rate (MHz)", &app.hackSampleRateMHz, 1.0, 2.0, "%.3f"))
        {
            if (app.hackSampleRateMHz < 2.0) app.hackSampleRateMHz = 2.0;
            if (app.hackSampleRateMHz > 20.0) app.hackSampleRateMHz = 20.0;
            app.viewA.resetView = true;
            if (running)
                app.hack.setSampleRate(app.hackSampleRateMHz * 1e6);
        }
        if (ImGui::SliderInt("LNA (IF) dB", &app.hackLna, 0, 40, "%d"))
        {
            app.hackLna = (app.hackLna / 8) * 8;
            if (running) app.hack.setLnaGain(app.hackLna);
        }
        if (ImGui::SliderInt("VGA (BB) dB", &app.hackVga, 0, 62, "%d"))
        {
            app.hackVga = (app.hackVga / 2) * 2;
            if (running) app.hack.setVgaGain(app.hackVga);
        }
        if (ImGui::Checkbox("RF amp (+~11 dB)", &app.hackAmp))
        {
            if (running) app.hack.setAmpEnable(app.hackAmp);
        }
        if (ImGui::Checkbox("Bias-T (antenna power)", &app.hackBias))
        {
            if (running) app.hack.setBiasTee(app.hackBias);
        }
        if (ImGui::Checkbox("DC block", &app.dcBlock))
        {
            if (running) app.hack.setDcBlock(app.dcBlock);
        }
    }

    ImGui::Separator();
    ImGui::Combo("FFT size", &app.fftSizeIdx, kFftLabels, kNumFftSizes);
    ImGui::SliderFloat("Averaging", &app.avgAlpha, 0.0f, 0.98f, "%.2f");
    ImGui::Checkbox("Auto-scale dB", &app.autoScale);
    ImGui::SliderFloat("dB min", &app.dbMin, -140.0f, 0.0f, "%.0f");
    ImGui::SliderFloat("dB max", &app.dbMax, -140.0f, 20.0f, "%.0f");
    if (app.dbMax < app.dbMin + 5.0f)
        app.dbMax = app.dbMin + 5.0f;

    if (ImGui::Button("Reset view (fit band)"))
    {
        app.viewA.resetView = true;
        app.viewB.resetView = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("drag=pan  scroll=zoom  dbl-click=fit");

    ImGui::BeginDisabled(app.sourceMode == 1);
    ImGui::Checkbox("Pan/scroll retunes SDR (browse band)", &app.bandBrowse);
    ImGui::EndDisabled();
    if (app.sourceMode == 1)
        ImGui::TextDisabled("  (WAV: tuning is fixed to the file)");

    ImGui::Separator();
    const char* bauds[] = {"600", "1200", "8400", "10500", "Inmarsat-C/EGC"};
    ImGui::Combo("Decode baud", &app.newBaud, bauds, 5);
    ImGui::TextDisabled("Ctrl+click the spectrum to add a decoder there");

    ImGui::Separator();
    ImGui::Checkbox("Follow C-channel voice", &app.voiceFollow);
    if (app.sourceMode == 1)
        ImGui::TextDisabled("  (WAV: only voice already in-band can be followed)");
    ImGui::SetNextItemWidth(140.0f);
    ImGui::SliderFloat("Hold (s)", &app.followHoldSec, 1.0f, 30.0f, "%.0f");
    if (app.following)
    {
        ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.35f, 1.0f),
                           "  Following ch %d @ %.4f MHz%s", app.followChannelId,
                           app.centerFreqMHz, app.followRetuned ? " (hopped)" : "");
    }
    else if (app.voiceFollow)
    {
        ImGui::TextDisabled("  Waiting for a voice assignment...");
    }

    ImGui::Separator();
    if (ImGui::CollapsingHeader("Voice SDR (2nd RTL)"))
    {
        ImGui::BeginDisabled(running);
        ImGui::Checkbox("Enable dedicated voice SDR", &app.voiceSdrEnabled);
        ImGui::InputInt("Device index", &app.deviceIndexB);
        if (app.deviceIndexB == app.deviceIndex)
            ImGui::TextColored(ImVec4(1, 0.5f, 0.3f, 1), "  must differ from primary (%d)", app.deviceIndex);
        ImGui::Combo("Voice rate (MHz)", &app.sampleRateIdxB, kRateLabels, kNumRates);
        ImGui::Checkbox("Voice auto gain", &app.autoGainB);
        if (!app.autoGainB)
            ImGui::SliderFloat("Voice gain (dB)", &app.gainDbB, 0.0f, 50.0f, "%.1f");
        ImGui::Checkbox("Voice Bias-T", &app.biasTeeB);
        ImGui::InputFloat("Voice PPM", &app.ppmB, 0.1f, 1.0f, "%.2f");
        ImGui::EndDisabled();
        ImGui::InputDouble("Voice park (MHz)", &app.voiceCenterMHz, 0.1, 1.0, "%.4f");
        if (app.dualMode)
            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.35f, 1.0f),
                               "  voice SDR active (dual mode)");
        else if (app.voiceSdrEnabled)
            ImGui::TextDisabled("  starts on next Start (uses 2 RTLs)");
    }

    ImGui::Separator();
    if (ImGui::CollapsingHeader("Output (message feed)"))
    {
        const char* fmts[] = {"JSON (JAERO/Acarshub)", "JAERO text"};
        ImGui::Combo("Format", &app.outFormat, fmts, 2);
        ImGui::Checkbox("Write to file", &app.outFile);
        ImGui::SetNextItemWidth(-70.0f);
        ImGui::InputText("File", app.outFilePath, sizeof(app.outFilePath));
        ImGui::Checkbox("Send over UDP", &app.outUdp);
        ImGui::SetNextItemWidth(-70.0f);
        ImGui::InputText("UDP host", app.outUdpHost, sizeof(app.outUdpHost));
        ImGui::InputInt("UDP port", &app.outUdpPort);
        ImGui::SetNextItemWidth(-70.0f);
        ImGui::InputText("Station", app.outStation, sizeof(app.outStation));
        ImGui::Separator();
        ImGui::Checkbox("SBS/BaseStation server (positions)", &app.outSbs);
        ImGui::InputInt("SBS port", &app.outSbsPort);
        if (app.outSbs)
        {
            if (app.feed.sbsListening())
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f),
                                   "Listening on TCP :%d  -  %d client(s), %llu sent",
                                   app.outSbsPort, app.feed.sbsClients(),
                                   (unsigned long long)app.feed.sbsSent());
            else
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f),
                                   "Bind failed on :%d (port in use? try another)",
                                   app.outSbsPort);
        }
        ImGui::Text("Sent: %llu", (unsigned long long)app.feed.sent());
        ImGui::TextDisabled("ACARS -> JAERO JSONdump; EGC -> STD-C JSON.");
        ImGui::TextDisabled("SBS: VRS receiver -> Network, 127.0.0.1, this port, BaseStation.");
    }

    if (running)
    {
        ImGui::Separator();
        ImGui::Text("  Sample rate:   %.4f MHz", app.active->sampleRate() / 1e6);
        ImGui::Text("  Input level:   %.1f dBFS", app.viewA.rmsDbfs);
        ImGui::Text("  Spectrum:      %.0f .. %.0f dB", app.viewA.frameDbMin, app.viewA.frameDbMax);

        if (app.sourceMode == 0)
        {
            double maxF = app.sdr.tunerMaxFreq();
            if (maxF > 0.0 && app.centerFreqMHz * 1e6 > maxF)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 90, 90, 255));
                ImGui::TextWrapped("  WARNING: %.0f MHz is above this tuner's ~%.0f MHz "
                                   "ceiling. The PLL can't lock here - pick a lower "
                                   "frequency or use an R820T2/R828D dongle.",
                                   app.centerFreqMHz, maxF / 1e6);
                ImGui::PopStyleColor();
            }
        }
    }

    ImGui::End();
}

static void drawSpectrum(App& app, SpectrumView& v, DecoderManager& mgr, const char* title,
                         bool allowBandBrowse, bool voiceView)
{
    ImGui::Begin(title);
    ImVec2 origin = ImGui::GetCursorScreenPos();
    float availW = ImGui::GetContentRegionAvail().x;
    std::string plotId = std::string("##plot_") + title;
    if (ImPlot::BeginPlot(plotId.c_str(), ImVec2(-1, -1), ImPlotFlags_NoLegend))
    {
        ImPlot::SetupAxes("MHz", "dB", 0, 0);

        bool bandValid = (v.curN > 0 && v.freqMHz.front() < v.freqMHz.back());
        if (bandValid)
        {
            double bandSpan = v.freqMHz.back() - v.freqMHz.front();
            ImPlot::SetupAxisZoomConstraints(ImAxis_X1, bandSpan * 1e-4, bandSpan);
        }
        if (v.resetView && bandValid)
            ImPlot::SetupAxisLimits(ImAxis_X1, v.freqMHz.front(), v.freqMHz.back(), ImGuiCond_Always);
        if (app.autoScale || v.resetView)
            ImPlot::SetupAxisLimits(ImAxis_Y1, app.dbMin, app.dbMax, ImGuiCond_Always);

        if (v.curN > 0)
            ImPlot::PlotLine("PSD", v.freqMHz.data(), v.avg.data(), v.curN);

        auto decs = mgr.status();
        for (auto& d : decs)
        {
            double x = d.freqMHz;
            ImVec4 col = d.locked ? ImVec4(0.2f, 1.0f, 0.35f, 1.0f)
                                  : ImVec4(0.9f, 0.7f, 0.2f, 1.0f);
            if (ImPlot::DragLineX(d.channelId, &x, col, 2.0f))
                mgr.setDecoderFreq(d.channelId, x * 1e6);
        }

        ImPlotRect lim = ImPlot::GetPlotLimits();
        v.viewXminMHz = lim.X.Min;
        v.viewXmaxMHz = lim.X.Max;

        if (allowBandBrowse && app.bandBrowse && app.sourceMode != 1 &&
            app.active->running() && !v.resetView && !app.following)
        {
            double viewCtr = 0.5 * (v.viewXminMHz + v.viewXmaxMHz);
            double viewHalf = 0.5 * (v.viewXmaxMHz - v.viewXminMHz);
            double sdrCtr = app.active->centerFreq() / 1e6;
            double fsMHz = app.active->sampleRate() / 1e6;
            double halfBand = 0.5 * fsMHz;
            // Clear margin remaining between the view edge and the captured band
            // edge. Recenter the SDR on the view *before* that margin runs out so
            // no black gap ever reaches the screen -> buttery smooth panning.
            double marginL = (viewCtr - viewHalf) - (sdrCtr - halfBand);
            double marginR = (sdrCtr + halfBand) - (viewCtr + viewHalf);
            double minMargin = std::min(marginL, marginR);
            double trigger = fsMHz * (app.browseEdgePct * 0.01);
            bool moved = std::fabs(viewCtr - app.lastRetuneCtr) > fsMHz * (app.browseMinMovePct * 0.01);
            auto now = std::chrono::steady_clock::now();
            double sinceMs =
                std::chrono::duration<double, std::milli>(now - app.lastRetune).count();
            if (fsMHz > 0.0 && minMargin < trigger && moved && sinceMs > app.browseThrottleMs)
            {
                retunePreserving(app, viewCtr);
                app.lastRetune = now;
                app.lastRetuneCtr = viewCtr;
            }
        }

        ImVec2 pp = ImPlot::GetPlotPos();
        ImVec2 ps = ImPlot::GetPlotSize();
        v.specLeftInset = pp.x - origin.x;
        v.specRightInset = (origin.x + availW) - (pp.x + ps.x);

        if (bandValid)
            v.resetView = false;

        if (ImPlot::IsPlotHovered() && ImGui::GetIO().KeyCtrl &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            ImPlotPoint mp = ImPlot::GetPlotMousePos();
            int baud;
            if (voiceView)
                baud = 8400; // voice SDR: manual voice decoder
            else
            {
                static const int kBaudVals[] = {600, 1200, 8400, 10500, kEgcBaud};
                int idx = app.newBaud < 0 ? 0 : (app.newBaud > 4 ? 4 : app.newBaud);
                baud = kBaudVals[idx];
            }
            mgr.addDecoder(mp.x * 1e6, baud);
        }

        ImPlot::EndPlot();
    }
    ImGui::End();
}

static void drawWaterfall(App& app, SpectrumView& v, const char* title)
{
    (void)app;
    ImGui::Begin(title);

    float uMin = 0.0f, uMax = 1.0f;
    float xLo = 0.0f, xHi = 1.0f;
    if (v.curN > 0)
    {
        double bandMin = v.freqMHz.front();
        double bandMax = v.freqMHz.back();
        double bandSpan = bandMax - bandMin;
        double viewSpan = v.viewXmaxMHz - v.viewXminMHz;
        if (bandSpan > 0.0 && viewSpan > 0.0)
        {
            double visLo = std::max(bandMin, v.viewXminMHz);
            double visHi = std::min(bandMax, v.viewXmaxMHz);
            if (visHi > visLo)
            {
                uMin = (float)((visLo - bandMin) / bandSpan);
                uMax = (float)((visHi - bandMin) / bandSpan);
                xLo = (float)((visLo - v.viewXminMHz) / viewSpan);
                xHi = (float)((visHi - v.viewXminMHz) / viewSpan);
            }
            else
            {
                xLo = xHi = 0.0f;
            }
        }
    }

    ImVec2 avail = ImGui::GetContentRegionAvail();
    float left = std::max(0.0f, v.specLeftInset);
    float right = std::max(0.0f, v.specRightInset);
    float w = avail.x - left - right;
    if (w < 1.0f)
    {
        w = avail.x;
        left = 0.0f;
    }
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + left);

    v.waterfall.draw(ImVec2(w, avail.y), uMin, uMax, xLo, xHi);
    ImGui::End();
}

static void drawDecoders(App& app)
{
    ImGui::Begin("Decoders");

    auto decs = app.decoders.status();
    ImGui::Text("%d active  |  %d sub-band(s)  %d threads", (int)decs.size(),
                app.decoders.subbandCount(), app.decoders.workerCount());
    uint64_t drops = app.decoders.drops();
    ImGui::SameLine();
    if (drops > 0)
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "  drops: %llu",
                           (unsigned long long)drops);
    else
        ImGui::TextDisabled("  drops: 0");
    ImGui::SameLine();
    if (ImGui::SmallButton("Remove all"))
        app.decoders.removeAll();

    int vm = app.decoders.voiceMonitor();
    if (vm >= 0)
        ImGui::Text("Voice: monitoring ch %d", vm);
    else
        ImGui::TextDisabled("Voice: (no 8400 decoder)");
    ImGui::SameLine();
    float lvl = app.decoders.audioLevel() * 5.0f; // voice is quiet; scale for the meter
    if (lvl > 1.0f) lvl = 1.0f;
    ImGui::ProgressBar(lvl, ImVec2(110, 0), "");
    ImGui::SameLine();
    if (ImGui::SmallButton("Listen to selected"))
        app.decoders.setVoiceMonitor(app.selectedDecoder);

    // Audio output device picker.
    if (app.audioDevs.empty())
        app.audioDevs = app.decoders.audioDevices();
    {
        std::vector<const char*> names;
        names.reserve(app.audioDevs.size());
        for (auto& s : app.audioDevs)
            names.push_back(s.c_str());
        if (app.audioDevice >= (int)names.size())
            app.audioDevice = 0;
        ImGui::SetNextItemWidth(-90.0f);
        if (ImGui::Combo("Audio out", &app.audioDevice, names.data(), (int)names.size()))
        {
            app.decoders.setAudioDevice(app.audioDevice);
            app.decodersB.setAudioDevice(app.audioDevice);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Refresh##aud"))
        {
            app.audioDevs = app.decoders.audioDevices();
            app.decodersB.audioDevices(); // keep B's cache aligned
        }
    }

    if (ImGui::Checkbox("Record voice calls", &app.recordVoice))
        app.decoders.setRecording(app.recordVoice, app.recordDir);
    ImGui::SameLine();
    if (app.recordVoice)
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "REC  (%d active)",
                           app.decoders.recordingCount());
    else
        ImGui::TextDisabled("(saves every 8400 call to its own WAV)");
    ImGui::BeginDisabled(app.recordVoice);
    ImGui::SetNextItemWidth(-90.0f);
    ImGui::InputText("Folder", app.recordDir, sizeof(app.recordDir));
    ImGui::EndDisabled();

    ImGui::Separator();

    if (ImGui::BeginTable("##decs", 6,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        ImGui::TableSetupColumn("Lock", ImGuiTableColumnFlags_WidthFixed, 36);
        ImGui::TableSetupColumn("Freq MHz");
        ImGui::TableSetupColumn("Baud");
        ImGui::TableSetupColumn("Eb/N0");
        ImGui::TableSetupColumn("Msgs");
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();

        int toRemove = -1;
        for (auto& d : decs)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            bool sel = (app.selectedDecoder == d.channelId);
            char selid[24];
            std::snprintf(selid, sizeof(selid), "##sel%d", d.channelId);
            if (ImGui::Selectable(selid, sel,
                                  ImGuiSelectableFlags_SpanAllColumns |
                                      ImGuiSelectableFlags_AllowOverlap))
                app.selectedDecoder = d.channelId;
            ImGui::SameLine();
            ImVec4 c = d.locked ? ImVec4(0.2f, 1.0f, 0.3f, 1.0f)
                                : ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
            ImGui::TextColored(c, "%s", d.locked ? "LOCK" : "--");
            ImGui::TableNextColumn();
            ImGui::Text("%.4f", d.freqMHz);
            ImGui::TableNextColumn();
            if (d.baud == kEgcBaud)
                ImGui::TextUnformatted("EGC");
            else
                ImGui::Text("%d", d.baud);
            ImGui::TableNextColumn();
            if (d.baud == kEgcBaud)
            {
                if (d.egcFrames > 0)
                    ImGui::Text("BER %d (%dfr)", d.egcBer, d.egcFrames);
                else
                    ImGui::TextDisabled("--");
            }
            else
                ImGui::Text("%.1f", d.ebno);
            ImGui::TableNextColumn();
            ImGui::Text("%llu", (unsigned long long)d.msgs);
            ImGui::TableNextColumn();
            char btn[24];
            std::snprintf(btn, sizeof(btn), "X##%d", d.channelId);
            if (ImGui::SmallButton(btn))
                toRemove = d.channelId;
        }
        ImGui::EndTable();
        if (toRemove >= 0)
            app.decoders.removeDecoder(toRemove);
    }

    ImGui::End();
}

static void drawSUs(App& app)
{
    ImGui::Begin("SUs");

    ImGui::Text("%llu total", (unsigned long long)app.decoders.suLog().count());
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear"))
        app.decoders.suLog().clear();
    ImGui::Separator();

    auto msgs = app.decoders.suLog().snapshot();
    if (ImGui::BeginTable("##sus", 3,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable))
    {
        ImGui::TableSetupColumn("Freq", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Text", ImGuiTableColumnFlags_WidthFixed, 220);
        ImGui::TableSetupColumn("Bytes");
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (auto it = msgs.rbegin(); it != msgs.rend(); ++it)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%.3f", it->freqMHz);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(it->text.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(it->hex.c_str());
        }
        ImGui::EndTable();
    }

    ImGui::End();
}

static void drawMessages(App& app)
{
    ImGui::Begin("Messages");

    ImGui::Text("%llu total", (unsigned long long)app.decoders.log().count());
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear"))
        app.decoders.log().clear();
    ImGui::SameLine();
    ImGui::Checkbox("Show empty", &app.showEmptyMsgs);
    ImGui::Separator();

    auto msgs = app.decoders.log().snapshot();
    if (ImGui::BeginTable("##msgs", 6,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable))
    {
        ImGui::TableSetupColumn("Freq", ImGuiTableColumnFlags_WidthFixed, 64);
        ImGui::TableSetupColumn("Dir", ImGuiTableColumnFlags_WidthFixed, 32);
        ImGui::TableSetupColumn("Reg", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("AES", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("Lbl", ImGuiTableColumnFlags_WidthFixed, 36);
        ImGui::TableSetupColumn("Text");
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        int rowIdx = 0;
        for (auto it = msgs.rbegin(); it != msgs.rend(); ++it)
        {
            // Hide empty ACARS messages (no text and no decoded body) unless shown.
            if (!app.showEmptyMsgs && it->text.empty() && it->decoded.empty())
                continue;
            ImGui::TableNextRow();
            ImGui::PushID(rowIdx++);
            ImGui::TableNextColumn();
            ImGui::Text("%.3f", it->freqMHz);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(it->downlink ? "DL" : "UL");
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(it->reg.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%06X", it->aesId);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(it->label.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(it->text.c_str());
            if (it->hasPos)
                ImGui::TextColored(ImVec4(0.3f, 0.9f, 1.0f, 1.0f),
                                   "POS %.4f, %.4f  %d ft", it->lat, it->lon, it->alt);
            if (!it->decoded.empty())
            {
                if (ImGui::TreeNodeEx("decoded", ImGuiTreeNodeFlags_SpanAvailWidth,
                                      "decoded (CPDLC/ADS-C/MIAM)"))
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 1.0f, 0.6f, 1.0f));
                    ImGui::TextUnformatted(it->decoded.c_str());
                    ImGui::PopStyleColor();
                    ImGui::TreePop();
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::End();
}

static void drawAircraft(App& app)
{
    ImGui::Begin("Aircraft");

    auto acs = app.decoders.aircraftTable().snapshot();
    ImGui::Text("%zu tracked", acs.size());
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear"))
        app.decoders.aircraftTable().clear();
    ImGui::SameLine();
    ImGui::Checkbox("With position only", &app.acPosOnly);
    ImGui::Separator();

    double now = (double)std::time(nullptr);
    // Newest activity first.
    std::sort(acs.begin(), acs.end(),
              [](const AircraftEntry& a, const AircraftEntry& b) { return a.lastSeen > b.lastSeen; });

    if (ImGui::BeginTable("##aircraft", 9,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable))
    {
        ImGui::TableSetupColumn("AES", ImGuiTableColumnFlags_WidthFixed, 58);
        ImGui::TableSetupColumn("ICAO", ImGuiTableColumnFlags_WidthFixed, 54);
        ImGui::TableSetupColumn("Reg", ImGuiTableColumnFlags_WidthFixed, 64);
        ImGui::TableSetupColumn("Flight", ImGuiTableColumnFlags_WidthFixed, 64);
        ImGui::TableSetupColumn("Lat", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Lon", ImGuiTableColumnFlags_WidthFixed, 74);
        ImGui::TableSetupColumn("Alt", ImGuiTableColumnFlags_WidthFixed, 56);
        ImGui::TableSetupColumn("Age", ImGuiTableColumnFlags_WidthFixed, 48);
        ImGui::TableSetupColumn("Msgs", ImGuiTableColumnFlags_WidthFixed, 48);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (const auto& a : acs)
        {
            if (app.acPosOnly && !a.hasPos)
                continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%06X", a.aesId);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(a.icao.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(a.reg.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(a.flight.c_str());
            ImGui::TableNextColumn();
            if (a.hasPos) ImGui::Text("%.4f", a.lat);
            ImGui::TableNextColumn();
            if (a.hasPos) ImGui::Text("%.4f", a.lon);
            ImGui::TableNextColumn();
            if (a.hasPos) ImGui::Text("%d", a.alt);
            ImGui::TableNextColumn();
            ImGui::Text("%ds", (int)(now - a.lastSeen));
            ImGui::TableNextColumn();
            ImGui::Text("%llu", (unsigned long long)a.msgs);
        }
        ImGui::EndTable();
    }

    ImGui::End();
}

static const char* cassignTypeName(uint8_t t)
{
    switch (t)
    {
    case 0x31: return "Distress";
    case 0x32: return "Flight safety";
    case 0x33: return "Other safety";
    case 0x34: return "Non-safety";
    default: return "C-channel";
    }
}

// Manually tune to a C-channel voice assignment: retune the SDR off the carrier
// (DC avoidance) if it is out of band, then drop an 8400 voice decoder on it and
// monitor it. Used by the C-Channel "Tune" button (works with auto-follow off).
static void tuneToVoice(App& app, double rxMHz)
{
    if (rxMHz <= 1.0 || !app.active->running())
        return;
    double centerMHz = app.active->centerFreq() / 1e6;
    double halfMHz = (app.active->sampleRate() / 1e6) * 0.45;
    bool inBand = std::fabs(rxMHz - centerMHz) <= halfMHz;
    if (!inBand)
    {
        if (app.sourceMode == 1)
            return; // WAV: tuning is fixed to the file
        double fsMHz = app.active->sampleRate() / 1e6;
        double off = std::min(0.2, 0.25 * fsMHz);
        retunePreserving(app, rxMHz - off); // move center off the carrier, keep decoders
    }
    int id = app.decoders.addDecoder(rxMHz * 1e6, 8400);
    if (id >= 0)
    {
        app.decoders.setVoiceMonitor(id);
        app.selectedDecoder = id;
    }
}

static void drawCChannel(App& app)
{
    ImGui::Begin("C-Channel");

    ImGui::Text("%llu assignment(s)", (unsigned long long)app.decoders.cassignLog().count());
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear"))
        app.decoders.cassignLog().clear();
    ImGui::Separator();

    auto items = app.decoders.cassignLog().snapshot();
    if (ImGui::BeginTable("##cchan", 6,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable))
    {
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 96);
        ImGui::TableSetupColumn("AES", ImGuiTableColumnFlags_WidthFixed, 64);
        ImGui::TableSetupColumn("GES", ImGuiTableColumnFlags_WidthFixed, 40);
        ImGui::TableSetupColumn("RX / down (MHz)");
        ImGui::TableSetupColumn("TX / up (MHz)");
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 52);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        // Oldest-first (newest appended at the bottom) so the list grows
        // downward and doesn't shift content out from under a scrolled-up user.
        for (size_t i = 0; i < items.size(); ++i)
        {
            const auto& it = items[i];
            ImGui::TableNextRow();
            ImGui::PushID((int)i);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(cassignTypeName(it.type));
            ImGui::TableNextColumn();
            ImGui::Text("%06X", it.aesId);
            ImGui::TableNextColumn();
            ImGui::Text("%02X", it.gesId);
            ImGui::TableNextColumn();
            ImGui::Text("%.4f", it.rxMHz);
            ImGui::TableNextColumn();
            ImGui::Text("%.4f", it.txMHz);
            ImGui::TableNextColumn();
            if (it.rxMHz > 1.0)
            {
                ImGui::BeginDisabled(!app.active->running() ||
                                     (app.sourceMode == 1));
                if (ImGui::SmallButton("Tune"))
                    tuneToVoice(app, it.rxMHz);
                ImGui::EndDisabled();
            }
            ImGui::PopID();
        }

        // Keep pinned to the newest row only while the user is already at the
        // bottom; if they scroll up, leave their position alone.
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
            ImGui::SetScrollHereY(1.0f);

        ImGui::EndTable();
    }
    ImGui::End();
}

static void drawNetwork(App& app)
{
    ImGui::Begin("Network");

    SatInfo sat = app.decoders.channelTable().satellite();
    if (sat.valid)
        ImGui::Text("Satellite ID: %d   Longitude: %s", sat.satId, sat.longitude.c_str());
    else
        ImGui::TextDisabled("Satellite: (waiting for system table)");
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear"))
        app.decoders.channelTable().clear();
    ImGui::TextDisabled("Discovered from system-table broadcasts. RX = forward (decodable).");
    ImGui::Separator();

    auto chans = app.decoders.channelTable().snapshot();
    if (ImGui::BeginTable("##net", 5,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable))
    {
        ImGui::TableSetupColumn("Freq MHz", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Type");
        ImGui::TableSetupColumn("GES", ImGuiTableColumnFlags_WidthFixed, 40);
        ImGui::TableSetupColumn("Hits", ImGuiTableColumnFlags_WidthFixed, 48);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < chans.size(); ++i)
        {
            const auto& c = chans[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%.4f", c.freqMHz);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(c.kind.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%02X", c.ges);
            ImGui::TableNextColumn();
            ImGui::Text("%llu", (unsigned long long)c.hits);
            ImGui::TableNextColumn();
            if (c.decodable)
            {
                char btn[24];
                std::snprintf(btn, sizeof(btn), "Tune##%zu", i);
                if (ImGui::SmallButton(btn))
                    app.decoders.addDecoder(c.freqMHz * 1e6, c.baud);
            }
            else
            {
                ImGui::TextDisabled("return");
            }
        }
        ImGui::EndTable();
    }

    ImGui::End();
}

static void drawEgc(App& app)
{
    ImGui::Begin("EGC");

    ImGui::Text("%llu message(s)", (unsigned long long)app.decoders.egcLog().count());
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear"))
        app.decoders.egcLog().clear();
    ImGui::TextDisabled("Inmarsat-C SafetyNET / FleetNET / system messages.");
    ImGui::Separator();

    auto msgs = app.decoders.egcLog().snapshot();
    if (ImGui::BeginTable("##egc", 5,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable))
    {
        ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 64);
        ImGui::TableSetupColumn("Priority", ImGuiTableColumnFlags_WidthFixed, 64);
        ImGui::TableSetupColumn("MsgId", ImGuiTableColumnFlags_WidthFixed, 52);
        ImGui::TableSetupColumn("Service", ImGuiTableColumnFlags_WidthFixed, 200);
        ImGui::TableSetupColumn("Message");
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (auto it = msgs.rbegin(); it != msgs.rend(); ++it)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(it->timeUtc.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(it->priority.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%d", it->messageId);
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", it->service.c_str());
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", it->text.c_str());
        }
        ImGui::EndTable();
    }

    ImGui::End();
}

static ImPlotPoint constGetter(int idx, void* data)
{
    const float* p = static_cast<const float*>(data);
    return ImPlotPoint(p[idx * 2], p[idx * 2 + 1]);
}

static void drawConstellation(App& app)
{
    ImGui::Begin("Constellation");

    auto decs = app.decoders.status();
    int chan = app.selectedDecoder;
    bool valid = false;
    double freq = 0.0;
    for (auto& d : decs)
        if (d.channelId == chan)
        {
            valid = true;
            freq = d.freqMHz;
            break;
        }
    if (!valid && !decs.empty())
    {
        chan = decs.front().channelId;
        freq = decs.front().freqMHz;
    }

    if (decs.empty())
    {
        ImGui::TextDisabled("No decoders. Ctrl+click the spectrum to add one.");
        ImGui::End();
        return;
    }

    // Decoder selector (also selectable by clicking a row in Decoders panel).
    char preview[64];
    std::snprintf(preview, sizeof(preview), "ch %d  %.4f MHz", chan, freq);
    if (ImGui::BeginCombo("Decoder", preview))
    {
        for (auto& d : decs)
        {
            char label[64];
            std::snprintf(label, sizeof(label), "ch %d  %.4f MHz  @%d",
                          d.channelId, d.freqMHz, d.baud);
            if (ImGui::Selectable(label, d.channelId == chan))
            {
                app.selectedDecoder = d.channelId;
                chan = d.channelId;
                freq = d.freqMHz;
            }
        }
        ImGui::EndCombo();
    }

    int pairs = app.decoders.getConstellation(chan, app.constBuf, 1024);
    ImGui::SameLine();
    ImGui::TextDisabled("(%d pts)", pairs);

    // Recompute the axis scale at most once per second so it holds steady
    // instead of jittering as the constellation data changes every frame.
    auto nowC = std::chrono::steady_clock::now();
    if (std::chrono::duration<double>(nowC - app.constLimTime).count() >= 1.0)
    {
        float m = 0.5f;
        for (float v : app.constBuf)
            m = std::max(m, std::fabs(v));
        app.constLim = m * 1.15;
        app.constLimTime = nowC;
    }
    double lim = app.constLim;

    if (ImPlot::BeginPlot("##const", ImVec2(-1, -1),
                          ImPlotFlags_Equal | ImPlotFlags_NoLegend))
    {
        ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoTickLabels,
                          ImPlotAxisFlags_NoTickLabels);
        ImPlot::SetupAxisLimits(ImAxis_X1, -lim, lim, ImGuiCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_Y1, -lim, lim, ImGuiCond_Always);
        if (pairs > 0)
            ImPlot::PlotScatterG("IQ", constGetter, app.constBuf.data(), pairs);
        ImPlot::EndPlot();
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Persistent settings: serialized into inmarscope.ini alongside the ImGui dock
// layout via a custom settings handler.
// ---------------------------------------------------------------------------
static void cfgWriteAll(App& app, ImGuiTextBuffer* buf)
{
    buf->append("[InmarScope][State]\n");
#define WI(f) buf->appendf(#f "=%d\n", (int)app.f)
#define WF(f) buf->appendf(#f "=%g\n", (double)app.f)
#define WD(f) buf->appendf(#f "=%.10g\n", (double)app.f)
#define WS(f) buf->appendf(#f "=%s\n", app.f)
    WI(sourceMode); WI(deviceIndex); WI(sampleRateIdx); WI(newBaud); WI(fftSizeIdx);
    WI(audioDevice);
    WD(centerFreqMHz);
    WI(autoGain); WF(gainDb); WI(biasTee); WF(ppm); WI(dcBlock);
    WI(autoScale); WI(bandBrowse); WF(avgAlpha); WF(dbMin); WF(dbMax);
    WF(browseEdgePct); WF(browseThrottleMs); WF(browseMinMovePct);
    WS(wavPath); WI(wavLoop);
    WS(serverHost); WI(serverPort); WI(serverCompression); WI(serverSampleType);
    WD(serverSampleRateMHz);
    WD(hackSampleRateMHz); WI(hackLna); WI(hackVga); WI(hackAmp); WI(hackBias);
    WI(voiceSdrEnabled); WI(deviceIndexB); WD(voiceCenterMHz); WI(sampleRateIdxB);
    WI(autoGainB); WF(gainDbB); WI(biasTeeB); WF(ppmB);
    WI(voiceFollow); WF(followHoldSec);
    WI(recordVoice); WS(recordDir);
    WI(acPosOnly);
    WI(showEmptyMsgs);
    WI(outFile); WS(outFilePath); WI(outUdp); WS(outUdpHost); WI(outUdpPort);
    WI(outFormat); WS(outStation); WI(outSbs); WI(outSbsPort);
    WI(layoutVersion);
    buf->append("\n");
#undef WI
#undef WF
#undef WD
#undef WS
}

static void cfgReadLine(App& app, const char* line)
{
    const char* eq = std::strchr(line, '=');
    if (!eq)
        return;
    char key[48];
    int klen = (int)(eq - line);
    if (klen <= 0 || klen >= (int)sizeof(key))
        return;
    std::memcpy(key, line, klen);
    key[klen] = 0;
    const char* val = eq + 1;
#define RI(f) if (!std::strcmp(key, #f)) { app.f = std::atoi(val); return; }
#define RB(f) if (!std::strcmp(key, #f)) { app.f = (std::atoi(val) != 0); return; }
#define RF(f) if (!std::strcmp(key, #f)) { app.f = (float)std::atof(val); return; }
#define RD(f) if (!std::strcmp(key, #f)) { app.f = std::atof(val); return; }
#define RS(f) if (!std::strcmp(key, #f)) { std::strncpy(app.f, val, sizeof(app.f) - 1); app.f[sizeof(app.f) - 1] = 0; return; }
    RI(sourceMode); RI(deviceIndex); RI(sampleRateIdx); RI(newBaud); RI(fftSizeIdx);
    RI(audioDevice);
    RD(centerFreqMHz);
    RB(autoGain); RF(gainDb); RB(biasTee); RF(ppm); RB(dcBlock);
    RB(autoScale); RB(bandBrowse); RF(avgAlpha); RF(dbMin); RF(dbMax);
    RF(browseEdgePct); RF(browseThrottleMs); RF(browseMinMovePct);
    RS(wavPath); RB(wavLoop);
    RS(serverHost); RI(serverPort); RB(serverCompression); RI(serverSampleType);
    RD(serverSampleRateMHz);
    RD(hackSampleRateMHz); RI(hackLna); RI(hackVga); RB(hackAmp); RB(hackBias);
    RB(voiceSdrEnabled); RI(deviceIndexB); RD(voiceCenterMHz); RI(sampleRateIdxB);
    RB(autoGainB); RF(gainDbB); RB(biasTeeB); RF(ppmB);
    RB(voiceFollow); RF(followHoldSec);
    RB(recordVoice); RS(recordDir);
    RB(acPosOnly);
    RB(showEmptyMsgs);
    RB(outFile); RS(outFilePath); RB(outUdp); RS(outUdpHost); RI(outUdpPort);
    RI(outFormat); RS(outStation); RB(outSbs); RI(outSbsPort);
    RI(layoutVersion);
#undef RI
#undef RB
#undef RF
#undef RD
#undef RS
}

static void cfgRegisterHandler(App& app)
{
    ImGuiSettingsHandler h;
    h.TypeName = "InmarScope";
    h.TypeHash = ImHashStr("InmarScope");
    h.UserData = &app;
    h.ReadOpenFn = [](ImGuiContext*, ImGuiSettingsHandler*, const char* name) -> void* {
        return std::strcmp(name, "State") == 0 ? (void*)1 : nullptr;
    };
    h.ReadLineFn = [](ImGuiContext*, ImGuiSettingsHandler* handler, void* entry,
                      const char* line) {
        if (!entry)
            return;
        cfgReadLine(*static_cast<App*>(handler->UserData), line);
    };
    h.WriteAllFn = [](ImGuiContext*, ImGuiSettingsHandler* handler, ImGuiTextBuffer* buf) {
        cfgWriteAll(*static_cast<App*>(handler->UserData), buf);
    };
    ImGui::AddSettingsHandler(&h);
}

// Bump this whenever the built-in default dock layout changes so saved older
// layouts are replaced by the new default on next launch.
static constexpr int kLayoutVersion = 1;

static void drawDockHost(App& app)
{
    // Default to NOT forcing a rebuild: if inmarscope.ini holds a saved layout,
    // the dock node already exists and we keep it. Only build the default layout
    // on first run (no node) or when explicitly forced (Reset Layout / dual /
    // a layout-version bump).
    static bool forceLayout = false;
    if (app.forceDefaultLayout) { forceLayout = true; app.forceDefaultLayout = false; }
    static bool lastDual = false;
    if (app.dualMode != lastDual) { forceLayout = true; lastDual = app.dualMode; }

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowViewport(vp->ID);

    ImGuiWindowFlags hostFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_MenuBar;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##InmarScopeHost", nullptr, hostFlags);
    ImGui::PopStyleVar(3);

    ImGuiID dockId = ImGui::GetID("InmarScopeDockSpace");
    ImGui::DockSpace(dockId, ImVec2(0, 0), ImGuiDockNodeFlags_NoUndocking);

    if (forceLayout || ImGui::DockBuilderGetNode(dockId) == nullptr)
    {
        forceLayout = false;
        ImGui::DockBuilderRemoveNode(dockId);
        ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockId, vp->WorkSize);

        ImGuiID left, right, rtop, rrest, rmid, rbot, rcon, ctrl, dec;
        ImGui::DockBuilderSplitNode(dockId, ImGuiDir_Left, 0.32f, &left, &right);
        ImGui::DockBuilderSplitNode(left, ImGuiDir_Up, 0.62f, &ctrl, &dec);
        ImGui::DockBuilderSplitNode(right, ImGuiDir_Up, 0.30f, &rtop, &rrest);
        ImGui::DockBuilderSplitNode(rrest, ImGuiDir_Up, 0.58f, &rmid, &rbot);
        ImGui::DockBuilderSplitNode(rbot, ImGuiDir_Right, 0.34f, &rcon, &rbot);

        ImGui::DockBuilderDockWindow("Control", ctrl);
        ImGui::DockBuilderDockWindow("Decoders", dec);

        // In dual-SDR mode, split the spectrum and waterfall rows side-by-side
        // so the voice SDR (B) gets its own live spectrum/waterfall.
        if (app.dualMode)
        {
            ImGuiID rtopR, rmidR;
            ImGui::DockBuilderSplitNode(rtop, ImGuiDir_Right, 0.5f, &rtopR, &rtop);
            ImGui::DockBuilderSplitNode(rmid, ImGuiDir_Right, 0.5f, &rmidR, &rmid);
            ImGui::DockBuilderDockWindow("Spectrum (Voice)", rtopR);
            ImGui::DockBuilderDockWindow("Waterfall (Voice)", rmidR);
        }
        ImGui::DockBuilderDockWindow("Spectrum", rtop);
        ImGui::DockBuilderDockWindow("Waterfall", rmid);
        ImGui::DockBuilderDockWindow("SUs", rbot);
        ImGui::DockBuilderDockWindow("Messages", rbot);
        ImGui::DockBuilderDockWindow("C-Channel", rbot);
        ImGui::DockBuilderDockWindow("Network", rbot);
        ImGui::DockBuilderDockWindow("EGC", rbot);
        ImGui::DockBuilderDockWindow("Aircraft", rbot);
        ImGui::DockBuilderDockWindow("Constellation", rcon);
        ImGui::DockBuilderFinish(dockId);
    }

    if (ImGui::BeginMenuBar())
    {
        if (ImGui::BeginMenu("View"))
        {
            if (ImGui::MenuItem("Reset Layout"))
                forceLayout = true;
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    ImGui::End();
}

int main(int, char**)
{
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit())
        return 1;

    const char* glsl_version = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);

    GLFWwindow* window = glfwCreateWindow(1400, 900, "InmarScope", nullptr, nullptr);
    if (!window)
    {
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    App app;

    // Persistent settings + dock layout live in inmarscope.ini. Register our
    // custom handler and load the file before init so the saved values take
    // effect (FFT size, dB scale, source, etc.).
    cfgRegisterHandler(app);
    io.IniFilename = "inmarscope.ini";
    ImGui::LoadIniSettingsFromDisk(io.IniFilename);

    // If the saved layout predates the current default (or none was saved),
    // rebuild the canonical default layout so users get the intended arrangement.
    if (app.layoutVersion != kLayoutVersion)
    {
        app.forceDefaultLayout = true;
        app.layoutVersion = kLayoutVersion;
    }

    buildWindow(app.viewA, kFftSizes[app.fftSizeIdx], app.dbMin);
    buildWindow(app.viewB, kFftSizes[app.fftSizeIdx], app.dbMin);
    app.devices = app.sdr.listDevices();
    app.audioDevs = app.decoders.audioDevices();
    app.decodersB.audioDevices(); // prime SDR B's device cache for id resolution
    app.decoders.setAudioDevice(app.audioDevice);  // apply persisted audio device
    app.decodersB.setAudioDevice(app.audioDevice);
    app.verCheck.start("inmarscope", INMARSCOPE_VERSION);

    const ImVec4 clear_color = ImVec4(0.06f, 0.07f, 0.09f, 1.0f);

    while (!glfwWindowShouldClose(window))
    {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        drawDockHost(app);

        if (app.active->running())
            processFft(app.viewA, app, app.active->centerFreq(), app.active->sampleRate());
        if (app.dualMode && app.sdrB.running())
            processFft(app.viewB, app, app.sdrB.centerFreq(), app.sdrB.sampleRate());

        if (app.active->running())
            updateVoiceFollow(app);

        if (app.active->running())
            updateRateChange(app);

        updateFeed(app);

        drawControls(app);
        drawSpectrum(app, app.viewA, app.decoders, "Spectrum", true, false);
        drawWaterfall(app, app.viewA, "Waterfall");
        if (app.dualMode)
        {
            drawSpectrum(app, app.viewB, app.decodersB, "Spectrum (Voice)", false, true);
            drawWaterfall(app, app.viewB, "Waterfall (Voice)");
        }
        drawDecoders(app);
        drawSUs(app);
        drawMessages(app);
        drawCChannel(app);
        drawNetwork(app);
        drawEgc(app);
        drawAircraft(app);
        drawConstellation(app);

        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(clear_color.x, clear_color.y, clear_color.z, clear_color.w);
        glClear(GL_COLOR_BUFFER_BIT);

        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    // Persist settings + dock layout to inmarscope.ini before shutting down.
    ImGui::SaveIniSettingsToDisk(io.IniFilename);

    app.decoders.stop();
    app.decodersB.stop();
    app.sdr.stop();
    app.sdrB.stop();
    app.wav.stop();
    app.server.stop();
    app.hack.stop();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
