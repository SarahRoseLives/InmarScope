// FrameSync - Inmarsat decoder
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
#include "gui/waterfall.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
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

    IqRing ring{1u << 21};
    JFFT fft;
    Waterfall waterfall;
    DecoderManager decoders;
    int newBaud = 1; // 0 = 600, 1 = 1200 (baud for click-added decoders)
    int selectedDecoder = -1;     // channelId shown in the constellation panel
    std::vector<float> constBuf;  // interleaved I,Q scratch for the plot

    // Voice call recording (8400). Saves every decoded call to its own WAV.
    bool recordVoice = false;
    char recordDir[256] = "recordings";

    // Voice follow: when a C-channel voice assignment appears, hop the SDR to
    // the assigned RX (forward) frequency, decode the 8400 voice call, then hop
    // back to the P-channel home when the call goes idle.
    bool   voiceFollow = false;
    float  followHoldSec = 6.0f;        // idle time before ending a follow
    bool   following = false;           // a follow is currently active
    bool   followRetuned = false;       // true if we had to move the SDR center
    bool   followEverLocked = false;    // the voice decoder has locked at least once
    int    followChannelId = -1;        // the spawned 8400 voice decoder
    double followHomeMHz = 0.0;         // P-channel center to return to
    uint64_t followSeenCount = 0;       // cassign entries already considered
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

    // FFT / display settings.
    int   fftSizeIdx = 2; // index into kFftSizes
    float avgAlpha = 0.6f;
    float dbMin = -80.0f;
    float dbMax = 0.0f;
    bool  dcBlock = true;      // gentle DC blocker in the source path

    std::string status = "Idle";

    // Live diagnostics.
    float rmsDbfs = -120.0f;   // input level (time-domain RMS)
    float frameDbMin = 0.0f;   // current FFT min/max (for auto-scale + diag)
    float frameDbMax = -120.0f;
    bool  autoScale = true;

    // Spectrum/waterfall view (frequency pan/zoom, in MHz). The waterfall
    // mirrors the spectrum's current X range.
    double viewXminMHz = 0.0;
    double viewXmaxMHz = 0.0;
    bool   resetView = true; // fit X to the full band on next draw

    // Band browsing: panning/scrolling the spectrum past the captured window
    // retunes a live SDR so you can sweep the whole band (not for WAV files).
    bool   bandBrowse = true;
    std::chrono::steady_clock::time_point lastRetune;

    // Last sample rate the decoder manager was configured with; if the source
    // rate changes (e.g. SDR++ server rate switch), the manager + FFT are
    // reconfigured to match.
    double lastConfiguredFs = 0.0;

    // Horizontal insets of the spectrum's plot data area (axis gutters), used
    // to align the waterfall to the spectrum frequency-for-frequency.
    float specLeftInset = 0.0f;
    float specRightInset = 0.0f;

    // Working buffers.
    std::vector<std::complex<double>> iq;
    std::vector<double> window;
    std::vector<float>  inst;   // instantaneous dB (shifted)
    std::vector<float>  avg;    // averaged dB for the line plot
    std::vector<float>  sortbuf; // scratch for percentile auto-scale
    std::vector<float>  freqMHz;
    int curN = 0;
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

static void buildWindow(App& app, int N)
{
    // Blackman window: low sidelobes -> clean noise floor, signals stand out.
    app.window.resize(N);
    for (int i = 0; i < N; ++i)
    {
        double x = 2.0 * M_PI * i / (N - 1);
        app.window[i] = 0.42 - 0.5 * std::cos(x) + 0.08 * std::cos(2.0 * x);
    }
    int nf = N; // init() builds the twiddle tables; must run before fft().
    app.fft.init(nf);
    app.iq.resize(N);
    app.inst.assign(N, app.dbMin);
    app.avg.assign(N, app.dbMin);
    app.freqMHz.resize(N);
    app.curN = N;
}

