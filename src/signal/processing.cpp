#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"
#include <GLFW/glfw3.h>
#include "core/app.h"
#include "core/main_funcs.h"
#include "decode/icao_country.h"
#include "util/log.h"
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
#include <shellapi.h>
#endif

void buildWindow(SpectrumView& v, int N, float initDb)
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

void updateFreqAxis(SpectrumView& v, double fc, double fs, int N)
{
    for (int i = 0; i < N; ++i)
        v.freqMHz[i] = (float)((fc + (i - N / 2) * fs / N) / 1e6);
}

// After DC removal the exact center (DC) bin is a deep null. Cover the few
// center bins with a copy of adjacent bins so it doesn't show as a hole.
void patchDcBins(std::vector<float>& a, int N, int w)
{
    int c = N / 2;
    int span = 2 * w + 1;
    int src = c - w - span; // equal-width band just left of the gap
    if (src < 0 || c + w >= N)
        return;
    for (int k = 0; k < span; ++k)
        a[c - w + k] = a[src + k];
}

void processFft(SpectrumView& v, App& app, double fc, double fs)
{
    if (v.fftSkip)
        return;

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
    v.fftSkip = true; // cleared by drawWaterfall / drawSpectrum when panel is visible
}

// C-channel assignment types that carry a voice call (0x31 distress .. 0x34
// non-safety). All of these point at a forward-link RX voice carrier.

void updateRateChange(App& app)
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
    app.iqRecorder.configurePrebuffer(fs, app.iqBufferSec);
    app.viewA.resetView = true;
    if (app.viewA.curN > 0)
        updateFreqAxis(app.viewA, center, fs, app.viewA.curN);
}

// Drives the voice-follow state machine once per frame while a source runs.
