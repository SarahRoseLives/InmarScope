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

void cfgWriteAll(App& app, ImGuiTextBuffer* buf)
{
    buf->append("[InmarScope][State]\n");
#define WI(f) buf->appendf(#f "=%d\n", (int)app.f)
#define WF(f) buf->appendf(#f "=%g\n", (double)app.f)
#define WD(f) buf->appendf(#f "=%.10g\n", (double)app.f)
#define WS(f) buf->appendf(#f "=%s\n", app.f)
    WI(sourceMode); WI(deviceIndex); WI(sampleRateIdx); WI(newBaud); WI(fftSizeIdx);
    WI(audioDevice); WI(voiceMuted);
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
    WS(recordDir);
    WI(recordFormat);
    WI(saveDecoders);
    WI(acPosOnly);
    WI(showEmptyMsgs);
    WI(showBandPlan); WI(bandPlanIdx); WS(bandPlanDir);
    WI(outFile); WS(outFilePath); WI(outUdp); WS(outUdpHost); WI(outUdpPort);
    WI(outFormat); WS(outStation); WI(outSbs); WI(outSbsPort);
    WI(layoutVersion);
    for (auto& cc : app.blacklistCountries)
        buf->appendf("blacklistCC=%s\n", cc.c_str());
    if (app.saveDecoders)
    {
        for (auto& st : app.decoders.status())
            if (st.baud != 8400)
                buf->appendf("savedDecoder=%.3f,%d\n", st.freqMHz, st.baud);
        for (auto& st : app.decodersB.status())
            if (st.baud != 8400)
                buf->appendf("savedDecoder=%.3f,%d\n", st.freqMHz, st.baud);
    }
    buf->append("\n");
#undef WI
#undef WF
#undef WD
#undef WS
}

void cfgReadLine(App& app, const char* line)
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
    RI(audioDevice); RB(voiceMuted);
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
    RS(recordDir);
    RI(recordFormat); RB(saveDecoders);
    // savedDecoder lines: freqMHz,baud (e.g. "1545.020,600")
    if (!std::strcmp(key, "savedDecoder"))
    {
        double f = 0.0; int b = 0;
        if (std::sscanf(val, "%lf,%d", &f, &b) == 2 && f > 0.0 && b > 0)
            app.savedDecoders.push_back({f, b});
        return;
    }
    RB(acPosOnly);
    RB(showEmptyMsgs);
    RB(showBandPlan); RI(bandPlanIdx); RS(bandPlanDir);
    RB(outFile); RS(outFilePath); RB(outUdp); RS(outUdpHost); RI(outUdpPort);
    RI(outFormat); RS(outStation); RB(outSbs); RI(outSbsPort);
    RI(layoutVersion);
    if (!std::strcmp(key, "blacklistCC") && val[0] && val[1] && !val[2])
    {
        std::string cc(val, 2);
        auto& bl = app.blacklistCountries;
        if (std::find(bl.begin(), bl.end(), cc) == bl.end())
            bl.push_back(cc);
        return;
    }
#undef RI
#undef RB
#undef RF
#undef RD
#undef RS
}

void cfgRegisterHandler(App& app)
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