static void updateFreqAxis(App& app, int N)
{
    bool run = app.active->running();
    double fs = run ? app.active->sampleRate() : kRates[app.sampleRateIdx];
    double fc = run ? app.active->centerFreq() : app.centerFreqMHz * 1e6;
    for (int i = 0; i < N; ++i)
        app.freqMHz[i] = (float)((fc + (i - N / 2) * fs / N) / 1e6);
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

static void processFft(App& app)
{
    int N = kFftSizes[app.fftSizeIdx];
    if (N != app.curN)
        buildWindow(app, N);

    if (!app.ring.latest(app.iq.data(), (size_t)N))
        return;

    // Time-domain input level (dBFS) -- shows whether real RF is arriving.
    double pwr = 0.0;
    for (int i = 0; i < N; ++i)
        pwr += std::norm(app.iq[i]);
    double rms = std::sqrt(pwr / (double)N);
    app.rmsDbfs = (float)(20.0 * std::log10(rms + 1e-12));

    for (int i = 0; i < N; ++i)
        app.iq[i] *= app.window[i];

    app.fft.fft(app.iq.data(), N, JFFT::FORWARD);

    const double invN = 1.0 / (double)N;
    const float alpha = app.avgAlpha;
    for (int i = 0; i < N; ++i)
    {
        int src = (i + N / 2) % N; // fftshift: center DC
        double p = std::norm(app.iq[src]) * invN; // power spectrum
        float db = (float)(10.0 * std::log10(p + 1e-20));
        app.inst[i] = db;
        app.avg[i] = alpha * app.avg[i] + (1.0f - alpha) * db;
    }

    // Hide the unavoidable center artifact (a deep null when DC-blocking, a
    // spike when not) so it doesn't dominate the display or the auto-scale.
    patchDcBins(app.inst, N, 4);
    patchDcBins(app.avg, N, 4);

    // Robust range via percentiles so the colour scale isn't dragged by the
    // DC null (low extreme) or a single freak bin (high extreme).
    app.sortbuf.assign(app.inst.begin(), app.inst.end());
    int iFloor = (int)(0.30 * N);
    int iPeak = (int)(0.995 * N);
    std::nth_element(app.sortbuf.begin(), app.sortbuf.begin() + iFloor, app.sortbuf.end());
    float floorDb = app.sortbuf[iFloor];
    std::nth_element(app.sortbuf.begin(), app.sortbuf.begin() + iPeak, app.sortbuf.end());
    float peakDb = app.sortbuf[iPeak];
    app.frameDbMin = floorDb;
    app.frameDbMax = peakDb;
    if (app.autoScale)
    {
        float tgtMin = floorDb - 6.0f;
        float tgtMax = std::max(peakDb + 12.0f, floorDb + 20.0f);
        app.dbMin = 0.85f * app.dbMin + 0.15f * tgtMin;
        app.dbMax = 0.85f * app.dbMax + 0.15f * tgtMax;
    }

    updateFreqAxis(app, N);
    app.waterfall.addRow(app.inst.data(), N, app.dbMin, app.dbMax);
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
    app.resetView = true;
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
    if (app.curN > 0)
        updateFreqAxis(app, app.curN);
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
    app.resetView = true;
    if (app.curN > 0)
        updateFreqAxis(app, app.curN);
}

// Drives the voice-follow state machine once per frame while a source runs.
static void updateVoiceFollow(App& app)
{
    using clock = std::chrono::steady_clock;
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
            retuneActive(app, rx);
            app.followRetuned = true;
        }

        app.followChannelId = app.decoders.addDecoder(rx * 1e6, 8400);
        if (app.followChannelId < 0)
            return; // failed to spawn (e.g. manager not configured)
        app.decoders.setVoiceMonitor(app.followChannelId);
        app.selectedDecoder = app.followChannelId;
        app.following = true;
        app.followEverLocked = false;
        app.followActivity = clock::now();
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Following voice %.4f MHz", rx);
        app.status = buf;
        return;
    }

    // A follow is active: hold the channel while the voice decoder is locked.
    // End when it has been unlocked for the hold time, then jump back to where
    // the user was (e.g. the 10500 channels they were decoding). A longer grace
    // applies before the first lock so we don't bail during acquisition.
    constexpr double kAcquireSec = 12.0;
    bool locked = false;
    for (auto& s : app.decoders.status())
        if (s.channelId == app.followChannelId)
        {
            locked = s.locked;
            break;
        }
        const auto now = clock::now();
    if (locked)
    {
        app.followActivity = now;
        app.followEverLocked = true;
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

static void startActive(App& app)
{
    app.ring.clear();
    app.waterfall.clear();
    app.resetView = true;

    IqRing* ring = &app.ring;
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
        app.decoders.removeAll();
        app.decoders.configure(app.active->sampleRate(), app.active->centerFreq());
        app.decoders.start();
        app.lastConfiguredFs = app.active->sampleRate();
        // Don't auto-follow assignments left over from a previous session.
        app.followSeenCount = app.decoders.cassignLog().count();
        app.following = false;
        app.followChannelId = -1;
        app.followHome.clear();
    }

    app.status = ok ? "Running" : ("Error: " + err);
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
            app.following = false;
            app.followChannelId = -1;
            app.followHome.clear();
            app.status = "Idle";
        }
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(app.status.c_str());

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
            app.resetView = true;
            if (running)
                app.sdr.setCenterFreq(app.centerFreqMHz * 1e6);
        }
        if (ImGui::Combo("Sample rate (MHz)", &app.sampleRateIdx, kRateLabels, kNumRates))
        {
            app.resetView = true;
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
            app.resetView = true;

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
            app.resetView = true;
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
                            app.resetView = true;
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
            app.resetView = true;
            if (running)
                app.hack.setCenterFreq(app.centerFreqMHz * 1e6);
        }
        if (ImGui::InputDouble("Sample rate (MHz)", &app.hackSampleRateMHz, 1.0, 2.0, "%.3f"))
        {
            if (app.hackSampleRateMHz < 2.0) app.hackSampleRateMHz = 2.0;
            if (app.hackSampleRateMHz > 20.0) app.hackSampleRateMHz = 20.0;
            app.resetView = true;
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
        app.resetView = true;
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

    if (running)
    {
        ImGui::Separator();
        ImGui::Text("  Sample rate:   %.4f MHz", app.active->sampleRate() / 1e6);
        ImGui::Text("  Input level:   %.1f dBFS", app.rmsDbfs);
        ImGui::Text("  Spectrum:      %.0f .. %.0f dB", app.frameDbMin, app.frameDbMax);

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

static void drawSpectrum(App& app)
{
    ImGui::Begin("Spectrum");
    ImVec2 origin = ImGui::GetCursorScreenPos();
    float availW = ImGui::GetContentRegionAvail().x;
    if (ImPlot::BeginPlot("##spectrum", ImVec2(-1, -1)))
    {
        ImPlot::SetupAxes("MHz", "dB", 0, 0);

        // The band is only valid once real frequency data exists (front<back).
        bool bandValid = (app.curN > 0 && app.freqMHz.front() < app.freqMHz.back());

        // Cap zoom-out so the view can never be wider than the captured band
        // (i.e. the sample rate). z_min stays tiny to allow zooming in.
        if (bandValid)
        {
            double bandSpan = app.freqMHz.back() - app.freqMHz.front();
            ImPlot::SetupAxisZoomConstraints(ImAxis_X1, bandSpan * 1e-4, bandSpan);
        }

        // Fit X to the full band only on request (start / tune / reset);
        // otherwise leave it alone so the user's pan/zoom persists.
        if (app.resetView && bandValid)
        {
            ImPlot::SetupAxisLimits(ImAxis_X1, app.freqMHz.front(),
                                    app.freqMHz.back(), ImGuiCond_Always);
        }
        // Y tracks the dB range when auto-scaling or resetting; otherwise the
        // user can pan/zoom it freely.
        if (app.autoScale || app.resetView)
            ImPlot::SetupAxisLimits(ImAxis_Y1, app.dbMin, app.dbMax, ImGuiCond_Always);

        if (app.curN > 0)
            ImPlot::PlotLine("PSD", app.freqMHz.data(), app.avg.data(), app.curN);

        // Draggable vertical markers at each decoder's frequency. Grab and
        // drag to retune that decoder; green = locked, amber = searching.
        auto decs = app.decoders.status();
        for (auto& d : decs)
        {
            double x = d.freqMHz;
            ImVec4 col = d.locked ? ImVec4(0.2f, 1.0f, 0.35f, 1.0f)
                                  : ImVec4(0.9f, 0.7f, 0.2f, 1.0f);
            if (ImPlot::DragLineX(d.channelId, &x, col, 2.0f))
                app.decoders.setDecoderFreq(d.channelId, x * 1e6);
        }

        ImPlotRect lim = ImPlot::GetPlotLimits();
        app.viewXminMHz = lim.X.Min;
        app.viewXmaxMHz = lim.X.Max;

        // Band browsing: if the view has been panned/zoomed so its center drifts
        // outside the captured window, retune a live SDR to follow. Throttled and
        // dead-banded so a steady drag sweeps the radio without thrashing.
        if (app.bandBrowse && app.sourceMode != 1 && app.active->running() &&
            !app.resetView && !app.following)
        {
            double viewCtr = 0.5 * (app.viewXminMHz + app.viewXmaxMHz);
            double sdrCtr = app.active->centerFreq() / 1e6;
            double fsMHz = app.active->sampleRate() / 1e6;
            double deadband = fsMHz * 0.20;
            auto now = std::chrono::steady_clock::now();
            double sinceMs =
                std::chrono::duration<double, std::milli>(now - app.lastRetune).count();
            if (fsMHz > 0.0 && std::fabs(viewCtr - sdrCtr) > deadband && sinceMs > 150.0)
            {
                retunePreserving(app, viewCtr);
                app.lastRetune = now;
            }
        }

        // Inset of the plot's data area within the panel, so the waterfall can
        // align to the exact same horizontal frequency span.
        ImVec2 pp = ImPlot::GetPlotPos();
        ImVec2 ps = ImPlot::GetPlotSize();
        app.specLeftInset = pp.x - origin.x;
        app.specRightInset = (origin.x + availW) - (pp.x + ps.x);

        // Only clear the reset request once a real band has actually been fit.
        if (bandValid)
            app.resetView = false;

        // Ctrl+left-click adds a decoder at the clicked frequency.
        if (ImPlot::IsPlotHovered() && ImGui::GetIO().KeyCtrl &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            ImPlotPoint mp = ImPlot::GetPlotMousePos();
            static const int kBaudVals[] = {600, 1200, 8400, 10500, kEgcBaud};
            int idx = app.newBaud < 0 ? 0 : (app.newBaud > 4 ? 4 : app.newBaud);
            int baud = kBaudVals[idx];
            app.decoders.addDecoder(mp.x * 1e6, baud);
        }

        ImPlot::EndPlot();
    }
    ImGui::End();
}

static void drawWaterfall(App& app)
{
    ImGui::Begin("Waterfall");

    // Map the captured band to the spectrum's current view so the waterfall
    // lines up frequency-for-frequency at any zoom. Both the texture UV range
    // and the on-screen X sub-rectangle follow the visible overlap, so zooming
    // out (view wider than the band) shrinks the image to the middle instead of
    // stretching it across the whole panel.
    float uMin = 0.0f, uMax = 1.0f; // texture sub-range to sample
    float xLo = 0.0f, xHi = 1.0f;   // screen X fractions to draw it across
    if (app.curN > 0)
    {
        double bandMin = app.freqMHz.front();
        double bandMax = app.freqMHz.back();
        double bandSpan = bandMax - bandMin;
        double viewSpan = app.viewXmaxMHz - app.viewXminMHz;
        if (bandSpan > 0.0 && viewSpan > 0.0)
        {
            double visLo = std::max(bandMin, app.viewXminMHz);
            double visHi = std::min(bandMax, app.viewXmaxMHz);
            if (visHi > visLo)
            {
                uMin = (float)((visLo - bandMin) / bandSpan);
                uMax = (float)((visHi - bandMin) / bandSpan);
                xLo = (float)((visLo - app.viewXminMHz) / viewSpan);
                xHi = (float)((visHi - app.viewXminMHz) / viewSpan);
            }
            else
            {
                xLo = xHi = 0.0f; // band entirely off-screen: just background
            }
        }
    }

    // Inset to match the spectrum plot's data area so the frequency axes line
    // up exactly between the two panels.
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float left = std::max(0.0f, app.specLeftInset);
    float right = std::max(0.0f, app.specRightInset);
    float w = avail.x - left - right;
    if (w < 1.0f)
    {
        w = avail.x;
        left = 0.0f;
    }
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + left);

    app.waterfall.draw(ImVec2(w, avail.y), uMin, uMax, xLo, xHi);
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

        for (auto it = msgs.rbegin(); it != msgs.rend(); ++it)
        {
            ImGui::TableNextRow();
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

static void drawCChannel(App& app)
{
    ImGui::Begin("C-Channel");

    ImGui::Text("%llu assignment(s)", (unsigned long long)app.decoders.cassignLog().count());
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear"))
        app.decoders.cassignLog().clear();
    ImGui::Separator();

    auto items = app.decoders.cassignLog().snapshot();
    if (ImGui::BeginTable("##cchan", 5,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable))
    {
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 96);
        ImGui::TableSetupColumn("AES", ImGuiTableColumnFlags_WidthFixed, 64);
        ImGui::TableSetupColumn("GES", ImGuiTableColumnFlags_WidthFixed, 40);
        ImGui::TableSetupColumn("RX / down (MHz)");
        ImGui::TableSetupColumn("TX / up (MHz)");
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (auto it = items.rbegin(); it != items.rend(); ++it)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(cassignTypeName(it->type));
            ImGui::TableNextColumn();
            ImGui::Text("%06X", it->aesId);
            ImGui::TableNextColumn();
            ImGui::Text("%02X", it->gesId);
            ImGui::TableNextColumn();
            ImGui::Text("%.4f", it->rxMHz);
            ImGui::TableNextColumn();
            ImGui::Text("%.4f", it->txMHz);
        }
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

    float m = 0.5f;
    for (float v : app.constBuf)
        m = std::max(m, std::fabs(v));
    double lim = m * 1.15;

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

static void drawDockHost()
{
    static bool forceLayout = true;

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
    ImGui::Begin("##FrameSyncHost", nullptr, hostFlags);
    ImGui::PopStyleVar(3);

    ImGuiID dockId = ImGui::GetID("FrameSyncDockSpace");
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
        // Right column: Spectrum (short, top) / Waterfall (middle) / bottom row.
        ImGui::DockBuilderSplitNode(right, ImGuiDir_Up, 0.30f, &rtop, &rrest);
        ImGui::DockBuilderSplitNode(rrest, ImGuiDir_Up, 0.58f, &rmid, &rbot);
        // Bottom row: Messages (left) | Constellation (right).
        ImGui::DockBuilderSplitNode(rbot, ImGuiDir_Right, 0.34f, &rcon, &rbot);

        ImGui::DockBuilderDockWindow("Control", ctrl);
        ImGui::DockBuilderDockWindow("Decoders", dec);
        ImGui::DockBuilderDockWindow("Spectrum", rtop);
        ImGui::DockBuilderDockWindow("Waterfall", rmid);
        ImGui::DockBuilderDockWindow("SUs", rbot);
        ImGui::DockBuilderDockWindow("Messages", rbot);
        ImGui::DockBuilderDockWindow("C-Channel", rbot);
        ImGui::DockBuilderDockWindow("Network", rbot);
        ImGui::DockBuilderDockWindow("EGC", rbot);
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

    GLFWwindow* window = glfwCreateWindow(1400, 900, "FrameSync", nullptr, nullptr);
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
    buildWindow(app, kFftSizes[app.fftSizeIdx]);
    app.devices = app.sdr.listDevices();

    const ImVec4 clear_color = ImVec4(0.06f, 0.07f, 0.09f, 1.0f);

    while (!glfwWindowShouldClose(window))
    {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        drawDockHost();

        if (app.active->running())
            processFft(app);

        if (app.active->running())
            updateVoiceFollow(app);

        if (app.active->running())
            updateRateChange(app);

        drawControls(app);
        drawSpectrum(app);
        drawWaterfall(app);
        drawDecoders(app);
        drawSUs(app);
        drawMessages(app);
        drawCChannel(app);
        drawNetwork(app);
        drawEgc(app);
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

    app.decoders.stop();
    app.sdr.stop();
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
