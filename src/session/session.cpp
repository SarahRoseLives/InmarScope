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

void updateFeed(App& app)
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
    auto& llog = app.decoders.lesLog();
    uint64_t lt = llog.count();
    if (lt > app.lastLesFed)
    {
        auto snap = llog.snapshot();
        uint64_t newN = lt - app.lastLesFed;
        if (newN > snap.size()) newN = snap.size();
        for (size_t i = snap.size() - (size_t)newN; i < snap.size(); ++i)
            app.feed.feedLes(snap[i]);
        app.lastLesFed = lt;
    }
}

void tuneBandPlan(App& app, bool second, double centerMHz)
{
    if (app.sourceMode == 1 || !std::isfinite(centerMHz) || centerMHz <= 0) return;
    auto* source = second ? app.activeB : app.active;
    auto& view = second ? app.viewB : app.viewA;
    auto& manager = second ? app.decodersB : app.decoders;
    auto& frequency = second ? app.centerFreqMHzB : app.centerFreqMHz;
    frequency = centerMHz;
    if (source->running()) {
        std::vector<std::pair<double, int>> keep;
        for (const auto& decoder : manager.status()) keep.push_back({decoder.freqMHz, decoder.baud});
        source->setCenterFreq(centerMHz * 1e6);
        frequency = source->centerFreq() / 1e6;
        manager.removeAll();
        manager.configure(source->sampleRate(), source->centerFreq());
        for (const auto& decoder : keep) manager.addDecoder(decoder.first * 1e6, decoder.second);
        view.ring.clear(); view.waterfall.clear();
        buildWindow(view, kFftSizes[app.fftSizeIdx], app.dbMin);
        updateFreqAxis(view, source->centerFreq(), source->sampleRate(), view.curN);
        // An explicit tuning choice supersedes the old voice-follow return point.
        app.following = false; app.followChannelId = -1; app.followHome.clear();
        app.followSeenCount = app.decoders.cassignLog().count();
    }
    view.resetView = true; view.fftSkip = false;
}

