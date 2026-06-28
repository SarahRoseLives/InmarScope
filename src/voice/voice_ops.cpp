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

bool isVoiceAssign(uint8_t type)
{
    return type >= 0x31 && type <= 0x34;
}

// Returns true if the aircraft is from a blacklisted country.
static bool voiceAesBlacklisted(const App& app, uint32_t aesId)
{
    if (app.blacklistCountries.empty() || aesId == 0)
        return false;
    std::string icao = app.decoders.aircraftTable().icao(aesId);
    if (icao.empty())
        return false;
    uint32_t ihex = (uint32_t)std::strtoul(icao.c_str(), nullptr, 16);
    const char* cc = icaoCountry(ihex);
    if (!cc)
        return false;
    auto& bl = app.blacklistCountries;
    return std::find(bl.begin(), bl.end(), std::string(cc)) != bl.end();
}

// Retune the active (live) source to a new center and re-point the decoder
// manager there. Wipes all decoders -- callers restore what they need.
void retuneActive(App& app, double centerMHz)
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
#ifdef HAS_AIRSPY
    else if (app.sourceMode == 5)
        app.airspy.setCenterFreq(hz);
#endif
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
void retunePreserving(App& app, double centerMHz)
{
    std::vector<std::pair<double, int>> keep;
    for (auto& s : app.decoders.status())
        keep.push_back({s.freqMHz, s.baud});

    double hz = centerMHz * 1e6;
    app.centerFreqMHz = centerMHz;
    if (app.sourceMode == 0 || app.sourceMode == 4)
        app.sdr.setCenterFreq(hz);
    else if (app.sourceMode == 2)
        app.server.setCenterFreq(hz);
    else if (app.sourceMode == 3)
        app.hack.setCenterFreq(hz);
#ifdef HAS_AIRSPY
    else if (app.sourceMode == 5)
        app.airspy.setCenterFreq(hz);
#endif
    app.decoders.removeAll();
    app.decoders.configure(app.active->sampleRate(), hz);
    for (auto& k : keep)
        app.decoders.addDecoder(k.first * 1e6, k.second);
}

// If the source's sample rate changes (e.g. an SDR++ server rate switch),
// reconfigure the decoder manager + FFT axis to match, keeping the decoders.

void updateVoiceFollow(App& app)
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
            if (voiceAesBlacklisted(app, pick->aesId))
                return; // blacklisted country — don't record, don't monitor
            double rx = pick->rxMHz;
            double fsBMHz = app.sdrB.sampleRate() / 1e6;
            double offB = std::min(0.2, 0.25 * fsBMHz);
            double bctr = rx - offB;
            app.sdrB.setCenterFreq(bctr * 1e6);
            app.decodersB.removeAll();
            app.decodersB.configure(app.sdrB.sampleRate(), bctr * 1e6);
            app.viewB.resetView = true;
            if (app.viewB.curN > 0)
                updateFreqAxis(app.viewB, bctr * 1e6, app.sdrB.sampleRate(), app.viewB.curN);
            app.followChannelId = app.decodersB.addDecoder(rx * 1e6, 8400, pick->aesId);
            if (app.followChannelId < 0) return;
            app.decodersB.setVoiceMonitor(app.followChannelId);
            app.centerFreqMHzB = bctr; // park B here when the call ends
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
            // SDR B stays parked + streaming at centerFreqMHzB for its waterfall.
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

        if (voiceAesBlacklisted(app, pick->aesId))
            return; // blacklisted — don't record, don't monitor

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

        app.followChannelId = app.decoders.addDecoder(rx * 1e6, 8400, pick->aesId);
        if (app.followChannelId < 0)
            return;
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

void tuneToVoice(App& app, double rxMHz, uint32_t aesId)
{
    if (rxMHz <= 1.0 || !app.active->running())
        return;
    if (voiceAesBlacklisted(app, aesId))
        return; // blacklisted — don't spawn
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
    int id = app.decoders.addDecoder(rxMHz * 1e6, 8400, aesId);
    if (id >= 0)
    {
        app.decoders.setVoiceMonitor(id);
        app.selectedDecoder = id;
    }
}


