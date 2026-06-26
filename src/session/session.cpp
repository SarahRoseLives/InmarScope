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

void startActive(App& app)
{
    app.viewA.ring.clear();
    app.viewA.waterfall.clear();
    app.viewA.resetView = true;

    IqRing* ring = &app.viewA.ring;
    DecoderManager* mgr = &app.decoders;
    IqRecorder* iqr = &app.iqRecorder;
    auto cb = [ring, mgr, iqr](const float* iq, int n) {
        ring->push(iq, (size_t)n);
        mgr->feed(iq, n);
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

        // Restore saved decoders (non-8400 only, from inmarscope.ini)
        if (app.saveDecoders && !app.savedDecoders.empty())
        {
            for (auto& sd : app.savedDecoders)
                app.decoders.addDecoder(sd.first * 1e6, sd.second);
        }
        if (!app.saveDecoders)
            app.savedDecoders.clear();
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

    if (ok)
        app.status = app.dualMode ? "Running (dual SDR)" : "Running";
    else
        app.status = "Error: " + err;
}