void startActive(App& app)
{
    app.activeB->stop();
    app.active->stop();
    app.decoders.stop();
    app.decodersB.stop();
    app.dualMode = false;
    app.activeB = &app.sdrB;
    app.viewA.ring.clear();
    app.viewA.waterfall.clear();
    app.viewA.resetView = true;

    IqRing* ring = &app.viewA.ring;
    DecoderManager* mgr = &app.decoders;
    IqRecorder* iqr = &app.iqRecorder;
    auto cb = [ring, mgr, iqr](const float* iq, int n) {
        ring->push(iq, (size_t)n);
        mgr->feed(iq, n);
        iqr->prebuffer(iq, n);
        if (iqr->isRecording())
            iqr->write(iq, n);
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
    else if (app.sourceMode == 4)
    {
        // Dual RTL: RTL A uses same config as mode 0
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
    else if (app.sourceMode == 3)
    {
        app.active = &app.hack;
        app.hack.setSampleRate(app.hackSampleRateMHz * 1e6);
        app.hack.setCenterFreq(app.centerFreqMHz * 1e6);
        app.hack.setLnaGain(app.hackLna);
        app.hack.setVgaGain(app.hackVga);
        app.hack.setAmpEnable(app.hackAmp);
        app.hack.setBiasTee(app.hackBias);
        app.hack.setPpm((double)app.ppm);
        app.hack.setDcBlock(app.dcBlock);
        ok = app.hack.start(app.deviceIndex, cb, err);
    }
#ifdef HAS_AIRSPY
    else if (app.sourceMode == 5)
    {
        app.active = &app.airspy;
        app.airspy.setSampleRate(kAirspyRates[app.airspySampleRateIdx]);
        app.airspy.setCenterFreq(app.centerFreqMHz * 1e6);
        app.airspy.setGainMode(app.airspyGainMode);
        app.airspy.setSenseGain(app.airspySenseGain);
        app.airspy.setLinearGain(app.airspyLinearGain);
        app.airspy.setLnaGain(app.airspyLnaGain);
        app.airspy.setMixerGain(app.airspyMixerGain);
        app.airspy.setVgaGain(app.airspyVgaGain);
        app.airspy.setLnaAgc(app.airspyLnaAgc);
        app.airspy.setMixerAgc(app.airspyMixerAgc);
        app.airspy.setBiasTee(app.airspyBias);
        app.airspy.setPpm((double)app.ppm);
        app.airspy.setDcBlock(app.dcBlock);
        ok = app.airspy.start(app.deviceIndex, cb, err);
    }
#endif
    else if (app.sourceMode == 6)
    {
        // RTL-TCP network source
        app.active = &app.rtltcp;
        app.rtltcp.setHost(app.rtlTcpHost);
        app.rtltcp.setPort((uint16_t)app.rtlTcpPort);
        app.rtltcp.setSampleRate(kRates[app.sampleRateIdx]);
        app.rtltcp.setCenterFreq(app.centerFreqMHz * 1e6);
        app.rtltcp.setGain(app.autoGain ? -1.0 : (double)app.gainDb);
        app.rtltcp.setBiasTee(app.biasTee);
        app.rtltcp.setPpm((double)app.ppm);
        ok = app.rtltcp.start(0, cb, err);
    }

    else if (app.sourceMode == 7)
    {
        app.rspB.close(); app.rsp.close();
        app.active = &app.rsp;
        app.activeB = &app.rspB;
        if (app.rspSecond == 2) app.rspConfig.mode = "MA";
        app.rsp.setCenterFreq(app.centerFreqMHz * 1e6);
        ok = app.rsp.prepare(app.rspConfig, err) && app.rsp.apply(app.rspConfig, err)
             && app.rsp.start(0, cb, err);
    }
    else err = "The selected source is not available in this build.";

    if (ok)
    {
        // Dual RTL mode: start second RTL with independent tuning
        bool startedB = false;
        if (app.sourceMode == 4)
        {
            app.viewB.ring.clear();
            app.viewB.waterfall.clear();
            app.viewB.resetView = true;
            IqRing* ringB = &app.viewB.ring;
            DecoderManager* mgrB = &app.decodersB;
            IqRecorder* iqr = &app.iqRecorder;
            auto cbB = [ringB, mgrB, iqr](const float* iq, int n) {
                ringB->push(iq, (size_t)n);
                mgrB->feed(iq, n);
                iqr->prebuffer(iq, n);
                if (iqr->isRecording())
                    iqr->write(iq, n);
            };
            app.sdrB.setSampleRate(kRates[app.sampleRateIdxB]);
            app.sdrB.setCenterFreq(app.centerFreqMHzB * 1e6);
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
                app.decodersB.setMaxWorkers(2);
                app.decodersB.setRecording(app.recordVoice, app.recordDir);
                app.decodersB.start();
            }
            else
                app.status = "Dual RTL B error: " + errB;
        }
        if (app.sourceMode == 7 && app.rspSecond != 0)
        {
            app.viewB.ring.clear(); app.viewB.waterfall.clear(); app.viewB.resetView = true;
            if (app.rspSecond == 2) {
                app.rspConfigB.serial = app.rspConfig.serial;
                app.rspConfigB.mode = "SL";
                app.rspConfigB.rate = app.rsp.sampleRate();
            }
            app.rspB.setCenterFreq(app.centerFreqMHzB * 1e6);
            std::string errorB;
            if (app.rspSecond == 1 && app.rspConfig.serial == app.rspConfigB.serial)
                errorB = "Choose different serial numbers, or select RSPduo two tuners.";
            else {
                auto cbB = [&app](const float* iq, int n) {
                    app.viewB.ring.push(iq, size_t(n)); app.decodersB.feed(iq, n);
                    // The single IQ recorder belongs to A; never interleave two radios.
                };
                startedB = app.rspB.prepare(app.rspConfigB, errorB)
                    && app.rspB.apply(app.rspConfigB, errorB) && app.rspB.start(0, cbB, errorB);
            }
            if (startedB) {
                app.decodersB.removeAll();
                app.decodersB.configure(app.rspB.sampleRate(), app.rspB.centerFreq());
                app.decodersB.setMaxWorkers(2);
                app.decodersB.setRecording(app.recordVoice, app.recordDir);
                app.decodersB.start();
            } else {
                app.rspB.close(); app.rsp.close();
                err = "SDRplay B: " + errorB; ok = false;
            }
        }
        app.dualMode = startedB;

        if (ok)
        {
            app.decoders.removeAll();
            app.decoders.configure(app.active->sampleRate(), app.active->centerFreq());
            app.decoders.setAudioEnabled(true); // A keeps audio in dual mode (both SDRs have voice capability)
            if (app.dualMode)
                app.decoders.setMaxWorkers(4); // cap primary workers in dual mode (B gets 2)
            app.decoders.start();
            app.lastConfiguredFs = app.active->sampleRate();
            app.iqRecorder.configurePrebuffer(app.active->sampleRate(), app.iqBufferSec);
            // Don't auto-follow assignments left over from a previous session.
            app.followSeenCount = app.decoders.cassignLog().count();
            app.following = false;
            app.followChannelId = -1;
            app.followHome.clear();

            // Restore saved decoders (non-8400 only, from inmarscope.ini)
            if (app.saveDecoders && !app.savedDecoders.empty())
            {
                for (auto& sd : app.savedDecoders)
                    app.decoders.addDecoder(sd.first * 1e6, sd.second);
            }
            if (app.dualMode && app.saveDecoders && !app.savedDecodersB.empty())
            {
                for (auto& sd : app.savedDecodersB)
                    app.decodersB.addDecoder(sd.first * 1e6, sd.second);
            }
            if (!app.saveDecoders)
            {
                app.savedDecoders.clear();
                app.savedDecodersB.clear();
            }
        }
    }

    // If the IQ recorder was active, restart it with the new sample rate
    // so the WAV header matches the actual capture rate.
    bool wasIqRec = app.iqRecorder.isRecording();
    if (wasIqRec)
    {
        app.iqRecorder.stop();
        if (ok)
            app.iqRecorder.start(app.iqRecPath, app.active->sampleRate());
    }

    if (ok) {
        // Start is handled after processFft in the GUI frame. Publish the NEW
        // receiver range now, before drawSpectrum consumes resetView.
        auto reset = [&](SpectrumView& view, SdrSource* source) {
            buildWindow(view, kFftSizes[app.fftSizeIdx], app.dbMin);
            updateFreqAxis(view, source->centerFreq(), source->sampleRate(), view.curN);
            view.resetView = true;
            view.fftSkip = false;
        };
        reset(app.viewA, app.active);
        if (app.dualMode) reset(app.viewB, app.activeB);
        app.status = app.dualMode ? "Running (dual SDR)" : "Running";
    }
    else
        app.status = "Error: " + err;
}