void updateCallHunter(App& app)
{
    if (!app.callHunterMode)
    {
        app.callHunterWarmup = 0;
        app.callHunterBaseline.clear();
        return;
    }
    if (app.following)
        return;
    if (!app.active->running())
        return;

    SpectrumView& v = app.viewA;
    if (v.curN <= 0 || (int)v.avg.size() < v.curN)
        return;

    // Reset the baseline when FFT size or SDR centre changes.
    double curCenter = app.active->centerFreq();
    if (app.callHunterBaseline.empty() ||
        (int)app.callHunterBaseline.size() != v.curN ||
        std::abs(curCenter - app.callHunterLastCenter) > 1e3)
    {
        app.callHunterBaseline.assign(v.avg.begin(), v.avg.begin() + v.curN);
        app.callHunterWarmup = 60; // ~3 s at 20 FPS — let the baseline settle
        app.callHunterLastCenter = curCenter;
    }

    // Slow EMA update of the baseline so background signals fade in gradually
    // but transient peaks (new voice calls) don't pull it up.  Bins under an
    // already‑spawned CallHunter decoder are frozen — otherwise the baseline
    // would absorb the signal and the decoder would be removed prematurely.
    std::vector<uint8_t> freeze(v.curN, 0);
    for (auto& c : app.callHunterCands)
    {
        if (c.channelId >= 0)
        {
            int lo = 0, hi = v.curN - 1;
            // Freeze bins within ±3 kHz of the spawned decoder.
            for (int i = 0; i < v.curN; ++i)
            {
                if (std::abs(v.freqMHz[i] - c.freqMHz) < 0.003)
                    freeze[i] = 1;
            }
        }
    }
    for (int i = 0; i < v.curN; ++i)
        if (!freeze[i])
            app.callHunterBaseline[i] = 0.999f * app.callHunterBaseline[i] + 0.001f * v.avg[i];

    // Still settling — don't flag anything yet or the 90 existing signals fire.
    if (app.callHunterWarmup > 0)
    {
        --app.callHunterWarmup;
        return;
    }

    // Only hunt within the intersection of the user's view and the captured band.
    double bandMin = v.freqMHz.front();
    double bandMax = v.freqMHz.back();
    if (bandMax <= bandMin)
        return;
    double visMin = std::max(bandMin, v.viewXminMHz);
    double visMax = std::min(bandMax, v.viewXmaxMHz);
    if (visMax <= visMin)
        return;
    int iLo = (int)((visMin - bandMin) / (bandMax - bandMin) * v.curN);
    int iHi = (int)((visMax - bandMin) / (bandMax - bandMin) * v.curN);
    iLo = std::clamp(iLo, 0, v.curN);
    iHi = std::clamp(iHi, 0, v.curN);
    if (iHi - iLo < 4)
        return;

    // Find peaks in the *difference* between current PSD and baseline.
    float thresh = app.callHunterThreshDB;
    float valleyThresh = thresh + 1.0f; // peaks must dip near baseline to split
    // Minimum peak width: real 8400 voice is ~10 kHz wide, noise spikes are
    // only a few bins.  Require at least ~3 kHz to reject narrow false positives.
    double binResHz = (bandMax - bandMin) * 1e6 / v.curN; // Hz per FFT bin
    int minWidthBins = std::max(1, (int)(3000.0 / binResHz));
    struct Peak { double f; float vv; float vdiff; };//}}
    std::vector<Peak> peaks;
    {
        int start = -1;
        for (int i = iLo; i < iHi; ++i)
        {
            float diff = v.avg[i] - app.callHunterBaseline[i];
            bool above = (diff >= thresh);
            if (above && start < 0) start = i;
            if ((!above || i == iHi - 1) && start >= 0)
            {
                int end = (!above) ? i : iHi;
                int wBins = end - start;
                if (wBins < minWidthBins)
                    start = -1; // too narrow — noise spike
                else if (wBins > minWidthBins * 2)
                {
                    // Wide block — scan for a valley to split adjacent calls.
                    int bestSplit = -1;
                    float bestValley = 1e9f;
                    int margin = minWidthBins / 2;
                    for (int k = start + margin; k < end - margin; ++k)
                    {
                        float dk = v.avg[k] - app.callHunterBaseline[k];
                        if (dk < bestValley) { bestValley = dk; bestSplit = k; }
                    }
                    if (bestSplit >= 0 && bestValley < valleyThresh)
                    {
                        // Split into two peaks at the valley.
                        double sumF1 = 0, sumW1 = 0; float bestV1 = -999;
                        for (int k = start; k < bestSplit; ++k)
                        {
                            float dk = v.avg[k] - app.callHunterBaseline[k];
                            if (dk < 0) dk = 0;
                            double w = std::pow(10.0, dk / 10.0);
                            sumF1 += v.freqMHz[k] * w;
                            sumW1 += w;
                            if (dk > bestV1) bestV1 = dk;
                        }
                        if (sumW1 > 0.0)
                            peaks.push_back({sumF1 / sumW1, bestV1, bestV1});
                        double sumF2 = 0, sumW2 = 0; float bestV2 = -999;
                        for (int k = bestSplit; k < end; ++k)
                        {
                            float dk = v.avg[k] - app.callHunterBaseline[k];
                            if (dk < 0) dk = 0;
                            double w = std::pow(10.0, dk / 10.0);
                            sumF2 += v.freqMHz[k] * w;
                            sumW2 += w;
                            if (dk > bestV2) bestV2 = dk;
                        }
                        if (sumW2 > 0.0)
                            peaks.push_back({sumF2 / sumW2, bestV2, bestV2});
                    }
                    else
                    {
                        double sumF = 0, sumW = 0; float bestV = -999;
                        for (int k = start; k < end; ++k)
                        {
                            float dk = v.avg[k] - app.callHunterBaseline[k];
                            if (dk < 0) dk = 0;
                            double w = std::pow(10.0, dk / 10.0);
                            sumF += v.freqMHz[k] * w;
                            sumW += w;
                            if (dk > bestV) bestV = dk;
                        }
                        if (sumW > 0.0)
                            peaks.push_back({sumF / sumW, bestV, bestV});
                    }
                    start = -1;
                }
                else
                {
                    double sumF = 0, sumW = 0; float bestV = -999;
                    for (int k = start; k < end; ++k)
                    {
                        float dk = v.avg[k] - app.callHunterBaseline[k];
                        if (dk < 0) dk = 0;
                        double w = std::pow(10.0, dk / 10.0);
                        sumF += v.freqMHz[k] * w;
                        sumW += w;
                        if (dk > bestV) bestV = dk;
                    }
                    if (sumW > 0.0)
                        peaks.push_back({sumF / sumW, bestV, bestV});
                    start = -1;
                }
            }
        }
    }

    // Match peaks to existing candidates.
    // Unspawned candidates: ±3 kHz.  Spawned decoders: ±750 Hz only —
    // a peak further from a spawned decoder is a *different* call and must
    // NOT be merged into the spawned candidate (or it steals the confirm).
    const double kSearchWide = 0.003;
    const double kSearchSpawn = 0.00075;
    for (auto& c : app.callHunterCands)
        c.matched = false;
    for (auto& p : peaks)
    {
        bool found = false;
        // Pass 1: match to unspawned candidates (±3 kHz).
        for (auto& c : app.callHunterCands)
        {
            if (c.channelId >= 0) continue;
            if (std::abs(p.f - c.freqMHz) < kSearchWide)
            {
                c.freqMHz = 0.7 * c.freqMHz + 0.3 * p.f;
                c.confirmCount++;
                c.lostCount = 0;
                c.matched = true;
                c.peakDB = p.vv;
                found = true;
                break;
            }
        }
        // Pass 2: if not matched, try spawned candidates (±750 Hz only).
        if (!found)
        {
            for (auto& c : app.callHunterCands)
            {
                if (c.channelId < 0) continue;
                if (std::abs(p.f - c.freqMHz) < kSearchSpawn)
                {
                    c.freqMHz = 0.7 * c.freqMHz + 0.3 * p.f;
                    c.confirmCount++;
                    c.lostCount = 0;
                    c.matched = true;
                    c.peakDB = p.vv;
                    found = true;
                    break;
                }
            }
        }
        if (!found)
        {
            CallHunterCand c;
            c.freqMHz = p.f;
            c.peakDB = p.vv;
            c.confirmCount = 1;
            c.matched = true;
            app.callHunterCands.push_back(c);
        }
    }

    for (auto& c : app.callHunterCands)
    {
        if (!c.matched)
        {
            // Spawned decoders: don't mark as lost if the absolute signal level
            // at this frequency is still strong (in case the peak was missed for
            // any reason, e.g. baseline hasn't fully frozen).
            if (c.channelId >= 0)
            {
                // Find the nearest bin and check absolute PSD vs baseline.
                int bi = -1; double bd = 1e9;
                for (int i = 0; i < v.curN; ++i)
                {
                    double d = std::abs(v.freqMHz[i] - c.freqMHz);
                    if (d < bd) { bd = d; bi = i; }
                }
                if (bi >= 0 && (v.avg[bi] - app.callHunterBaseline[bi] >= 3.0f))
                    continue; // signal still present — don't count as lost
            }
            c.confirmCount = 0;
            c.lostCount++;
        }
    }

    const double kDecoderCover = 0.0010;
    auto hasExistingDecoder = [&](double f) {
        for (auto& s : app.decoders.status())
            if (std::abs(s.freqMHz - f) < kDecoderCover) return true;
        if (app.dualMode)
            for (auto& s : app.decodersB.status())
                if (std::abs(s.freqMHz - f) < kDecoderCover) return true;
        return false;
    };

    for (auto& c : app.callHunterCands)
    {
        if (c.channelId < 0 && c.confirmCount >= app.callHunterConfirm)
        {
            if (hasExistingDecoder(c.freqMHz))
            {
                c.channelId = -2;
                continue;
            }
            int id = app.decoders.addDecoder(c.freqMHz * 1e6, 8400);
            if (id >= 0)
                c.channelId = id;
        }
    }

    for (size_t j = 0; j < app.callHunterCands.size();)
    {
        auto& c = app.callHunterCands[j];
        if (c.channelId >= 0 && c.lostCount >= app.callHunterLost)
        {
            app.decoders.removeDecoder(c.channelId);
            app.callHunterCands.erase(app.callHunterCands.begin() + j);
            continue;
        }
        if (c.channelId == -2 || (c.channelId < 0 && c.lostCount > app.callHunterLost * 3))
        {
            app.callHunterCands.erase(app.callHunterCands.begin() + j);
            continue;
        }
        ++j;
    }
}

