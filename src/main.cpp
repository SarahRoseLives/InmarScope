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
#include "sdr/wav_file_source.h"
#include "decode/decoder_manager.h"
#include "gui/waterfall.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <string>
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
    SdrSource* active = &sdr;   // currently selected/running source
    int  sourceMode = 0;        // 0 = RTL-SDR, 1 = WAV file
    char wavPath[512] = "";
    bool wavLoop = true;

    IqRing ring{1u << 21};
    JFFT fft;
    Waterfall waterfall;
    DecoderManager decoders;
    int newBaud = 1; // 0 = 600, 1 = 1200 (baud for click-added decoders)
    int selectedDecoder = -1;     // channelId shown in the constellation panel
    std::vector<float> constBuf;  // interleaved I,Q scratch for the plot

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

static const int kFftSizes[] = {1024, 2048, 4096, 8192, 16384};
static const char* kFftLabels[] = {"1024", "2048", "4096", "8192", "16384"};
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
    else
    {
        app.active = &app.wav;
        app.wav.setPath(app.wavPath);
        app.wav.setLoop(app.wavLoop);
        app.wav.setCenterFreq(app.centerFreqMHz * 1e6);
        ok = app.wav.start(0, cb, err);
    }

    if (ok)
    {
        app.decoders.removeAll();
        app.decoders.configure(app.active->sampleRate(), app.active->centerFreq());
        app.decoders.start();
    }

    app.status = ok ? "Running" : ("Error: " + err);
}

static void drawControls(App& app)
{
    ImGui::Begin("Control");

    bool running = app.active->running();

    ImGui::BeginDisabled(running);
    const char* modes[] = {"RTL-SDR", "WAV file"};
    ImGui::Combo("Source", &app.sourceMode, modes, 2);
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
    else
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

    ImGui::Separator();
    const char* bauds[] = {"600", "1200", "8400", "10500"};
    ImGui::Combo("Decode baud", &app.newBaud, bauds, 4);
    ImGui::TextDisabled("Ctrl+click the spectrum to add a decoder there");

    if (running)
    {
        ImGui::Separator();
        if (app.sourceMode == 0)
        {
            ImGui::TextUnformatted("Radio status:");
            ImGui::Text("  Tuner:         %s", app.sdr.tunerType().c_str());
            ImGui::Text("  Requested:     %.4f MHz", app.centerFreqMHz);
            ImGui::Text("  Actual center: %.4f MHz", app.sdr.actualCenterFreq() / 1e6);
        }
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
            static const int kBaudVals[] = {600, 1200, 8400, 10500};
            int idx = app.newBaud < 0 ? 0 : (app.newBaud > 3 ? 3 : app.newBaud);
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

    float uMin = 0.0f, uMax = 1.0f;
    if (app.curN > 0)
    {
        double bandMin = app.freqMHz.front();
        double bandMax = app.freqMHz.back();
        double span = bandMax - bandMin;
        if (span > 0.0)
        {
            uMin = (float)std::clamp((app.viewXminMHz - bandMin) / span, 0.0, 1.0);
            uMax = (float)std::clamp((app.viewXmaxMHz - bandMin) / span, 0.0, 1.0);
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

    app.waterfall.draw(ImVec2(w, avail.y), uMin, uMax);
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
            ImGui::Text("%d", d.baud);
            ImGui::TableNextColumn();
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

// Full-screen host window holding a locked dockspace. Builds a default
// layout on first run (Control left, Spectrum top-right, Waterfall
// bottom-right). Panels can't be undocked or closed; "View > Reset Layout"
// restores the default.
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
        ImGui::DockBuilderSplitNode(dockId, ImGuiDir_Left, 0.26f, &left, &right);
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

        drawControls(app);
        drawSpectrum(app);
        drawWaterfall(app);
        drawDecoders(app);
        drawSUs(app);
        drawMessages(app);
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

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
