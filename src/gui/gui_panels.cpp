#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"
#include <GLFW/glfw3.h>
#include "core/app.h"
#include "core/main_funcs.h"
#include "decode/icao_country.h"
#include "decode/band_plan.h"
#include "decode/acars_explain.h"
#include "stt/transcriber.h"
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include "i18n/i18n.h"
#include "util/log.h"
#include "version.h"
#include "gui/waterfall.h"
#include "gui/waterfall_labels.h"
#include "gui/copyable.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <ctime>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

// Light-mode colour dimmer — accent colours that work on a dark background
// are too washed-out on a white one.  Halve the RGB channels when in light mode.
inline ImVec4 Lc(const App& app, const ImVec4& c) {
    if (!app.lightMode) return c;
    return ImVec4(c.x * 0.44f, c.y * 0.56f, c.z * 0.44f, c.w);
}

// PPM crystal-offset control: typed value plus a slider for fine adjustment.
static bool drawPpmAdjust(const char* label, float* ppm)
{
    ImGui::PushID(label);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    bool changed = ImGui::InputFloat("##ppm", ppm, 0.1f, 1.0f, "%.2f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    changed |= ImGui::SliderFloat("##ppmsl", ppm, -50.0f, 50.0f, "");
    if (*ppm < -200.0f) *ppm = -200.0f;
    if (*ppm > 200.0f) *ppm = 200.0f;
    ImGui::PopID();
    return changed;
}

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

void drawSdrplayControls(App& app);

// Bring an aircraft up on the Flight Map (airplanes.live) and switch to that
// tab. A manual pick turns off "Follow aircraft on voice call", which would
// otherwise jump straight back to the voice-call aircraft.
static void showAircraftOnMap(App& app, uint32_t aes, const std::string& icaoText)
{
#if defined(_WIN32)
    std::string hex;
    if (aes) { char b[8]; std::snprintf(b, sizeof(b), "%06X", aes & 0xFFFFFF); hex = b; }
    else if (icaoText.size() == 6 && std::all_of(icaoText.begin(), icaoText.end(), [](char c) { return std::isxdigit((unsigned char)c); })) hex = icaoText;
    if (hex.empty()) return;
    FlightMapWebView& map = app.flightMapWv;
    if (map.mode == 2) { map.mode = 1; map.applyMode(); }
    map.followVoice = false;
    map.showAircraft(hex);
    ImGui::SetWindowFocus("###Flight Map");
#else
    (void)app; (void)aes; (void)icaoText;
#endif
}

// Waveform of a call: mirrored peak envelope, one bar per pixel, scaled to the
// call's loudest moment, with a playback cursor. Hover shows the time.
// Returns the clicked position in seconds, or -1 if not clicked.
static float drawCallWaveform(App& app, const std::vector<int16_t>& pcm, bool playing, float posSec)
{
    const float wh = ImGui::GetFrameHeight() * 3.2f;
    const float ww = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("##callwave", ImVec2(ww, wh));
    float clickSec = -1.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p1(p0.x + ww, p0.y + wh);
    const bool light = app.lightMode;
    dl->AddRectFilled(p0, p1, light ? IM_COL32(235, 238, 244, 255) : IM_COL32(16, 20, 28, 255), 3.0f);
    const float mid = p0.y + wh * 0.5f;
    dl->AddLine(ImVec2(p0.x, mid), ImVec2(p1.x, mid), light ? IM_COL32(180, 186, 200, 255) : IM_COL32(60, 70, 90, 255));
    const int n = (int)pcm.size();
    if (n == 0) return -1.0f;
    if (clicked)
        clickSec = std::clamp((ImGui::GetIO().MousePos.x - p0.x) / ww, 0.0f, 1.0f) * n / 8000.0f;
    int peak = 1;
    for (int16_t v : pcm) peak = std::max(peak, std::abs((int)v));
    const float scale = (wh * 0.48f) / (float)std::max(peak, 1500);
    const int cols = std::max(1, (int)ww);
    const ImU32 col = light ? IM_COL32(30, 110, 200, 255) : IM_COL32(90, 200, 255, 230);
    for (int c = 0; c < cols; c++) {
        const int i0 = (int)((long long)c * n / cols), i1 = (int)((long long)(c + 1) * n / cols);
        int pk = 0;
        for (int k = i0; k < i1 && k < n; k++) pk = std::max(pk, std::abs((int)pcm[k]));
        if (pk < 2) continue;
        const float h = std::max(1.0f, pk * scale);
        dl->AddLine(ImVec2(p0.x + c + 0.5f, mid - h), ImVec2(p0.x + c + 0.5f, mid + h), col);
    }
    if (playing) {
        const float x = p0.x + std::min(1.0f, posSec * 8000.0f / n) * ww;
        dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), IM_COL32(255, 120, 80, 255), 2.0f);
    }
    if (ImGui::IsItemHovered()) {
        const float fx = (ImGui::GetIO().MousePos.x - p0.x) / std::max(1.0f, ww);
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SetTooltip("%.1f s  (click to play from here)", std::clamp(fx, 0.0f, 1.0f) * n / 8000.0f);
    }
    return clickSec;
}

void reloadBandPlans(App& app) {
    scanBandPlans(app.bandPlanDir, app.bandPlanNames, app.bandPlanPaths, &app.bandPlanErrors);
    auto restore = [&](int& index, char* saved, BandPlan& loaded) {
        if (*saved) {
            auto found = std::find(app.bandPlanPaths.begin(), app.bandPlanPaths.end(), saved);
            index = found == app.bandPlanPaths.end() ? -1 : int(found - app.bandPlanPaths.begin());
        }
        if (index >= 0 && index < (int)app.bandPlanPaths.size()) {
            loaded = loadBandPlan(app.bandPlanPaths[index]);
            std::snprintf(saved, 512, "%s", app.bandPlanPaths[index].c_str());
        } else { index = -1; loaded = {}; }
    };
    restore(app.bandPlanIdx, app.bandPlanFile, app.bandPlanLoaded);
    restore(app.bandPlanIdxB, app.bandPlanFileB, app.bandPlanLoadedB);
}
static void bandPlanSelector(App& app, bool second) {
    ImGui::PushID(second ? "plansB" : "plansA");
    int& index = second ? app.bandPlanIdxB : app.bandPlanIdx;
    auto& loaded = second ? app.bandPlanLoadedB : app.bandPlanLoaded;
    char* saved = second ? app.bandPlanFileB : app.bandPlanFile;
    int& groupIndex = second ? app.bandPlanGroupB : app.bandPlanGroup;
    int& channelIndex = second ? app.bandPlanChannelB : app.bandPlanChannel;
    bool changed = false;
    static ImGuiTextFilter filters[2];
    filters[second ? 1 : 0].Draw("Region / country / plan", -1);
    const char* preview = index >= 0 && index < (int)app.bandPlanNames.size() ? app.bandPlanNames[index].c_str() : "Select a band plan";
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##plan", preview)) {
        for (int i = 0; i < (int)app.bandPlanNames.size(); ++i) {
            if (!filters[second ? 1 : 0].PassFilter(app.bandPlanNames[i].c_str())) continue;
            if (ImGui::Selectable(app.bandPlanNames[i].c_str(), i == index)) {
                index = i; loaded = loadBandPlan(app.bandPlanPaths[i]);
                std::snprintf(saved, 512, "%s", app.bandPlanPaths[i].c_str());
                groupIndex = 0;
                channelIndex = -1;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    if (index < 0 && *saved) ImGui::TextWrapped("Saved plan unavailable: %s", saved);
    const double rate = bandPlanCaptureRate(app, second);
    const auto groups = bandPlanGroups(loaded, rate);
    if (!groups.empty()) {
        groupIndex = std::clamp(groupIndex, 0, int(groups.size())-1);
        auto label = [](const BandPlanGroup& group) {
            char text[160];
            std::snprintf(text, sizeof(text), "%s: %.6f - %.6f MHz (%zu)", group.service.c_str(), group.loMHz, group.hiMHz, group.channels.size());
            return std::string(text);
        };
        ImGui::BeginDisabled(app.sourceMode == 1);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##frequency-group", label(groups[groupIndex]).c_str())) {
            for (int i = 0; i < int(groups.size()); ++i)
                if (ImGui::Selectable(label(groups[i]).c_str(), i == groupIndex)) { groupIndex = i; channelIndex = -1; changed = true; }
            ImGui::EndCombo();
        }
        bool& decode = second ? app.decodeBandPlanB : app.decodeBandPlan;
        if (ImGui::Checkbox("Create decoders from plan", &decode) && decode) changed = true;
        if (ImGui::SmallButton("Tune selected group")) { channelIndex = -1; changed = true; }
        if (changed) activateBandPlan(app, second);
        if (ImGui::TreeNode("Channel frequencies (MHz)")) {
            for (const auto channel : groups[groupIndex].channels) {
                const auto& entry = loaded.entries[channel];
                ImGui::PushID(int(channel));
                char frequency[32]; std::snprintf(frequency, sizeof(frequency), "%.6f", entry.frequencyMHz);
                if (ImGui::SmallButton(frequency)) {
                    channelIndex = int(channel);
                    activateBandPlan(app, second);
                }
                ImGui::SameLine(); ImGui::TextUnformatted(entry.label.c_str());
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        ImGui::EndDisabled();
        ImGui::TextWrapped(app.sourceMode == 1 ? "WAV frequency is fixed; tuning is unavailable." : "Selecting a plan or group tunes this receiver. Decoder modes come from the channel plan; groups fit the sample rate and IF bandwidth.");
    }
    if (loaded.valid && !loaded.notes.empty()) ImGui::TextWrapped("%s", loaded.notes.c_str());
    ImGui::PopID();
}

void drawControls(App& app)
{
    if (app.sourceMode == 7 && (app.rsp.streamFailed() || app.rspB.streamFailed())) {
        std::string err = app.rsp.streamFailed() ? app.rsp.error() : app.rspB.error();
        app.rspB.stop(); app.rsp.stop();
        app.decodersB.stop(); app.decoders.stop(); app.dualMode = false;
        app.iqRecorder.stop();
        app.status = "SDRplay: " + err;
    }
    ImGui::Begin((std::string(_L("Control")) + "###Control").c_str());

    bool running = app.active->running();

    ImGui::BeginDisabled(running);
    struct SourceChoice { const char* label; int mode; };
    const SourceChoice modes[] = {
        {"RTL-SDR", 0}, {"WAV file", 1}, {"SDR++ Server", 2},
        {"HackRF", 3}, {"Dual RTL", 4},
#ifdef HAS_AIRSPY
        {"Airspy", 5},
#endif
        {"RTL-TCP", 6}, {"SDRplay", 7},
#ifdef HAS_LIBIIO
        {"Pluto+ / AD936x", 8}
#endif
    };
    const char* sourcePreview = "Unavailable source";
    bool sourceAvailable = false;
    for (const auto& mode : modes)
    {
        if (mode.mode == app.sourceMode)
        {
            sourcePreview = mode.label;
            sourceAvailable = true;
        }
    }
    if (ImGui::BeginCombo(_L("Source"), sourcePreview))
    {
        for (const auto& mode : modes)
        {
            bool selected = mode.mode == app.sourceMode;
            if (ImGui::Selectable(mode.label, selected)) {
                app.rspB.close(); app.rsp.close();
                app.sourceMode = mode.mode;
                app.devices.clear(); app.deviceIndex = 0;
            }
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();

    ImGui::Separator();

    if (!running)
    {
        bool canStart = sourceAvailable &&
                        ((app.sourceMode == 1) ? (app.wavPath[0] != '\0') : true);
        ImGui::BeginDisabled(!canStart);
        if (ImGui::Button(_L("Start"), ImVec2(120, 0)))
            startActive(app);
        ImGui::EndDisabled();
    }
    else
    {
        if (ImGui::Button(_L("Stop"), ImVec2(120, 0)))
        {
            app.activeB->stop();
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
                if (url.empty()) url = "https://sarahsforge.dev/login";
                ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#endif
            }
        }
        else if (st == VersionCheck::UpToDate)
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "  (up to date)");
        }
        else if (st == VersionCheck::Unreleased)
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.7f, 0.5f, 1.0f, 1.0f), "  (unreleased)");
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
        if (ImGui::Button(_L("Refresh devices")))
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
        if (ImGui::Combo(_L("Sample rate (MHz)"), &app.sampleRateIdx, kRateLabels, kNumRates))
        {
            app.viewA.resetView = true;
            if (running)
                app.sdr.setSampleRate(kRates[app.sampleRateIdx]);
        }
        if (ImGui::Checkbox(_L("Auto gain (AGC)"), &app.autoGain))
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
        if (ImGui::Checkbox(_L("Bias-T"), &app.biasTee))
        {
            if (running)
                app.sdr.setBiasTee(app.biasTee);
        }
        if (drawPpmAdjust("PPM", &app.ppm))
        {
            if (running)
                app.sdr.setPpm((double)app.ppm);
        }
        if (ImGui::Checkbox(_L("DC block"), &app.dcBlock))
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
        if (ImGui::Button(_L("Browse...")))
            openWavDialog(app.wavPath, sizeof(app.wavPath));

        if (ImGui::Checkbox(_L("Loop"), &app.wavLoop))
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
    else if (app.sourceMode == 3)
    {
        // ---- HackRF (native) ----
        if (ImGui::Button(_L("Refresh devices")))
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
        if (drawPpmAdjust("PPM", &app.ppm))
        {
            if (running) app.hack.setPpm((double)app.ppm);
        }
        if (ImGui::Checkbox(_L("DC block"), &app.dcBlock))
        {
            if (running) app.hack.setDcBlock(app.dcBlock);
        }
    }
#ifdef HAS_AIRSPY
    else if (app.sourceMode == 5)
    {
        // ---- Airspy (native) ----
        if (ImGui::Button(_L("Refresh devices")))
            app.devices = app.airspy.listDevices();
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
                app.airspy.setCenterFreq(app.centerFreqMHz * 1e6);
        }
        if (ImGui::Combo(_L("Sample rate (MHz)"), &app.airspySampleRateIdx, kAirspyRateLabels, kAirspyNumRates))
        {
            app.viewA.resetView = true;
            if (running)
                app.airspy.setSampleRate(kAirspyRates[app.airspySampleRateIdx]);
        }

        ImGui::Separator();
        if (ImGui::RadioButton("Sensitive", app.airspyGainMode == 0)) app.airspyGainMode = 0;
        ImGui::SameLine();
        if (ImGui::RadioButton("Linear", app.airspyGainMode == 1)) app.airspyGainMode = 1;
        ImGui::SameLine();
        if (ImGui::RadioButton("Free", app.airspyGainMode == 2)) app.airspyGainMode = 2;

        if (app.airspyGainMode == 0)
        {
            if (ImGui::SliderInt("Sensitivity gain", &app.airspySenseGain, 0, 21))
            {
                if (running) app.airspy.setGainMode(0), app.airspy.setSenseGain(app.airspySenseGain);
            }
        }
        else if (app.airspyGainMode == 1)
        {
            if (ImGui::SliderInt("Linearity gain", &app.airspyLinearGain, 0, 21))
            {
                if (running) app.airspy.setGainMode(1), app.airspy.setLinearGain(app.airspyLinearGain);
            }
        }
        else
        {
            if (ImGui::Checkbox("LNA AGC", &app.airspyLnaAgc))
            {
                if (running) app.airspy.setLnaAgc(app.airspyLnaAgc);
            }
            ImGui::BeginDisabled(app.airspyLnaAgc);
            if (ImGui::SliderInt("LNA gain", &app.airspyLnaGain, 0, 15))
            {
                if (running) app.airspy.setLnaGain(app.airspyLnaGain);
            }
            ImGui::EndDisabled();

            if (ImGui::Checkbox("Mixer AGC", &app.airspyMixerAgc))
            {
                if (running) app.airspy.setMixerAgc(app.airspyMixerAgc);
            }
            ImGui::BeginDisabled(app.airspyMixerAgc);
            if (ImGui::SliderInt("Mixer gain", &app.airspyMixerGain, 0, 15))
            {
                if (running) app.airspy.setMixerGain(app.airspyMixerGain);
            }
            ImGui::EndDisabled();

            if (ImGui::SliderInt("VGA gain", &app.airspyVgaGain, 0, 15))
            {
                if (running) app.airspy.setVgaGain(app.airspyVgaGain);
            }
        }
        if (ImGui::Checkbox("Bias T (antenna power)", &app.airspyBias))
        {
            if (running) app.airspy.setBiasTee(app.airspyBias);
        }
        if (drawPpmAdjust("PPM", &app.ppm))
        {
            if (running) app.airspy.setPpm((double)app.ppm);
        }
        if (ImGui::Checkbox(_L("DC block"), &app.dcBlock))
        {
            if (running) app.airspy.setDcBlock(app.dcBlock);
        }
    }
#endif
#ifdef HAS_LIBIIO
    else if (app.sourceMode == 8)
    {
        // ---- ADALM-Pluto / Pluto+ (native libiio) ----
        ImGui::BeginDisabled(running);
        ImGui::InputText("IIO URI", app.plutoUri, sizeof(app.plutoUri));
        ImGui::EndDisabled();
        ImGui::TextDisabled("Examples: ip:192.168.2.1 or usb:1.2.3");

        if (ImGui::InputDouble("Center (MHz)", &app.centerFreqMHz, 0.1, 1.0, "%.4f"))
        {
            app.viewA.resetView = true;
            if (running)
                app.pluto.setCenterFreq(app.centerFreqMHz * 1e6);
        }
        if (ImGui::InputDouble("Sample rate (MHz)", &app.plutoSampleRateMHz,
                               0.1, 1.0, "%.3f"))
        {
            app.plutoSampleRateMHz = std::clamp(app.plutoSampleRateMHz, 0.521, 61.44);
            app.viewA.resetView = true;
            if (running)
                app.pluto.setSampleRate(app.plutoSampleRateMHz * 1e6);
        }
        if (ImGui::InputDouble("RF bandwidth (MHz)", &app.plutoBandwidthMHz,
                               0.1, 1.0, "%.3f"))
        {
            app.plutoBandwidthMHz = std::clamp(app.plutoBandwidthMHz, 0.2, 56.0);
            if (running)
                app.pluto.setBandwidth(app.plutoBandwidthMHz * 1e6);
        }

        const char* rfPorts[] = {"RX1 (A_BALANCED)", "RX2 (B_BALANCED)"};
        if (ImGui::Combo("RF input", &app.plutoRfPort, rfPorts, 2) && running)
            app.pluto.setRfPort(app.plutoRfPort == 1 ? "B_BALANCED" : "A_BALANCED");

        if (ImGui::Checkbox("Slow-attack AGC", &app.plutoAgc) && running)
            app.pluto.setGain(app.plutoAgc ? -1.0 : (double)app.plutoGainDb);
        if (!app.plutoAgc)
        {
            if (ImGui::SliderFloat("RX gain (dB)", &app.plutoGainDb, -3.0f, 73.0f, "%.1f") && running)
                app.pluto.setGain((double)app.plutoGainDb);
        }
        if (drawPpmAdjust("PPM", &app.ppm) && running)
            app.pluto.setPpm((double)app.ppm);
        if (ImGui::Checkbox(_L("DC block"), &app.dcBlock) && running)
            app.pluto.setDcBlock(app.dcBlock);
    }
#endif
    if (app.sourceMode == 6)
    {
        // ---- RTL-TCP (network) ----
        ImGui::SetNextItemWidth(-60.0f);
        ImGui::InputText("Host", app.rtlTcpHost, sizeof(app.rtlTcpHost));
        ImGui::InputInt("Port", &app.rtlTcpPort);
        if (app.rtlTcpPort < 1) app.rtlTcpPort = 1234;
        ImGui::Separator();
        if (ImGui::InputDouble("Center (MHz)", &app.centerFreqMHz, 0.1, 1.0, "%.4f"))
        {
            app.viewA.resetView = true;
            if (running) app.rtltcp.setCenterFreq(app.centerFreqMHz * 1e6);
        }
        if (ImGui::Combo(_L("Sample rate (MHz)"), &app.sampleRateIdx, kRateLabels, kNumRates))
        {
            app.viewA.resetView = true;
            if (running) app.rtltcp.setSampleRate(kRates[app.sampleRateIdx]);
        }
        if (ImGui::Checkbox(_L("Auto gain (AGC)"), &app.autoGain))
        {
            if (running) app.rtltcp.setGain(app.autoGain ? -1.0 : (double)app.gainDb);
        }
        if (!app.autoGain)
        {
            if (ImGui::SliderFloat("Gain (dB)", &app.gainDb, 0.0f, 50.0f, "%.1f"))
            {
                if (running) app.rtltcp.setGain((double)app.gainDb);
            }
        }
        if (ImGui::Checkbox(_L("Bias-T"), &app.biasTee))
        {
            if (running) app.rtltcp.setBiasTee(app.biasTee);
        }
        if (drawPpmAdjust("PPM", &app.ppm))
        {
            if (running) app.rtltcp.setPpm((double)app.ppm);
        }
        ImGui::TextDisabled("Remote rtl_tcp server. Connect and stream.");
    }
    if (app.sourceMode == 7) drawSdrplayControls(app);
    if (app.sourceMode == 4)
    {
        // ---- Dual RTL: two independent RTL-SDRs ----
        ImGui::TextColored(Lc(app, ImVec4(0.4f, 0.8f, 1.0f, 1.0f)), "RTL A (Spectrum / Waterfall A)");
        ImGui::Separator();
        if (ImGui::Button("Refresh A"))
            app.devices = app.sdr.listDevices();
        ImGui::SameLine();
        ImGui::Text("(%d found)", (int)app.devices.size());
        if (!app.devices.empty())
        {
            std::string preview = app.devices[std::min(app.deviceIndex, (int)app.devices.size() - 1)].name;
            if (ImGui::BeginCombo("Device A", preview.c_str()))
            {
                for (int i = 0; i < (int)app.devices.size(); ++i)
                {
                    bool sel = (app.deviceIndex == i);
                    std::string label = std::to_string(i) + ": " + app.devices[i].name + " [" + app.devices[i].serial + "]";
                    if (ImGui::Selectable(label.c_str(), sel)) app.deviceIndex = i;
                }
                ImGui::EndCombo();
            }
        }
        if (ImGui::InputDouble("Center A (MHz)", &app.centerFreqMHz, 0.1, 1.0, "%.4f"))
            app.viewA.resetView = true;
        ImGui::Combo("Rate A (MHz)", &app.sampleRateIdx, kRateLabels, kNumRates);
        if (ImGui::Checkbox("Auto gain A", &app.autoGain)) {}
        if (!app.autoGain)
            ImGui::SliderFloat("Gain A (dB)", &app.gainDb, 0.0f, 50.0f, "%.1f");
        ImGui::Checkbox("Bias-T A", &app.biasTee);
        if (drawPpmAdjust("PPM A", &app.ppm) && running)
            app.sdr.setPpm((double)app.ppm);
        ImGui::Checkbox("DC block A", &app.dcBlock);

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f), "RTL B (Spectrum / Waterfall B)");
        ImGui::Separator();
        if (ImGui::Button("Refresh B"))
            app.devices = app.sdrB.listDevices();
        if (!app.devices.empty())
        {
            std::string preview = app.devices[std::min(app.deviceIndexB, (int)app.devices.size() - 1)].name;
            if (ImGui::BeginCombo("Device B", preview.c_str()))
            {
                for (int i = 0; i < (int)app.devices.size(); ++i)
                {
                    bool sel = (app.deviceIndexB == i);
                    std::string label = std::to_string(i) + ": " + app.devices[i].name + " [" + app.devices[i].serial + "]";
                    if (ImGui::Selectable(label.c_str(), sel)) app.deviceIndexB = i;
                }
                ImGui::EndCombo();
            }
        }
        if (ImGui::InputDouble("Center B (MHz)", &app.centerFreqMHzB, 0.1, 1.0, "%.4f"))
            app.viewB.resetView = true;
        ImGui::Combo("Rate B (MHz)", &app.sampleRateIdxB, kRateLabels, kNumRates);
        if (ImGui::Checkbox("Auto gain B", &app.autoGainB)) {}
        if (!app.autoGainB)
            ImGui::SliderFloat("Gain B (dB)", &app.gainDbB, 0.0f, 50.0f, "%.1f");
        ImGui::Checkbox("Bias-T B", &app.biasTeeB);
        if (drawPpmAdjust("PPM B", &app.ppmB) && running)
            app.sdrB.setPpm((double)app.ppmB);
        ImGui::Checkbox("DC block B", &app.dcBlock); // same dcblock toggle
    }

    ImGui::Separator();
    ImGui::Combo("FFT size", &app.fftSizeIdx, kFftLabels, kNumFftSizes);
    ImGui::SliderFloat(_L("Averaging"), &app.avgAlpha, 0.0f, 0.98f, "%.2f");
    ImGui::Checkbox(_L("Auto-scale dB"), &app.autoScale);
    ImGui::SliderFloat(_L("dB min"), &app.dbMin, -140.0f, 0.0f, "%.0f");
    ImGui::SliderFloat(_L("dB max"), &app.dbMax, -140.0f, 20.0f, "%.0f");
    if (app.dbMax < app.dbMin + 5.0f)
        app.dbMax = app.dbMin + 5.0f;

    if (ImGui::Button(_L("Reset view (fit band)")))
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

    // Independent plans and region/country searches for receivers A and B.
    ImGui::Checkbox(_L("Band Plan"), &app.showBandPlan);
    ImGui::SameLine();
    if (ImGui::SmallButton("Reload plans")) reloadBandPlans(app);
    if (ImGui::InputText("Band plan folder", app.bandPlanDir, sizeof(app.bandPlanDir), ImGuiInputTextFlags_EnterReturnsTrue))
        reloadBandPlans(app);
    ImGui::TextDisabled("%d valid plans; search by region, country, satellite or name.", (int)app.bandPlanNames.size());
    if (app.showBandPlan) bandPlanSelector(app, false);
    if (app.dualMode) {
        ImGui::Checkbox(_L("Band Plan (B)"), &app.showBandPlanB);
        if (app.showBandPlanB) bandPlanSelector(app, true);
    }
    if (!app.bandPlanErrors.empty() && ImGui::TreeNode("Band plan errors")) {
        for (const auto& error : app.bandPlanErrors) ImGui::TextWrapped("%s", error.c_str());
        ImGui::TreePop();
    }

    ImGui::Separator();
    const char* bauds[] = {"600", "1200", "8400", "10500", "Inmarsat-C/EGC"};
    ImGui::PushStyleColor(ImGuiCol_FrameBg, app.lightMode
        ? ImVec4(0.70f, 0.78f, 0.90f, 1.0f) : ImVec4(0.10f, 0.18f, 0.42f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, app.lightMode
        ? ImVec4(0.78f, 0.85f, 0.95f, 1.0f) : ImVec4(0.15f, 0.28f, 0.60f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, app.lightMode
        ? ImVec4(0.65f, 0.73f, 0.88f, 1.0f) : ImVec4(0.12f, 0.22f, 0.50f, 1.0f));
    ImGui::Combo(_L("Decode baud"), &app.newBaud, bauds, 5);
    ImGui::PopStyleColor(3);
    ImGui::TextDisabled("Ctrl+click the spectrum to add a decoder there");

    ImGui::Separator();
    ImGui::Checkbox(_L("Follow C-channel voice"), &app.voiceFollow);
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
    if (ImGui::CollapsingHeader(_L("CallHunter (auto-scan for voice)")))
    {
        ImGui::Checkbox(_L("Enable CallHunter"), &app.callHunterMode);
        ImGui::SliderFloat(_L("Threshold (dB above baseline)"), &app.callHunterThreshDB, 1.0f, 20.0f, "%.1f");
        ImGui::SliderInt(_L("Confirm frames"), &app.callHunterConfirm, 5, 60);
        ImGui::SliderInt(_L("Lost frames"), &app.callHunterLost, 10, 120);

        int activeN = 0, candN = (int)app.callHunterCands.size();
        for (auto& c : app.callHunterCands)
            if (c.channelId >= 0) ++activeN;
        if (app.callHunterWarmup > 0)
            ImGui::TextDisabled("Settling baseline... (%d)", app.callHunterWarmup);
        else
            ImGui::TextDisabled("Candidates: %d tracked, %d decoders active", candN, activeN);
        if (app.following)
            ImGui::TextDisabled("(paused — voice‑follow is active)");
    }

    ImGui::Separator();
    if (ImGui::CollapsingHeader(_L("Database (SQLite log)")))
    {
        ImGui::Checkbox(_L("Log messages to database"), &app.logToDb);
        if (app.writeDb.enabled())
            ImGui::TextDisabled("  Session DB is active");
        if (ImGui::SliderInt(_L("Keep DB (days)"), &app.maxDbAgeDays, 1, 90))
        {
            if (app.maxDbAgeDays < 1) app.maxDbAgeDays = 1;
            if (app.maxDbAgeDays > 90) app.maxDbAgeDays = 90;
        }
        ImGui::TextDisabled("  Archives in: .\\databases\\messages_*.db");
    }

    ImGui::Separator();
    if (ImGui::CollapsingHeader(_L("Display")))
    {
        if (ImGui::Checkbox(_L("Light mode"), &app.lightMode))
        {
            if (app.lightMode)
            {
                ImGui::StyleColorsLight();
                ImGuiStyle& st = ImGui::GetStyle();
                st.Colors[ImGuiCol_Text]                  = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
                st.Colors[ImGuiCol_TextDisabled]           = ImVec4(0.36f, 0.36f, 0.36f, 1.00f);
                st.Colors[ImGuiCol_WindowBg]               = ImVec4(0.94f, 0.94f, 0.94f, 1.00f);
                st.Colors[ImGuiCol_TableHeaderBg]          = ImVec4(0.73f, 0.73f, 0.73f, 1.00f);
                st.Colors[ImGuiCol_TableRowBg]             = ImVec4(0.97f, 0.97f, 0.97f, 1.00f);
                st.Colors[ImGuiCol_TableRowBgAlt]          = ImVec4(0.88f, 0.88f, 0.88f, 1.00f);
                st.Colors[ImGuiCol_FrameBg]                = ImVec4(0.85f, 0.85f, 0.85f, 1.00f);
            }
            else
                ImGui::StyleColorsDark();
        }
        if (ImGui::SliderInt(_L("Font size"), &app.fontSize, 8, 24, "%d", ImGuiSliderFlags_AlwaysClamp))
        {
            if (app.fontSize < 8)  app.fontSize = 8;
            if (app.fontSize > 24) app.fontSize = 24;
        }
        ImGui::TextDisabled("  Restart to apply");
    }

    ImGui::Separator();
    if (ImGui::CollapsingHeader(_L("Output (message feed)")))
    {
        const char* fmts[] = {"JSON (JAERO/Acarshub)", "JAERO text", "JSON (InmarScope)"};
        ImGui::Combo("Format", &app.outFormat, fmts, 3);
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

    ImGui::Separator();
    if (ImGui::CollapsingHeader("IQ Recorder"))
    {
        ImGui::SetNextItemWidth(-70.0f);
        ImGui::InputText("IQ file", app.iqRecPath, sizeof(app.iqRecPath));
        // Pre-buffer slider (disabled above 3 Msps)
        double fs = app.active->running() ? app.active->sampleRate() : 0.0;
        if (!app.active->running())
            ImGui::BeginDisabled();
        bool overLimit = (fs > 3.0e6);
        if (overLimit)
            ImGui::BeginDisabled();
        if (ImGui::SliderFloat("Pre-buffer (s)", &app.iqBufferSec, 0.0f, 60.0f, "%.0f"))
        {
            app.iqRecorder.configurePrebuffer(fs, app.iqBufferSec);
        }
        if (overLimit)
        {
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("(disabled > 3 Msps)");
        }
        else if (app.iqBufferSec > 0.0f && fs > 0.0)
        {
            size_t bytes = (size_t)(fs * app.iqBufferSec * 2 * sizeof(float));
            char mem[32];
            if (bytes >= 1024 * 1024 * 1024)
                std::snprintf(mem, sizeof(mem), "(~%.1f GB)", (double)bytes / (1024.0 * 1024.0 * 1024.0));
            else
                std::snprintf(mem, sizeof(mem), "(~%.0f MB)", (double)bytes / (1024.0 * 1024.0));
            ImGui::SameLine();
            ImGui::TextDisabled("%s", mem);
        }
        if (!app.active->running())
            ImGui::EndDisabled();
        bool iqRec = app.iqRecorder.isRecording();
        if (iqRec)
        {
            if (ImGui::Button("Stop##iqrec"))
                app.iqRecorder.stop();
        }
        else
        {
            if (ImGui::Button("Start##iqrec"))
            {
                if (app.active && app.active->running())
                    app.iqRecorder.start(app.iqRecPath, app.active->sampleRate());
            }
        }
        if (app.iqRecorder.isRecording())
        {
            double sec = app.iqRecorder.elapsed();
            int m = (int)(sec / 60), s = (int)(sec) % 60;
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "REC %02d:%02d  —  %s",
                               m, s, app.iqRecorder.path().c_str());
        }
    }

    if (running)
    {
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

    // ---- Web Dashboard ----
    ImGui::Separator();
    if (ImGui::CollapsingHeader("Web Dashboard"))
    {
        if (ImGui::Checkbox("Enable server", &app.webServerEnabled))
        {
            if (app.webServerEnabled)
            {
                app.webServer.decodersA = &app.decoders;
                app.webServer.decodersB = &app.decodersB;
                app.webServer.dualMode = &app.dualMode;
                app.webServer.active = &app.active;
                app.webServer.start(app.webServerPort);
            }
            else
                app.webServer.stop();
        }
        if (!app.webServer.running())
        {
            ImGui::SetNextItemWidth(80);
            if (ImGui::InputInt("Port", &app.webServerPort))
            {
                if (app.webServerPort < 1) app.webServerPort = 1;
                if (app.webServerPort > 65535) app.webServerPort = 65535;
            }
        }
        else
        {
            char url[64];
            std::snprintf(url, sizeof(url), "http://localhost:%d", app.webServerPort);
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Running on port %d", app.webServerPort);
            ImGui::SameLine();
            if (ImGui::SmallButton("Open in browser"))
            {
#if defined(_WIN32)
                ShellExecuteA(nullptr, "open", url, nullptr, nullptr, SW_SHOWNORMAL);
#endif
            }
            ImGui::SameLine();
            ImGui::TextDisabled(url);
        }
    }

    ImGui::End();
}

void drawSpectrum(App& app, SpectrumView& v, DecoderManager& mgr, const char* title,
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
        {
            ImPlot::PlotLine("PSD", v.freqMHz.data(), v.avg.data(), v.curN);
        }

        auto decs = mgr.status();
        for (auto& d : decs)
        {
            double x = d.freqMHz;
            ImVec4 col = d.monitored ? ImVec4(0.3f, 0.5f, 1.0f, 1.0f)   // blue = active monitor
                       : d.locked    ? ImVec4(0.2f, 1.0f, 0.35f, 1.0f)  // green = locked
                                     : ImVec4(0.9f, 0.7f, 0.2f, 1.0f); // orange = unlocked
            if (ImPlot::DragLineX(d.channelId, &x, col, 2.0f))
                mgr.setDecoderFreq(d.channelId, x * 1e6);
        }

        ImPlotRect lim = ImPlot::GetPlotLimits();
        v.viewXminMHz = lim.X.Min;
        v.viewXmaxMHz = lim.X.Max;
        if (v.resetView) {
            v.lastBrowseCenterMHz = 0.5 * (lim.X.Min + lim.X.Max);
            v.lastBrowseRetune = std::chrono::steady_clock::now();
        }

        // Band-browse retuning: in dual mode use explicit SDR pointers,
        // otherwise use app.active (which covers RTL/WAV/SDR++/HackRF).
        SdrSource* browseSdr;
        if (app.dualMode)
            browseSdr = voiceView ? app.activeB
                                  : app.active;
        else
            browseSdr = app.active;
        if (allowBandBrowse && app.bandBrowse && app.sourceMode != 1 &&
            browseSdr->running() && !v.resetView && !app.following)
        {
            double viewCtr = 0.5 * (v.viewXminMHz + v.viewXmaxMHz);
            double viewHalf = 0.5 * (v.viewXmaxMHz - v.viewXminMHz);
            double sdrCtr = browseSdr->centerFreq() / 1e6;
            double fsMHz = browseSdr->sampleRate() / 1e6;
            double halfBand = 0.5 * fsMHz;
            double marginL = (viewCtr - viewHalf) - (sdrCtr - halfBand);
            double marginR = (sdrCtr + halfBand) - (viewCtr + viewHalf);
            double minMargin = std::min(marginL, marginR);
            double trigger = fsMHz * (app.browseEdgePct * 0.01);
            bool moved = std::fabs(viewCtr - v.lastBrowseCenterMHz) > fsMHz * (app.browseMinMovePct * 0.01);
            auto now = std::chrono::steady_clock::now();
            double sinceMs =
                std::chrono::duration<double, std::milli>(now - v.lastBrowseRetune).count();
            if (fsMHz > 0.0 && minMargin < trigger && moved && sinceMs > app.browseThrottleMs)
            {
                if (voiceView)
                {
                    // Retune SDR B preserving decoders on manager B
                    std::vector<std::pair<double, int>> keep;
                    for (auto& s : app.decodersB.status())
                        keep.push_back({s.freqMHz, s.baud});
                    app.centerFreqMHzB = viewCtr;
                    app.activeB->setCenterFreq(viewCtr * 1e6);
                    app.decodersB.removeAll();
                    app.decodersB.configure(app.activeB->sampleRate(), app.activeB->centerFreq());
                    for (auto& k : keep)
                        app.decodersB.addDecoder(k.first * 1e6, k.second);
                }
                else
                {
                    retunePreserving(app, viewCtr);
                }
                v.lastBrowseRetune = now;
                v.lastBrowseCenterMHz = viewCtr;
            }
        }

        ImVec2 pp = ImPlot::GetPlotPos();
        ImVec2 ps = ImPlot::GetPlotSize();
        v.specLeftInset = pp.x - origin.x;
        v.specRightInset = (origin.x + availW) - (pp.x + ps.x);

        if (bandValid)
            v.resetView = false;

        // Drag-to-place decoder: Ctrl+mousedown starts placing, move shows a white
        // preview line through the spectrum and waterfall, release creates the decoder.
        if (ImPlot::IsPlotHovered() && ImGui::GetIO().KeyCtrl)
        {
            ImPlotPoint mp = ImPlot::GetPlotMousePos();
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                app.placingDecoder = true;
                app.placingVoiceView = voiceView;
                app.placingFreqMHz = mp.x;
            }
        if (app.placingDecoder && app.placingVoiceView == voiceView)
            {
                app.placingFreqMHz = mp.x;
            }
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && app.placingDecoder)
            {
                app.placingDecoder = false;
                int baud;
                static const int kBaudVals[] = {600, 1200, 8400, 10500, kEgcBaud};
                int idx = app.newBaud < 0 ? 0 : (app.newBaud > 4 ? 4 : app.newBaud);
                baud = kBaudVals[idx];
                mgr.addDecoder(mp.x * 1e6, baud);
            }
        }
        else if (app.placingDecoder && app.placingVoiceView == voiceView)
        {
            // Ctrl released or cursor left the plot that started the placement.
            app.placingDecoder = false;
        }

        // Drag-to-place preview line redraw
        if (app.placingDecoder && app.placingVoiceView == voiceView)
        {
            ImPlotRect lim = ImPlot::GetPlotLimits();
            float xMin = (float)lim.X.Min;
            float xMax = (float)lim.X.Max;
            if (xMax > xMin)
            {
                float frac = ((float)app.placingFreqMHz - xMin) / (xMax - xMin);
                ImVec2 pp = ImPlot::GetPlotPos();
                ImVec2 ps = ImPlot::GetPlotSize();
                float px = pp.x + frac * ps.x;
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddLine(ImVec2(px, pp.y), ImVec2(px, pp.y + ps.y),
                            IM_COL32(255, 40, 40, 200), 1.5f);
            }
        }

        // --- Band plan: solid coloured bar along the bottom ---
        bool showBp = voiceView ? app.showBandPlanB : app.showBandPlan;
        const BandPlan& bp = voiceView ? app.bandPlanLoadedB : app.bandPlanLoaded;
        if (showBp && bp.valid && bandValid)
        {
            const ImPlotRect vp = ImPlot::GetPlotLimits();
            double viewLo = vp.X.Min, viewHi = vp.X.Max;
            if (viewHi <= viewLo) { viewLo = v.freqMHz.front(); viewHi = v.freqMHz.back(); }
            auto* dl = ImPlot::GetPlotDrawList();
            constexpr float kBandH = 28.0f;
            ImVec2 pp = ImPlot::GetPlotPos(), ps = ImPlot::GetPlotSize();
            float bandTop = pp.y + std::max(0.0f, ps.y - kBandH);
            float bandBot = pp.y + ps.y;
            ImPlot::PushPlotClipRect();
            float pxPerMHz = (float)(ps.x / (viewHi - viewLo));
            for (auto& e : bp.entries)
            {
                if (e.hiMHz < viewLo || e.loMHz > viewHi) continue;
                float loPx = pp.x + (float)((std::max(e.loMHz, viewLo) - viewLo) * pxPerMHz);
                float hiPx = pp.x + (float)((std::min(e.hiMHz, viewHi) - viewLo) * pxPerMHz);
                if (hiPx - loPx < 3.0f) {
                    const float middle = (loPx + hiPx) * 0.5f;
                    loPx = std::max(pp.x, middle - 1.5f);
                    hiPx = std::min(pp.x + ps.x, middle + 1.5f);
                }
                dl->AddRectFilled(ImVec2(loPx, bandTop), ImVec2(hiPx, bandBot), e.color);
                if (ImGui::IsMouseHoveringRect(ImVec2(loPx, bandTop), ImVec2(hiPx, bandBot)))
                    ImGui::SetTooltip("%s\n%.6f - %.6f MHz", e.label.c_str(), e.loMHz, e.hiMHz);
                float segW = hiPx - loPx;
                if (segW > 50 && !e.label.empty())
                {
                    float lw = ImGui::CalcTextSize(e.label.c_str()).x;
                    if (lw < segW - 4)
                    {
                        float cx = loPx + (segW - lw) * 0.5f;
                        float cy = bandTop + (kBandH - ImGui::GetTextLineHeight()) * 0.5f;
                        dl->AddText(ImVec2(cx, cy), IM_COL32(255, 255, 255, 230), e.label.c_str());
                    }
                }
            }
            ImPlot::PopPlotClipRect();
        }

        ImPlot::EndPlot();
        v.fftSkip = false;
    }
    ImGui::End();
}

void drawWaterfall(App& app, SpectrumView& v, const char* title)
{
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

    ImVec2 wfP0 = ImGui::GetCursorScreenPos();
    v.waterfall.draw(ImVec2(w, avail.y), uMin, uMax, xLo, xHi);
    v.fftSkip = false;
    if (v.curN > 0 && !v.freqMHz.empty()) {
        const bool second = &v == &app.viewB;
        const bool showPlan = second ? app.showBandPlanB : app.showBandPlan;
        const auto& plan = second ? app.bandPlanLoadedB : app.bandPlanLoaded;
        auto& manager = second ? app.decodersB : app.decoders;
        std::vector<WaterfallChannel> channels;
        for (const auto& decoder : manager.status())
            channels.push_back({decoder.freqMHz, decoder.channelId, decoder.baud, decoder.locked});
        const auto labels = layoutWaterfallLabels(showPlan ? &plan : nullptr, channels,
            v.viewXminMHz, v.viewXmaxMHz, v.freqMHz.front(), v.freqMHz.back(), ImVec2(w, avail.y));
        drawWaterfallLabels(labels, wfP0, ImVec2(w, avail.y));
    }

    // Drag-to-place preview line: white vertical line through the waterfall
    // at the frequency the user is hovering, so they can centre on a signal.
    if (app.placingDecoder && app.placingVoiceView == (&v == &app.viewB) && v.curN > 0)
    {
        double bandMin = v.freqMHz.front();
        double bandMax = v.freqMHz.back();
        double bandSpan = bandMax - bandMin;
        double viewSpan = v.viewXmaxMHz - v.viewXminMHz;
        double visLo = std::max(bandMin, v.viewXminMHz);
        double visHi = std::min(bandMax, v.viewXmaxMHz);
        if (bandSpan > 0 && viewSpan > 0 &&
            app.placingFreqMHz >= visLo && app.placingFreqMHz <= visHi)
        {
            float u = (float)((app.placingFreqMHz - visLo) / (visHi - visLo));
            float pixFrac = xLo + u * (xHi - xLo);
            float px = wfP0.x + pixFrac * w;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddLine(ImVec2(px, wfP0.y), ImVec2(px, wfP0.y + avail.y),
                        IM_COL32(255, 40, 40, 200), 1.5f);
        }
    }
    ImGui::End();
}

// Both panes operate the same recording state and receiver managers.
static void drawRecordVoiceToggle(App& app)
{
    if (ImGui::Checkbox(_L("Record voice calls"), &app.recordVoice))
    {
        app.decoders.setRecording(app.recordVoice, app.recordDir);
        app.decodersB.setRecording(app.recordVoice, app.recordDir);
        RecordFormat rf = (app.recordFormat == 1) ? RecordFormat::OGG : RecordFormat::WAV;
        app.decoders.setRecordFormat(rf);
        app.decodersB.setRecordFormat(rf);
    }
}

void drawDecoders(App& app)
{
    ImGui::Begin((std::string(_L("Decoders")) + "###Decoders").c_str());
    drawRecordVoiceToggle(app);

    auto decs = app.decoders.status();
    if (app.dualMode)
    {
        auto decsB = app.decodersB.status();
        for (auto& d : decsB) d.isB = true;
        decs.insert(decs.end(), decsB.begin(), decsB.end());
    }
    ImGui::Text("%d active  |  %d sub-band(s)  %d threads", (int)decs.size(),
                app.decoders.subbandCount() + (app.dualMode ? app.decodersB.subbandCount() : 0),
                app.decoders.workerCount() + (app.dualMode ? app.decodersB.workerCount() : 0));
    uint64_t drops = app.decoders.drops() + (app.dualMode ? app.decodersB.drops() : 0);
    ImGui::SameLine();
    if (drops > 0)
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "  drops: %llu",
                           (unsigned long long)drops);
    else
        ImGui::TextDisabled("  drops: 0");
    ImGui::SameLine();
    if (ImGui::SmallButton(_L("Remove all")))
    {
        app.decoders.removeAll();
        if (app.dualMode) app.decodersB.removeAll();
    }
    static std::string decsCopy;
    ImGui::SameLine();
    copyAllButton(decsCopy);

    int vm = app.decoders.voiceMonitor();
    int vmB = app.dualMode ? app.decodersB.voiceMonitor() : -1;
    if (vm >= 0 || vmB >= 0)
    {
        if (vm >= 0) ImGui::Text("Voice: monitoring ch %d", vm);
        if (vmB >= 0) ImGui::Text("Voice B: monitoring ch %d", vmB);
    }
        ImGui::TextDisabled("Voice: (no 8400 decoder)");
    ImGui::SameLine();
    float lvl = app.decoders.audioLevel() * 5.0f;
    if (lvl > 1.0f) lvl = 1.0f;
    ImGui::ProgressBar(lvl, ImVec2(110, 0), "");
    ImGui::SameLine();
    if (ImGui::Checkbox(_L("Mute"), &app.voiceMuted))
    {
        app.decoders.setVoiceMute(app.voiceMuted);
        app.decodersB.setVoiceMute(app.voiceMuted);
    }

    ImGui::SameLine();
    if (ImGui::Checkbox(_L("CPU reduce"), &app.cpuReduce))
    {
        app.decoders.setCpuReduce(app.cpuReduce);
        app.decodersB.setCpuReduce(app.cpuReduce);
    }

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

    const char* recFmts[] = {"WAV", "OGG"};
    ImGui::SetNextItemWidth(70);
    if (ImGui::Combo("##recfmt", &app.recordFormat, recFmts, 2))
    {
        RecordFormat fmt = (app.recordFormat == 1) ? RecordFormat::OGG : RecordFormat::WAV;
        app.decoders.setRecordFormat(fmt);
        app.decodersB.setRecordFormat(fmt);
    }
    ImGui::SameLine();
    if (app.recordVoice)
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "REC  (%d active, %s)",
                           app.decoders.recordingCount(),
                           app.recordFormat ? "OGG" : "WAV");
    else
        ImGui::TextDisabled("(saves every 8400 call to its own %s)",
                            app.recordFormat ? "OGG" : "WAV");
    ImGui::BeginDisabled(app.recordVoice);
    ImGui::SetNextItemWidth(-90.0f);
    ImGui::InputText("Folder", app.recordDir, sizeof(app.recordDir));
    ImGui::EndDisabled();

    if (ImGui::Checkbox("Save decoders on restart", &app.saveDecoders))
    {
        // Don't save 8400 decoders since voice frequencies change
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(excludes 8400 voice)");

    // Country blacklist — voice calls from these countries won't be monitored.
    {
        ImGui::Spacing();
        ImGui::Text("Voice country blacklist:");
        static char blBuf[4] = "";
        ImGui::SetNextItemWidth(50);
        ImGui::SameLine();
        ImGui::InputText("##blcode", blBuf, sizeof(blBuf),
                         ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (ImGui::SmallButton("Add##bladd"))
        {
            if (blBuf[0] && blBuf[1] && !blBuf[2])
            {
                std::string cc(blBuf, 2);
                auto& bl2 = app.blacklistCountries;
                if (std::find(bl2.begin(), bl2.end(), cc) == bl2.end())
                    bl2.push_back(cc);
                blBuf[0] = blBuf[1] = blBuf[2] = 0;
            }
        }
        bool first = true;
        for (size_t i = 0; i < app.blacklistCountries.size(); ++i)
        {
            if (!first) ImGui::SameLine();
            first = false;
            ImGui::Text("%s", app.blacklistCountries[i].c_str());
            ImGui::SameLine();
            char xbtn[12];
            std::snprintf(xbtn, sizeof(xbtn), "X##bl%zu", i);
            if (ImGui::SmallButton(xbtn))
            {
                app.blacklistCountries.erase(app.blacklistCountries.begin() + (ptrdiff_t)i);
                break;
            }
        }
        ImGui::Spacing();
    }

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
    bool toRemoveB = false;
        std::vector<std::string> copyRows;
        for (auto& d : decs)
        {
            int uid = d.channelId + (d.isB ? 100000 : 0);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            char selid[24];
            std::snprintf(selid, sizeof(selid), "##sel%d", uid);
            ImVec4 c = d.monitored ? ImVec4(0.3f, 0.5f, 1.0f, 1.0f)
                     : d.locked    ? Lc(app, ImVec4(0.2f, 1.0f, 0.3f, 1.0f))
                                   : ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Header, c);
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(c.x*1.3f, c.y*1.3f, c.z*1.3f, 1.0f));
            bool sel = (app.selectedDecoder == d.channelId);
            if (ImGui::Selectable(selid, sel, ImGuiSelectableFlags_None))
            {
                app.selectedDecoder = d.channelId;
                if (d.isVoice && !d.isB)
                    app.decoders.setVoiceMonitor(d.channelId);
            }
            ImGui::PopStyleColor(2);
            ImGui::SameLine();
            ImGui::TextColored(c, "%s", d.monitored ? "MON" : d.locked ? "LOCK" : "--");
            ImGui::TableNextColumn();
            ImGui::Text("%.4f", d.freqMHz);
            ImGui::TableNextColumn();
            if (d.baud == kEgcBaud)
            {
                if (d.egcCType == 1)
                    ImGui::TextUnformatted("EGC (NCS)");
                else if (d.egcCType == 2)
                    ImGui::TextUnformatted("EGC (LES)");
                else
                    ImGui::TextUnformatted("EGC");
            }
            else
                ImGui::Text("%d", d.baud);
            ImGui::TableNextColumn();
            if (d.baud == kEgcBaud)
            {
                if (d.egcFrames > 0)
                {
                    ImVec4 berCol = d.egcBer < 0  ? ImVec4(0.5f, 0.5f, 0.5f, 1.0f)
                                  : d.egcBer <= 10 ? Lc(app, ImVec4(0.2f, 1.0f, 0.3f, 1.0f))
                                  : d.egcBer <= 50 ? Lc(app, ImVec4(1.0f, 0.85f, 0.2f, 1.0f))
                                                   : ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
                    ImGui::TextColored(berCol, "BER %d (%dfr)", d.egcBer, d.egcFrames);
                }
                else
                    ImGui::TextDisabled("--");
            }
            else
            {
                double loRed, hiGreen;
                if (d.baud == 600 || d.baud == 1200) { loRed = 5.0; hiGreen = 8.0; }
                else                                 { loRed = 4.0; hiGreen = 6.0; }
                ImVec4 ebCol = d.ebno < loRed  ? ImVec4(1.0f, 0.3f, 0.3f, 1.0f)
                             : d.ebno < hiGreen ? Lc(app, ImVec4(1.0f, 0.85f, 0.2f, 1.0f))
                                                : Lc(app, ImVec4(0.2f, 1.0f, 0.3f, 1.0f));
                ImGui::TextColored(ebCol, "%.1f", d.ebno);
            }
            ImGui::TableNextColumn();
            ImGui::Text("%llu", (unsigned long long)d.msgs);
            ImGui::TableNextColumn();
            char btn[24];
            std::snprintf(btn, sizeof(btn), "X##%d", uid);
            if (ImGui::SmallButton(btn))
            {
                toRemove = d.channelId;
                toRemoveB = d.isB;
            }
            if (d.baud == kEgcBaud)
                copyRows.push_back(copyFmt("%.4f\tEGC\t%s\t%llu", d.freqMHz,
                    d.egcFrames > 0 ? copyFmt("BER %d", d.egcBer).c_str() : "--",
                    (unsigned long long)d.msgs));
            else
                copyRows.push_back(copyFmt("%.4f\t%d\t%.1f\t%llu", d.freqMHz, d.baud, d.ebno,
                    (unsigned long long)d.msgs));
        }
        handleTableCopy(copyRows);
        decsCopy = copyJoin(copyRows);
        ImGui::EndTable();
        if (toRemove >= 0)
        {
            if (toRemoveB) app.decodersB.removeDecoder(toRemove);
            else           app.decoders.removeDecoder(toRemove);
        }
    }

    ImGui::End();
}

void drawSUs(App& app)
{
    ImGui::Begin((std::string(_L("SUs")) + "###SUs").c_str());

    unsigned long long suTotal = app.decoders.suLog().count();
    if (app.dualMode) suTotal += app.decodersB.suLog().count();
    ImGui::Text("%llu total", suTotal);
    ImGui::SameLine();
    if (ImGui::SmallButton(_L("Clear")))
    {
        app.decoders.suLog().clear();
        if (app.dualMode) app.decodersB.suLog().clear();
    }
    static std::string suCopy;
    ImGui::SameLine();
    copyAllButton(suCopy);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##searchsu", "Search...", app.searchBuf, sizeof(app.searchBuf));

    // Session archive dropdown for SUs.
    if (!app.archiveDbLabels.empty())
    {
        std::vector<const char*> items;
        items.push_back("Live");
        for (auto& lbl : app.archiveDbLabels)
            items.push_back(lbl.c_str());
        ImGui::SetNextItemWidth(200);
        if (ImGui::Combo("Session", &app.archiveComboSu, items.data(), (int)items.size()))
        {
            if (app.archiveComboSu == 0)
            {
                app.decoders.suLog().clearArchive();
            }
            else
            {
                int idx = app.archiveComboSu - 1;
                if (idx < (int)app.archiveDbPaths.size())
                    app.writeDb.loadAcarsOrSu(app.archiveDbPaths[idx],
                                              MessageStore::SU,
                                              &app.decoders.suLog(), 0);
            }
        }
        if (app.decoders.suLog().hasArchive())
            ImGui::TextDisabled("  Viewing archived session");
    }
    ImGui::Separator();

    auto msgs = app.decoders.suLog().snapshot();
    if (app.dualMode)
    {
        auto b = app.decodersB.suLog().snapshot();
        msgs.insert(msgs.end(), b.begin(), b.end());
    }
    std::sort(msgs.begin(), msgs.end(),
              [](const DecodedMessage& a, const DecodedMessage& b) { return a.timeSec > b.timeSec; });
    std::string searchLower;
    bool hasSearch = (app.searchBuf[0] != 0);
    if (hasSearch)
    {
        searchLower = app.searchBuf;
        for (auto& ch : searchLower) ch = (char)std::tolower((unsigned char)ch);
    }
    if (ImGui::BeginTable("##sus", 3,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable))
    {
        ImGui::TableSetupColumn("Freq", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Text", ImGuiTableColumnFlags_WidthFixed, 220);
        ImGui::TableSetupColumn("Bytes");
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        std::vector<std::string> copyRows;
        for (auto it = msgs.begin(); it != msgs.end(); ++it)
        {
            if (hasSearch)
            {
                std::string hay = it->text + "|" + it->hex;
                for (auto& ch : hay) ch = (char)std::tolower((unsigned char)ch);
                if (hay.find(searchLower) == std::string::npos)
                    continue;
            }
            ImGui::TableNextRow();

            // Colorize by SU type
            ImVec4 col = ImVec4(0.7f, 0.7f, 0.7f, 1.0f); // default gray
            if (it->suType == 0x30 && it->aesId != 0)      // Call progress — per-aircraft color
            {
                float hue = (it->aesId * 0.618033988749895f); // golden ratio conjugate
                hue = hue - (int)hue;
                ImGui::ColorConvertHSVtoRGB(hue, 0.8f, 0.9f, col.x, col.y, col.z);
            }
            else if (it->suType == 0x21)                    // Call announcement
                col = Lc(app, ImVec4(1.0f, 0.85f, 0.2f, 1.0f));     // gold
            else if (it->suType >= 0x31 && it->suType <= 0x34) // C-channel assignment
                col = Lc(app, ImVec4(0.3f, 0.7f, 1.0f, 1.0f));     // blue

            ImGui::TableNextColumn();
            ImGui::Text("%.3f", it->freqMHz);
            ImGui::TableNextColumn();
            ImGui::TextColored(col, "%s", it->text.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(it->hex.c_str());
            copyRows.push_back(copyFmt("%.3f\t%s\t%s", it->freqMHz, it->text.c_str(), it->hex.c_str()));
        }
        handleTableCopy(copyRows);
        suCopy = copyJoin(copyRows);
        ImGui::EndTable();
    }

    ImGui::End();
}

void drawMessages(App& app)
{
    ImGui::Begin((std::string(_L("Messages")) + "###Messages").c_str());

    unsigned long long msgTotal = app.decoders.log().count();
    if (app.dualMode) msgTotal += app.decodersB.log().count();
    ImGui::Text("%llu total", msgTotal);
    ImGui::SameLine();
    if (ImGui::SmallButton(_L("Clear")))
    {
        app.decoders.log().clear();
        if (app.dualMode) app.decodersB.log().clear();
    }
    static std::string msgCopy;
    ImGui::SameLine();
    copyAllButton(msgCopy);
    ImGui::SameLine();
    ImGui::Checkbox(_L("Show empty"), &app.showEmptyMsgs);
    // Pause: freeze the list so it can be read (messages keep being logged).
    static bool msgPaused = false;
    static std::vector<DecodedMessage> frozenMsgs;
    static unsigned long long frozenTotal = 0;
    ImGui::SameLine();
    if (ImGui::Button(msgPaused ? "Resume" : "Pause")) {
        msgPaused = !msgPaused;
        if (msgPaused) {
            frozenMsgs = app.decoders.log().snapshot();
            if (app.dualMode) { auto b = app.decodersB.log().snapshot(); frozenMsgs.insert(frozenMsgs.end(), b.begin(), b.end()); }
            frozenTotal = msgTotal;
        }
    }
    if (msgPaused) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "Paused (%llu new)", msgTotal - frozenTotal);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##searchmsg", "Search...", app.searchBuf, sizeof(app.searchBuf));

    // Session archive dropdown — reloads ACARS from a past DB file.
    {
        double now = (double)std::time(nullptr);
        if (app.archiveDbLastScan == 0.0 || now - app.archiveDbLastScan > 3.0)
        {
            app.archiveDbPaths.clear();
            app.archiveDbLabels.clear();
            auto files = app.writeDb.scanArchives("databases");
            for (auto& f : files)
            {
                app.archiveDbPaths.push_back(f.filename);
                app.archiveDbLabels.push_back(f.displayLabel + "  (" +
                                              std::to_string(f.rowCount) + " msgs)");
            }
            app.archiveDbLastScan = now;
        }
        if (!app.archiveDbLabels.empty())
        {
            std::vector<const char*> items;
            items.push_back("Live");
            for (auto& lbl : app.archiveDbLabels)
                items.push_back(lbl.c_str());
            ImGui::SetNextItemWidth(200);
            if (ImGui::Combo("Session", &app.archiveComboMsg, items.data(), (int)items.size()))
            {
                if (app.archiveComboMsg == 0)
                {
                    app.decoders.log().clearArchive();
                }
                else
                {
                    int idx = app.archiveComboMsg - 1;
                    if (idx < (int)app.archiveDbPaths.size())
                        app.writeDb.loadAcarsOrSu(app.archiveDbPaths[idx],
                                                  MessageStore::ACARS,
                                                  &app.decoders.log(), 0);
                }
            }
            if (app.decoders.log().hasArchive())
                ImGui::TextDisabled("  Viewing archived session");
        }
    }
    ImGui::Separator();

    std::vector<DecodedMessage> msgs;
    if (msgPaused) msgs = frozenMsgs;
    else {
        msgs = app.decoders.log().snapshot();
        if (app.dualMode)
        {
            auto b = app.decodersB.log().snapshot();
            msgs.insert(msgs.end(), b.begin(), b.end());
        }
    }
    // Selected message (a copy, so it stays put while the list moves).
    static bool msgHasSel = false;
    static DecodedMessage msgSel;
    // Filter on search text (case-insensitive substring across all fields).
    std::string searchLower;
    bool hasSearch = (app.searchBuf[0] != 0);
    if (hasSearch)
    {
        searchLower = app.searchBuf;
        for (auto& ch : searchLower) ch = (char)std::tolower((unsigned char)ch);
    }
    const float detailH = msgHasSel ? std::max(150.0f, ImGui::GetContentRegionAvail().y * 0.38f) : 0.0f;
    if (ImGui::BeginTable("##msgs", 7,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                          ImVec2(0, msgHasSel ? -detailH : 0.0f)))
    {
        ImGui::TableSetupColumn("UTC", ImGuiTableColumnFlags_WidthFixed, 54);
        ImGui::TableSetupColumn("Freq", ImGuiTableColumnFlags_WidthFixed, 64);
        ImGui::TableSetupColumn("Dir", ImGuiTableColumnFlags_WidthFixed, 32);
        ImGui::TableSetupColumn("Reg", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("AES", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("Lbl", ImGuiTableColumnFlags_WidthFixed, 36);
        ImGui::TableSetupColumn("Text");
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        int rowIdx = 0;
        std::vector<std::string> copyRows;
        for (auto it = msgs.rbegin(); it != msgs.rend(); ++it)
        {
            // Hide empty ACARS messages (no text and no decoded body) unless shown.
            if (!app.showEmptyMsgs && it->text.empty() && it->decoded.empty())
                continue;
            // Search filter — match any field (case-insensitive).
            if (hasSearch)
            {
                std::string hay = it->text + "|" + it->hex + "|" + it->reg + "|"
                                + it->label + "|" + it->icao + "|" + it->decoded;
                for (auto& ch : hay) ch = (char)std::tolower((unsigned char)ch);
                if (hay.find(searchLower) == std::string::npos)
                    continue;
            }
            ImGui::TableNextRow();
            ImGui::PushID(rowIdx++);
            ImGui::TableNextColumn();
            // UTC time from epoch seconds
            {
                time_t t = (time_t)it->timeSec;
                std::tm tm{};
#if defined(_WIN32)
                gmtime_s(&tm, &t);
#else
                gmtime_r(&t, &tm);
#endif
                char tl[16];
                std::snprintf(tl, sizeof(tl), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
                const bool isSel = msgHasSel && msgSel.timeSec == it->timeSec && msgSel.aesId == it->aesId &&
                                   msgSel.text == it->text;
                // Click a row (its time) to see it explained below.
                if (ImGui::Selectable(tl, isSel, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
                    if (isSel) msgHasSel = false;
                    else { msgSel = *it; msgHasSel = true; }
                }
            }
            ImGui::TableNextColumn();
            ImGui::Text("%.3f", it->freqMHz);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(it->downlink ? "DL" : "UL");
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(it->reg.c_str());
            ImGui::TableNextColumn();
            if (it->aesId) {
                char ab[8];
                std::snprintf(ab, sizeof(ab), "%06X", it->aesId & 0xFFFFFF);
                ImGui::PushStyleColor(ImGuiCol_Text, Lc(app, ImVec4(1.0f, 0.85f, 0.3f, 1.0f)));
                if (ImGui::SmallButton(ab)) showAircraftOnMap(app, it->aesId, it->icao);
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show this aircraft on the Flight Map");
            } else
                ImGui::TextDisabled("--");
            ImGui::TableNextColumn();
            {
                std::string lab = it->label;
                for (auto& ch : lab) if ((unsigned char)ch < 0x20 || (unsigned char)ch >= 0x7f) ch = '.';
                ImGui::TextUnformatted(lab.c_str());
                if (ImGui::IsItemHovered()) {
                    const std::string mean = acarsLabelMeaning(it->label);
                    if (!mean.empty()) ImGui::SetTooltip("%s", mean.c_str());
                }
            }
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", it->text.c_str());
            if (it->hasPos)
                ImGui::TextColored(ImVec4(0.3f, 0.9f, 1.0f, 1.0f),
                                   "POS %.4f, %.4f  %d ft", it->lat, it->lon, it->alt);
            if (!it->decoded.empty())
            {
                ImGui::PushStyleColor(ImGuiCol_Text, Lc(app, ImVec4(0.6f, 1.0f, 0.6f, 1.0f)));
                ImGui::TextWrapped("%s", it->decoded.c_str());
                ImGui::PopStyleColor();
            }
            char utc[16];
            {
                time_t t = (time_t)it->timeSec;
                std::tm tm{};
#if defined(_WIN32)
                gmtime_s(&tm, &t);
#else
                gmtime_r(&t, &tm);
#endif
                std::snprintf(utc, sizeof(utc), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
            }
            std::string row = copyFmt("%s\t%.3f\t%s\t%s\t%06X\t%s\t%s",
                                      utc, it->freqMHz, it->downlink ? "DL" : "UL",
                                      it->reg.c_str(), it->aesId, it->label.c_str(),
                                      it->text.c_str());
            if (!it->decoded.empty())
            {
                row += '\n';
                row += it->decoded;
            }
            copyRows.push_back(std::move(row));
            ImGui::PopID();
        }
        handleTableCopy(copyRows);
        msgCopy = copyJoin(copyRows);
        ImGui::EndTable();
    }
    // Plain-English breakdown of the selected message.
    if (msgHasSel) {
        ImGui::BeginChild("##msgdetail", ImVec2(0, 0), ImGuiChildFlags_Borders);
        ImGui::TextColored(Lc(app, ImVec4(0.5f, 0.85f, 1.0f, 1.0f)), "In plain English");
        ImGui::SameLine();
        if (msgSel.aesId) {
            if (ImGui::SmallButton("Show on map")) showAircraftOnMap(app, msgSel.aesId, msgSel.icao);
            ImGui::SameLine();
        }
        static std::string explainCopy;
        explainCopy = explainAcars(msgSel) + "\nRaw text: " + msgSel.text;
        if (ImGui::SmallButton("Copy")) ImGui::SetClipboardText(explainCopy.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Close")) msgHasSel = false;
        ImGui::Separator();
        ImGui::TextWrapped("%s", explainAcars(msgSel).c_str());
        ImGui::TextDisabled("Raw text:");
        ImGui::TextWrapped("%s", msgSel.text.c_str());
        ImGui::EndChild();
    }

    ImGui::End();
}

void drawAircraft(App& app)
{
    ImGui::Begin((std::string(_L("Aircraft")) + "###Aircraft").c_str());

    auto acs = app.decoders.aircraftTable().snapshot();
    ImGui::Text("%zu tracked", acs.size());
    ImGui::SameLine();
    if (ImGui::SmallButton(_L("Clear")))
        app.decoders.aircraftTable().clear();
    static std::string acCopy;
    ImGui::SameLine();
    copyAllButton(acCopy);
    ImGui::SameLine();
    ImGui::Checkbox(_L("With position only"), &app.acPosOnly);
    ImGui::Separator();

    double now = (double)std::time(nullptr);
    // Newest activity first.
    std::sort(acs.begin(), acs.end(),
              [](const AircraftEntry& a, const AircraftEntry& b) { return a.lastSeen > b.lastSeen; });

    if (ImGui::BeginTable("##aircraft", 10,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable))
    {
        ImGui::TableSetupColumn("AES", ImGuiTableColumnFlags_WidthFixed, 58);
        ImGui::TableSetupColumn("ICAO", ImGuiTableColumnFlags_WidthFixed, 54);
        ImGui::TableSetupColumn("Ctry", ImGuiTableColumnFlags_WidthFixed, 34);
        ImGui::TableSetupColumn("Reg", ImGuiTableColumnFlags_WidthFixed, 64);
        ImGui::TableSetupColumn("Flight", ImGuiTableColumnFlags_WidthFixed, 64);
        ImGui::TableSetupColumn("Lat", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Lon", ImGuiTableColumnFlags_WidthFixed, 74);
        ImGui::TableSetupColumn("Alt", ImGuiTableColumnFlags_WidthFixed, 56);
        ImGui::TableSetupColumn("Age", ImGuiTableColumnFlags_WidthFixed, 48);
        ImGui::TableSetupColumn("Msgs", ImGuiTableColumnFlags_WidthFixed, 48);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        std::vector<std::string> copyRows;
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
            const char* cc = nullptr;
            if (!a.icao.empty())
            {
                uint32_t ihex = (uint32_t)std::strtoul(a.icao.c_str(), nullptr, 16);
                cc = icaoCountry(ihex);
                if (cc) ImGui::TextUnformatted(cc);
            }
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
            copyRows.push_back(copyFmt("%06X\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%ds\t%llu",
                a.aesId, a.icao.c_str(), cc ? cc : "",
                a.reg.c_str(), a.flight.c_str(),
                a.hasPos ? copyFmt("%.4f", a.lat).c_str() : "",
                a.hasPos ? copyFmt("%.4f", a.lon).c_str() : "",
                a.hasPos ? copyFmt("%d", a.alt).c_str() : "",
                (int)(now - a.lastSeen), (unsigned long long)a.msgs));
        }
        handleTableCopy(copyRows);
        acCopy = copyJoin(copyRows);
        ImGui::EndTable();
    }

    ImGui::End();
}

const char* cassignTypeName(uint8_t t)
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

void drawCChannel(App& app)
{
    ImGui::Begin((std::string(_L("C-Channel")) + "###C-Channel").c_str());

    unsigned long long cTotal = app.decoders.cassignLog().count();
    if (app.dualMode) cTotal += app.decodersB.cassignLog().count();
    ImGui::Text("%llu assignment(s)", cTotal);
    ImGui::SameLine();
    if (ImGui::SmallButton(_L("Clear")))
    {
        app.decoders.cassignLog().clear();
        if (app.dualMode) app.decodersB.cassignLog().clear();
    }
    static std::string cchanCopy;
    ImGui::SameLine();
    copyAllButton(cchanCopy);
    ImGui::Separator();

    auto items = app.decoders.cassignLog().snapshot();
    if (app.dualMode)
    {
        auto b = app.decodersB.cassignLog().snapshot();
        items.insert(items.end(), b.begin(), b.end());
    }
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

        std::vector<std::string> copyRows;
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
                    tuneToVoice(app, it.rxMHz, it.aesId);
                ImGui::EndDisabled();
            }
            copyRows.push_back(copyFmt("%s\t%06X\t%02X\t%.4f\t%.4f",
                cassignTypeName(it.type), it.aesId, it.gesId, it.rxMHz, it.txMHz));
            ImGui::PopID();
        }

        // Keep pinned to the newest row only while the user is already at the
        // bottom; if they scroll up, leave their position alone.
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
            ImGui::SetScrollHereY(1.0f);

        handleTableCopy(copyRows);
        cchanCopy = copyJoin(copyRows);
        ImGui::EndTable();
    }
    ImGui::End();
}

void drawNetwork(App& app)
{
    ImGui::Begin((std::string(_L("Network")) + "###Network").c_str());

    SatInfo sat = app.decoders.channelTable().satellite();
    if (sat.valid)
        ImGui::Text("Satellite ID: %d   Longitude: %s", sat.satId, sat.longitude.c_str());
    else
        ImGui::TextDisabled("Satellite: (waiting for system table)");
    ImGui::SameLine();
    if (ImGui::SmallButton(_L("Clear")))
        app.decoders.channelTable().clear();
    static std::string netCopy;
    ImGui::SameLine();
    copyAllButton(netCopy);
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

        std::vector<std::string> copyRows;
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
            copyRows.push_back(copyFmt("%.4f\t%s\t%02X\t%llu",
                c.freqMHz, c.kind.c_str(), c.ges, (unsigned long long)c.hits));
        }
        handleTableCopy(copyRows);
        netCopy = copyJoin(copyRows);
        ImGui::EndTable();
    }

    ImGui::End();
}

#if defined(_WIN32)
void drawFlightMap(App& app)
{
    ImGui::Begin((std::string(_L("Flight Map")) + "###Flight Map").c_str());

    auto acs = app.decoders.aircraftTable().snapshot();
    if (app.dualMode) {
        auto second = app.decodersB.aircraftTable().snapshot();
        acs.insert(acs.end(), second.begin(), second.end());
    }
    FlightMapWebView& map = app.flightMapWv;
    {
        const char* modes[] = { "All live traffic (airplanes.live)",
                                "Received aircraft only (airplanes.live)",
                                "Received aircraft only (decoded positions)" };
        ImGui::SetNextItemWidth(300);
        if (ImGui::Combo("Map", &map.mode, modes, 3)) map.applyMode();
    }

    if (map.mode == 0 || map.mode == 1)
    {
        // Aircraft whose voice call is being monitored (SDR B does voice in
        // dual mode). On Inmarsat Aero the AES ID is the ICAO 24-bit address.
        uint32_t voiceAes = app.dualMode ? app.decodersB.voiceAes() : 0;
        if (!voiceAes) voiceAes = app.decoders.voiceAes();
        auto icaoOf = [](uint32_t aes) { char b[8]; std::snprintf(b, sizeof(b), "%06X", aes & 0xFFFFFF); return std::string(b); };
        auto describe = [&](const std::string& icao) {
            std::string out = icao;
            for (const auto& a : acs) {
                if (icaoOf(a.aesId) != icao && a.icao != icao) continue;
                if (!a.flight.empty()) out += "  " + a.flight;
                if (!a.reg.empty())    out += "  " + a.reg;
                break;
            }
            return out;
        };

        if (map.mode == 1) {
            // Aircraft received in the last hour, newest first, capped so the
            // filter stays a reasonable size.
            std::vector<const AircraftEntry*> live;
            const double nowSec = (double)std::time(nullptr);
            for (const auto& a : acs) if (a.aesId && nowSec - a.lastSeen < 3600.0) live.push_back(&a);
            std::sort(live.begin(), live.end(),
                      [](const AircraftEntry* x, const AircraftEntry* y) { return x->lastSeen > y->lastSeen; });
            std::vector<std::string> ids;
            if (voiceAes) ids.push_back(icaoOf(voiceAes));
            for (const auto* a : live) { if (ids.size() >= 200) break; ids.push_back(icaoOf(a->aesId)); }
            map.setReceived(ids);
        }

        ImGui::SameLine();
        ImGui::Checkbox("Follow aircraft on voice call", &map.followVoice);
        if (map.followVoice && voiceAes) map.showAircraft(icaoOf(voiceAes));

        // Manual pick from received aircraft, most recent first.
        std::vector<const AircraftEntry*> recent;
        for (const auto& a : acs) if (a.aesId) recent.push_back(&a);
        std::sort(recent.begin(), recent.end(),
                  [](const AircraftEntry* x, const AircraftEntry* y) { return x->lastSeen > y->lastSeen; });
        static uint32_t chosenAes = 0;
        std::string chosenLabel = chosenAes ? describe(icaoOf(chosenAes)) : std::string("Select received aircraft...");
        ImGui::SetNextItemWidth(260);
        if (ImGui::BeginCombo("##mapaircraft", chosenLabel.c_str()))
        {
            for (const auto* a : recent) {
                const std::string label = describe(icaoOf(a->aesId));
                ImGui::PushID((int)a->aesId);
                if (ImGui::Selectable(label.c_str(), a->aesId == chosenAes)) chosenAes = a->aesId;
                ImGui::PopID();
            }
            if (recent.empty()) ImGui::TextDisabled("No aircraft received yet");
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(chosenAes == 0);
        if (ImGui::Button("Show on map")) map.showAircraft(icaoOf(chosenAes));
        ImGui::EndDisabled();

        const std::string shown = map.shownIcao();
        ImGui::SameLine();
        if (voiceAes && map.followVoice)
            ImGui::TextColored(ImVec4(0.3f, 0.85f, 0.4f, 1.0f), "On voice call: %s", describe(icaoOf(voiceAes)).c_str());
        else if (!shown.empty()) {
            std::string up = shown;
            for (auto& c : up) c = (char)std::toupper((unsigned char)c);
            ImGui::TextDisabled("Showing: %s", describe(up).c_str());
        }
        else ImGui::TextDisabled("Waiting for a voice call...");
        if (!map.isReady()) ImGui::TextDisabled("Loading airplanes.live...");
    }
    else
    {
        map.updateAircraft(acs);
        ImGui::Checkbox("Online positions for received aircraft", &map.onlinePositions);
        ImGui::TextWrapped("%s", map.positionStatus().c_str());
        if (!map.isReady()) ImGui::TextDisabled("Loading received-aircraft map...");
    }
    // Embed the map as an Edge WebView2 child window inside this panel.
    // Hide when another tab in the same dock is active.
    ImVec2 pos  = ImGui::GetCursorScreenPos();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    int w = std::max(1, (int)avail.x);
    int h = std::max(1, (int)avail.y);
    bool tabActive = true;
    if (ImGuiDockNode* node = ImGui::GetCurrentWindow()->DockNode)
        tabActive = (node->VisibleWindow == ImGui::GetCurrentWindow());
    ImGui::InvisibleButton("##map", ImVec2((float)w, (float)h));
    bool inMainViewport = (ImGui::GetWindowViewport() == ImGui::GetMainViewport());
    if (!inMainViewport)
    {
        app.flightMapWv.setBounds(0, 0, 0, 0, false);
        ImGui::SetCursorScreenPos(pos);
        ImGui::TextWrapped("%s", _L("Dock the Flight Map back in the main window to show the embedded map."));
    }
    else {
        // With multi-viewports ImGui positions are desktop coordinates; the
        // WebView is a child of the main window, so make them relative to its
        // client area (otherwise the map sits a title bar's height too low).
        const ImVec2 vp = ImGui::GetMainViewport()->Pos;
        // The map is a native window drawn on top of everything ImGui draws,
        // so it would cover any dropdown list or menu that opens over it
        // (e.g. this panel's Map / aircraft lists). Step it aside (the same
        // show/hide used when switching tabs - nothing is suspended) while one
        // is open.
        const bool popupOpen = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
        app.flightMapWv.setBounds((int)(pos.x - vp.x), (int)(pos.y - vp.y), w, h, tabActive && !popupOpen);
    }

    ImGui::End();
}
#else
// The Flight Map is an embedded Edge WebView2 browser, which only exists on
// Windows. On other platforms the panel is not registered or drawn at all.
void drawFlightMap(App&) {}
#endif // _WIN32

void drawEgc(App& app)
{
    ImGui::Begin((std::string(_L("EGC")) + "###EGC").c_str());

    unsigned long long egcTotal = app.decoders.egcLog().count();
    if (app.dualMode) egcTotal += app.decodersB.egcLog().count();
    ImGui::Text("%llu message(s)", egcTotal);
    ImGui::SameLine();
    if (ImGui::SmallButton(_L("Clear")))
    {
        app.decoders.egcLog().clear();
        if (app.dualMode) app.decodersB.egcLog().clear();
    }
    static std::string egcCopy;
    ImGui::SameLine();
    copyAllButton(egcCopy);
    ImGui::SameLine();
    static bool showEgc = true, showTerminal = true;
    ImGui::Checkbox("EGC", &showEgc); ImGui::SameLine();
    ImGui::Checkbox("STDC", &showTerminal);
    ImGui::TextDisabled("Inmarsat-C SafetyNET / FleetNET / system messages.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##searchegc", "Search...", app.searchBuf, sizeof(app.searchBuf));

    // Session archive dropdown for EGC.
    if (!app.archiveDbLabels.empty())
    {
        std::vector<const char*> items;
        items.push_back("Live");
        for (auto& lbl : app.archiveDbLabels)
            items.push_back(lbl.c_str());
        ImGui::SetNextItemWidth(200);
        if (ImGui::Combo("Session", &app.archiveComboEgc, items.data(), (int)items.size()))
        {
            if (app.archiveComboEgc == 0)
                app.decoders.egcLog().clearArchive();
            else
            {
                int idx = app.archiveComboEgc - 1;
                if (idx < (int)app.archiveDbPaths.size())
                    app.writeDb.loadEgc(app.archiveDbPaths[idx], &app.decoders.egcLog());
            }
        }
        if (app.decoders.egcLog().hasArchive())
            ImGui::TextDisabled("  Viewing archived session");
    }
    ImGui::Separator();

    std::string searchLower;
    bool hasSearch = (app.searchBuf[0] != 0);
    if (hasSearch)
    {
        searchLower = app.searchBuf;
        for (auto& ch : searchLower) ch = (char)std::tolower((unsigned char)ch);
    }

    auto msgs = app.decoders.egcLog().snapshot();
    if (app.dualMode)
    {
        auto b = app.decodersB.egcLog().snapshot();
        msgs.insert(msgs.end(), b.begin(), b.end());
    }
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

        std::vector<std::string> copyRows;
        for (auto it = msgs.rbegin(); it != msgs.rend(); ++it)
        {
            bool isTerminal = (it->priority == "Terminal");
            if (isTerminal && !showTerminal) continue;
            if (!isTerminal && !showEgc) continue;
            if (hasSearch)
            {
                std::string hay = it->text + "|" + it->service + "|" + it->priority + "|" + it->timeUtc;
                for (auto& ch : hay) ch = (char)std::tolower((unsigned char)ch);
                if (hay.find(searchLower) == std::string::npos)
                    continue;
            }
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
            copyRows.push_back(copyFmt("%s\t%s\t%d\t%s\t%s",
                it->timeUtc.c_str(), it->priority.c_str(), it->messageId,
                it->service.c_str(), it->text.c_str()));
        }
        handleTableCopy(copyRows);
        egcCopy = copyJoin(copyRows);
        ImGui::EndTable();
    }

    ImGui::End();
}

void drawMes(App& app)
{
    ImGui::Begin((std::string(_L("MES")) + "###MES").c_str());

    auto entries = app.decoders.mesLog().snapshot();
    if (app.dualMode)
    {
        auto b = app.decodersB.mesLog().snapshot();
        entries.insert(entries.end(), b.begin(), b.end());
    }
    ImGui::Text("%zu terminal(s)", entries.size());
    ImGui::SameLine();
    if (ImGui::SmallButton(_L("Clear")))
    {
        app.decoders.mesLog().clear();
        if (app.dualMode) app.decodersB.mesLog().clear();
    }
    static std::string mesCopy;
    ImGui::SameLine();
    copyAllButton(mesCopy);
    ImGui::Separator();

    std::sort(entries.begin(), entries.end(),
              [](const MesEntry& a, const MesEntry& b) { return a.lastSeen > b.lastSeen; });

    double now = (double)std::time(nullptr);
    if (ImGui::BeginTable("##mes", 7,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable))
    {
        ImGui::TableSetupColumn("MES ID", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Sat", ImGuiTableColumnFlags_WidthFixed, 52);
        ImGui::TableSetupColumn("LES", ImGuiTableColumnFlags_WidthFixed, 36);
        ImGui::TableSetupColumn("Ch", ImGuiTableColumnFlags_WidthFixed, 32);
        ImGui::TableSetupColumn("Age", ImGuiTableColumnFlags_WidthFixed, 48);
        ImGui::TableSetupColumn("Msgs", ImGuiTableColumnFlags_WidthFixed, 48);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        std::vector<std::string> copyRows;
        for (auto& e : entries)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%u", e.mesId);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(e.action.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(e.sat.c_str());
            ImGui::TableNextColumn();
            if (e.les >= 0) ImGui::Text("%02d", e.les);
            ImGui::TableNextColumn();
            if (e.channel >= 0) ImGui::Text("%d", e.channel);
            ImGui::TableNextColumn();
            ImGui::Text("%ds", (int)(now - e.lastSeen));
            ImGui::TableNextColumn();
            ImGui::Text("%llu", (unsigned long long)e.msgs);
            copyRows.push_back(copyFmt("%u\t%s\t%s\t%d\t%d\t%ds\t%llu",
                e.mesId, e.action.c_str(), e.sat.c_str(), e.les, e.channel,
                (int)(now - e.lastSeen), (unsigned long long)e.msgs));
        }
        handleTableCopy(copyRows);
        mesCopy = copyJoin(copyRows);
        ImGui::EndTable();
    }

    ImGui::End();
}

void drawLes(App& app)
{
    ImGui::Begin((std::string(_L("LES")) + "###LES").c_str());

    unsigned long long lesTotal = app.decoders.lesLog().count();
    if (app.dualMode) lesTotal += app.decodersB.lesLog().count();
    ImGui::Text("%llu message(s)", lesTotal);
    ImGui::SameLine();
    if (ImGui::SmallButton(_L("Clear")))
    {
        app.decoders.lesLog().clear();
        if (app.dualMode) app.decodersB.lesLog().clear();
    }
    static std::string lesCopy;
    ImGui::SameLine();
    copyAllButton(lesCopy);
    ImGui::SameLine();
    static bool hideEncrypted = false;
    ImGui::Checkbox(_L("Hide encrypted"), &hideEncrypted);
    ImGui::TextDisabled("LES private ship/shore messages (0xAA non-EGC).");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##searchles", "Search...", app.searchBuf, sizeof(app.searchBuf));

    // Session archive dropdown for LES.
    if (!app.archiveDbLabels.empty())
    {
        std::vector<const char*> items;
        items.push_back("Live");
        for (auto& lbl : app.archiveDbLabels)
            items.push_back(lbl.c_str());
        ImGui::SetNextItemWidth(200);
        if (ImGui::Combo("Session", &app.archiveComboLes, items.data(), (int)items.size()))
        {
            if (app.archiveComboLes == 0)
                app.decoders.lesLog().clearArchive();
            else
            {
                int idx = app.archiveComboLes - 1;
                if (idx < (int)app.archiveDbPaths.size())
                    app.writeDb.loadLes(app.archiveDbPaths[idx], &app.decoders.lesLog());
            }
        }
        if (app.decoders.lesLog().hasArchive())
            ImGui::TextDisabled("  Viewing archived session");
    }
    ImGui::Separator();

    std::string searchLower;
    bool hasSearch = (app.searchBuf[0] != 0);
    if (hasSearch)
    {
        searchLower = app.searchBuf;
        for (auto& ch : searchLower) ch = (char)std::tolower((unsigned char)ch);
    }

    auto msgs = app.decoders.lesLog().snapshot();
    if (app.dualMode)
    {
        auto b = app.decodersB.lesLog().snapshot();
        msgs.insert(msgs.end(), b.begin(), b.end());
    }
    if (ImGui::BeginTable("##les", 6,
                          ImGuiTableFlags_Borders |
                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable))
    {
        ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 64);
        ImGui::TableSetupColumn("LES", ImGuiTableColumnFlags_WidthFixed, 160);
        ImGui::TableSetupColumn("Sat", ImGuiTableColumnFlags_WidthFixed, 52);
        ImGui::TableSetupColumn("Ch", ImGuiTableColumnFlags_WidthFixed, 32);
        ImGui::TableSetupColumn("Pkt", ImGuiTableColumnFlags_WidthFixed, 40);
        ImGui::TableSetupColumn("Message");
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        std::vector<std::string> copyRows;
        for (auto it = msgs.rbegin(); it != msgs.rend(); ++it)
        {
            if (hideEncrypted && it->isEncrypted) continue;
            if (hasSearch)
            {
                std::string hay = it->text + "|" + it->timeUtc + "|" + it->satName + "|" + it->lesLabel;
                for (auto& ch : hay) ch = (char)std::tolower((unsigned char)ch);
                if (hay.find(searchLower) == std::string::npos)
                    continue;
            }
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(it->timeUtc.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%s LES %02d", it->satName.c_str(), it->lesId);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(it->satName.c_str());
            ImGui::TableNextColumn();
            if (it->channel >= 0) ImGui::Text("%d", it->channel);
            ImGui::TableNextColumn();
            ImGui::Text("%d", it->pktNo);
            ImGui::TableNextColumn();
            if (it->isEncrypted)
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 60, 60, 255));
            else
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(60, 255, 60, 255));
            ImGui::TextWrapped("%s", it->text.c_str());
            ImGui::PopStyleColor();
            copyRows.push_back(copyFmt("%s\t%s LES %02d\t%s\t%d\t%d\t%s",
                it->timeUtc.c_str(), it->satName.c_str(), it->lesId,
                it->satName.c_str(), it->channel, it->pktNo, it->text.c_str()));
        }
        handleTableCopy(copyRows);
        lesCopy = copyJoin(copyRows);
        ImGui::EndTable();
    }

    ImGui::End();
}

ImPlotPoint constGetter(int idx, void* data)
{
    const float* p = static_cast<const float*>(data);
    return ImPlotPoint(p[idx * 2], p[idx * 2 + 1]);
}

void drawConstellation(App& app)
{
    ImGui::Begin((std::string(_L("Constellation")) + "###Constellation").c_str());

    auto decs = app.decoders.status();
    if (app.dualMode)
    {
        auto decsB = app.decodersB.status();
        for (auto& d : decsB) d.isB = true;
        decs.insert(decs.end(), decsB.begin(), decsB.end());
    }
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
    int preBaud = 0;
    bool preIsB = false;
    for (auto& d : decs)
    {
        if (d.channelId == chan)
        {
            preBaud = d.baud;
            preIsB = d.isB;
            break;
        }
    }
    char preview[128];
    const char* baudStr = (preBaud == kEgcBaud) ? "EGC" : nullptr;
    if (baudStr)
        std::snprintf(preview, sizeof(preview), "Channel %d  %.4f MHz  %s%s",
                      chan, freq, baudStr, preIsB ? " [B]" : "");
    else
        std::snprintf(preview, sizeof(preview), "Channel %d  %.4f MHz  @%d%s",
                      chan, freq, preBaud, preIsB ? " [B]" : "");
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("Decoder", preview))
    {
        for (auto& d : decs)
        {
            char label[64];
            const char* b = (d.baud == kEgcBaud) ? "EGC" : nullptr;
            if (b)
                std::snprintf(label, sizeof(label), "Channel %d  %.4f MHz  %s%s",
                              d.channelId, d.freqMHz, b,
                              d.isB ? " [B]" : "");
            else
                std::snprintf(label, sizeof(label), "Channel %d  %.4f MHz  @%d%s",
                              d.channelId, d.freqMHz, d.baud,
                              d.isB ? " [B]" : "");
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
    if (pairs == 0 && app.dualMode)
        pairs = app.decodersB.getConstellation(chan, app.constBuf, 1024);
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
// Voice Calls browser
// ---------------------------------------------------------------------------

// ---- Speech-to-text of recorded calls (stt/transcriber.h) -------------------
// Transcripts are kept by recording file name; each is also written next to
// the recording as a .txt (and read back from there after a restart).
namespace {
struct TxEntry { int state = 0; std::string lang, text, en; }; // 0 none, 1 queued, 2 done, 3 no speech, 4 failed
std::mutex g_txMtx;
std::map<std::string, TxEntry> g_tx;
std::map<int, std::string> g_txIdFile;
int g_txNextId = 1;
const double g_txAppStart = (double)std::time(nullptr);

std::string txTxtPath(const std::string& dir, const std::string& file) {
    std::string f = file;
    const size_t dot = f.find_last_of('.');
    if (dot != std::string::npos) f = f.substr(0, dot);
    return dir + "/" + f + ".txt";
}

// "[lang, whisper model]\n<text>\n\n[English translation]\n<text>"
bool txLoadTxt(const std::string& path, TxEntry& e) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::stringstream ss; ss << in.rdbuf();
    std::string all = ss.str(), head;
    const size_t nl = all.find('\n');
    if (nl == std::string::npos) return false;
    head = all.substr(0, nl);
    std::string body = all.substr(nl + 1);
    if (head.size() > 2 && head[0] == '[') e.lang = head.substr(1, head.find_first_of(",]") - 1);
    const std::string mark = "\n[English translation]\n";
    const size_t m = body.find(mark);
    if (m != std::string::npos) { e.en = body.substr(m + mark.size()); body = body.substr(0, m); }
    while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) body.pop_back();
    while (!e.en.empty() && (e.en.back() == '\n' || e.en.back() == '\r')) e.en.pop_back();
    e.text = body;
    e.state = 2;
    return true;
}

void txQueue(App& app, const std::string& file) {
    const std::string path = std::string(app.recordDir) + "/" + file;
    int id;
    {
        std::lock_guard<std::mutex> lk(g_txMtx);
        id = g_txNextId++;
        g_txIdFile[id] = file;
        g_tx[file].state = 1;
    }
    stt::Transcriber::instance().submit_loader(
        id, [path] { std::vector<int16_t> pcm; AudioPlayer::loadPcm(path, pcm); return pcm; },
        txTxtPath(app.recordDir, file), [](int cid, const stt::Result& r) {
            std::lock_guard<std::mutex> lk(g_txMtx);
            auto f = g_txIdFile.find(cid);
            if (f == g_txIdFile.end()) return;
            TxEntry& e = g_tx[f->second];
            e.state = !r.ok ? 4 : r.text.empty() ? 3 : 2;
            e.lang = r.lang; e.text = r.text; e.en = r.text_en;
        });
}

TxEntry txGet(const std::string& file) {
    std::lock_guard<std::mutex> lk(g_txMtx);
    auto it = g_tx.find(file);
    return it == g_tx.end() ? TxEntry{} : it->second;
}

// New finished recordings: read an existing .txt, or queue the ones that ended
// since InmarScope started (older ones get a Transcribe button).
void txScan(App& app, const std::vector<VoiceCallRecord>& calls) {
    auto& tx = stt::Transcriber::instance();
    tx.set_enabled(app.sttOn);
    tx.set_translate(app.sttTranslate);
    tx.set_large_for_all(app.sttLargeAll);
    for (const auto& c : calls) {
        if (c.recording || c.filename.empty()) continue;
        {
            std::lock_guard<std::mutex> lk(g_txMtx);
            if (g_tx.count(c.filename)) continue;
            g_tx[c.filename];   // seen
        }
        TxEntry e;
        if (txLoadTxt(txTxtPath(app.recordDir, c.filename), e)) {
            std::lock_guard<std::mutex> lk(g_txMtx);
            g_tx[c.filename] = e;
        } else if (app.sttOn && tx.model_state(0) == stt::ModelState::Ready && c.durationSec >= 3.0 &&
                   c.timeSec + c.durationSec >= g_txAppStart - 5) {
            txQueue(app, c.filename);
        }
    }
}

void drawTxPanel(App& app, const std::string& file) {
    auto& tx = stt::Transcriber::instance();
    const TxEntry e = txGet(file);
    if (e.state == 2) {
        ImGui::TextDisabled("Transcript (%s)", e.lang.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy##tx")) {
            std::string s = e.text;
            if (!e.en.empty()) s += "\n\nEnglish:\n" + e.en;
            ImGui::SetClipboardText(s.c_str());
        }
        const float h = ImGui::GetTextLineHeightWithSpacing() * (e.en.empty() ? 4.5f : 7.5f);
        ImGui::BeginChild("##txtext", ImVec2(0, h), true);
        ImGui::TextWrapped("%s", e.text.c_str());
        if (!e.en.empty()) { ImGui::Spacing(); ImGui::TextDisabled("English:"); ImGui::TextWrapped("%s", e.en.c_str()); }
        ImGui::EndChild();
    } else if (e.state == 1) {
        ImGui::TextDisabled("Transcribing / queued (%d waiting)", tx.queued());
    } else if (e.state == 3) {
        ImGui::TextDisabled("Transcript: no speech found");
    } else if (!app.sttOn || tx.model_state(0) != stt::ModelState::Ready) {
        ImGui::TextDisabled("No transcript.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Set up transcription...")) app.showStt = true;
    } else if (ImGui::SmallButton(e.state == 4 ? "Transcribe again" : "Transcribe")) {
        txQueue(app, file);
    }
}

void drawTranscriptionWindow(App& app) {
    if (!app.showStt) return;
    ImGui::SetNextWindowSize(ImVec2(560, 420), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Transcription###stt", &app.showStt)) {
        auto& tx = stt::Transcriber::instance();
        if (!stt::Transcriber::available()) {
            ImGui::TextWrapped("This build doesn't include speech-to-text: whisper.cpp wasn't found when it was "
                               "compiled. See the build notes for how to add it.");
            ImGui::End();
            return;
        }
        static bool vocabSet = false;
        if (!vocabSet) { tx.set_vocabulary(app.sttVocab); vocabSet = true; }
        ImGui::TextWrapped("Recorded voice calls are turned into text on this PC (whisper.cpp; nothing is sent "
                           "anywhere), after each call ends and below normal priority. The text is shown in "
                           "Voice Calls and saved next to the recording as a .txt.");
        ImGui::Separator();
        static const char* names[2] = {"Small model (every call, detects language)",
                                       "Large-v3-turbo (re-does non-English calls, optional)"};
        for (int m = 0; m < 2; m++) {
            ImGui::PushID(m);
            ImGui::TextUnformatted(names[m]);
            const stt::ModelState st = tx.model_state(m);
            if (st == stt::ModelState::Ready) {
                ImGui::SameLine(); ImGui::TextColored(ImVec4(0.5f, 1, 0.5f, 1), "ready");
            } else if (st == stt::ModelState::Downloading) {
                char b[48]; std::snprintf(b, sizeof(b), "%.0f of %.0f MB", tx.download_progress(m) * tx.model_size_mb(m), tx.model_size_mb(m));
                ImGui::ProgressBar(tx.download_progress(m), ImVec2(260, 0), b);
                ImGui::SameLine();
                if (ImGui::SmallButton("Cancel")) tx.cancel_download(m);
            } else {
                char b[48]; std::snprintf(b, sizeof(b), "Download (%.0f MB)", tx.model_size_mb(m));
                if (ImGui::Button(b)) tx.download(m);
                if (st == stt::ModelState::Failed) { ImGui::SameLine(); ImGui::TextColored(ImVec4(1, .6f, .4f, 1), "download failed"); }
            }
            ImGui::PopID();
        }
        ImGui::Separator();
        const bool ready = tx.model_state(0) == stt::ModelState::Ready;
        if (!ready) ImGui::BeginDisabled();
        ImGui::Checkbox("Transcribe recorded calls", &app.sttOn);
        if (!ready) { ImGui::EndDisabled(); ImGui::SameLine(); ImGui::TextDisabled("(download the small model first)"); }
        ImGui::Checkbox("Use the large model for all calls", &app.sttLargeAll);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("More accurate (names, accents, choppy audio) but about 3-4x slower and\n"
                              "~1 GB more memory while working. Needs the large model; without it every\n"
                              "call uses the small one. Off: large only re-does non-English calls.");
        ImGui::Checkbox("Translate non-English calls to English", &app.sttTranslate);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Adds an English version under the original text. Uses the large model if\n"
                              "downloaded. Treat translations as rough: on choppy audio Whisper can\n"
                              "invent sentences when translating.");
        ImGui::TextDisabled("Words to expect (names, jargon; English calls only):");
        if (ImGui::InputTextMultiline("##vocab", app.sttVocab, sizeof(app.sttVocab), ImVec2(-1, ImGui::GetTextLineHeight() * 4)))
            tx.set_vocabulary(app.sttVocab);
        ImGui::Separator();
        if (tx.busy_id() >= 0) ImGui::Text("Transcribing, %d waiting", tx.queued());
        else ImGui::TextDisabled("Idle, %d waiting", tx.queued());
        ImGui::TextDisabled("Models are stored in the models folder next to InmarScope.exe.");
    }
    ImGui::End();
}
} // namespace

void drawVoiceCalls(App& app)
{
    ImGui::Begin((std::string(_L("Voice Calls")) + "###Voice Calls").c_str());
    drawRecordVoiceToggle(app);
    if (app.recordVoice)
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "REC (%d active, %s)",
                          app.decoders.recordingCount() + (app.dualMode ? app.decodersB.recordingCount() : 0),
                          app.recordFormat ? "OGG" : "WAV");

    auto calls = app.decoders.voiceCallLog().snapshot();
    if (app.dualMode)
    {
        auto b = app.decodersB.voiceCallLog().snapshot();
        calls.insert(calls.end(), b.begin(), b.end());
    }
    // Sort newest first
    std::sort(calls.begin(), calls.end(),
              [](const VoiceCallRecord& a, const VoiceCallRecord& b) { return a.timeSec > b.timeSec; });

    ImGui::Text("%llu calls", (unsigned long long)app.decoders.voiceCallLog().count() +
                               (app.dualMode ? app.decodersB.voiceCallLog().count() : 0));
    ImGui::SameLine();
    if (ImGui::SmallButton(_L("Clear")))
    {
        app.decoders.voiceCallLog().clear();
        if (app.dualMode) app.decodersB.voiceCallLog().clear();
    }
    static std::string vcCopy;
    ImGui::SameLine();
    copyAllButton(vcCopy);
    ImGui::SameLine();
    if (ImGui::SmallButton("Rescan"))
    {
        app.decoders.voiceCallLog().scanDir(app.recordDir);
        if (app.dualMode) app.decodersB.voiceCallLog().scanDir(app.recordDir);
    }
    ImGui::SameLine();

    // Playback status + Pause/Resume and Stop for the loaded call.
    if (app.audioPlayer.isLoaded()) {
        if (app.audioPlayer.isPaused()) {
            if (ImGui::SmallButton("Resume##vcpr")) app.audioPlayer.resume();
        } else if (ImGui::SmallButton("Pause##vcpr")) app.audioPlayer.pause();
        ImGui::SameLine();
        if (ImGui::SmallButton("Stop##vcps")) app.audioPlayer.stop();
        ImGui::SameLine();
    }
    if (app.audioPlayer.isPlaying())
        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.4f, 1.0f), "  Playing... %.0fs", (double)app.audioPlayer.positionSec());
    else if (app.audioPlayer.isPaused())
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "  Paused at %.0fs", (double)app.audioPlayer.positionSec());
    else
        ImGui::TextDisabled("  Idle");

    // Waveform of the selected (or playing) call.
    static std::string wfFile;            // filename whose audio is loaded
    static std::vector<int16_t> wfPcm;
    static std::string selFile;           // selected row (by filename)
    if (!selFile.empty() && selFile != wfFile) {
        wfFile = selFile;
        if (!AudioPlayer::loadPcm(std::string(app.recordDir) + "/" + selFile, wfPcm)) wfPcm.clear();
    }
    if (!selFile.empty()) {
        const bool loadedSel = app.audioPlayer.isLoaded() &&
                               app.audioPlayer.currentPath().find(selFile) != std::string::npos;
        ImGui::TextDisabled("%s", selFile.c_str());
        const float at = drawCallWaveform(app, wfPcm, loadedSel, app.audioPlayer.positionSec());
        if (at >= 0.0f) {
            // Click: jump there in the playing/paused call, or start it there.
            if (!loadedSel) app.audioPlayer.play(std::string(app.recordDir) + "/" + selFile);
            app.audioPlayer.seek(at);
        }
        drawTxPanel(app, selFile);
    }
    txScan(app, calls);

    if (ImGui::BeginTable("##vclist", 6,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable))
    {
        ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 68);
        ImGui::TableSetupColumn("Freq", ImGuiTableColumnFlags_WidthFixed, 78);
        ImGui::TableSetupColumn("ICAO", ImGuiTableColumnFlags_WidthFixed, 64);
        ImGui::TableSetupColumn("Duration", ImGuiTableColumnFlags_WidthFixed, 72);
        ImGui::TableSetupColumn(">", ImGuiTableColumnFlags_WidthFixed, 24);
        ImGui::TableSetupColumn("Transcript");
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        int rowIdx = 0;
        std::vector<std::string> copyRows;
        for (auto& c : calls)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            // Time
            time_t t = (time_t)c.timeSec;
            std::tm tm{};
#if defined(_WIN32)
            localtime_s(&tm, &t);
#else
            localtime_r(&t, &tm);
#endif
            {
                char tl[32];
                std::snprintf(tl, sizeof(tl), "%02d:%02d:%02d##vcsel%d", tm.tm_hour, tm.tm_min, tm.tm_sec, rowIdx);
                // Click the time to select the call and show its waveform.
                if (ImGui::Selectable(tl, !c.filename.empty() && c.filename == selFile, 0,
                                      ImGui::CalcTextSize("00:00:00")))
                    selFile = c.filename;
            }

            ImGui::TableNextColumn();
            ImGui::Text("%.4f", c.freqMHz);

            ImGui::TableNextColumn();
            {
                // Click the ICAO address to see the aircraft on the Flight Map.
                std::string ic = !c.icao.empty() ? c.icao : (c.aesId ? copyFmt("%06X", c.aesId) : std::string());
                if (!ic.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, Lc(app, ImVec4(1.0f, 0.85f, 0.3f, 1.0f)));
                    ImGui::PushID(rowIdx);
                    if (ImGui::Selectable(ic.c_str(), false, 0, ImGui::CalcTextSize(ic.c_str())))
                        showAircraftOnMap(app, c.aesId, c.icao);
                    ImGui::PopID();
                    ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show this aircraft on the Flight Map");
                } else
                    ImGui::TextUnformatted("--");
            }

            ImGui::TableNextColumn();
            // Duration
            if (c.recording)
            {
                double nowSec = (double)std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
                double liveDur = (nowSec > c.timeSec) ? (nowSec - c.timeSec) : 0.0;
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Rec %.0fs", liveDur);
            }
            else if (c.durationSec > 0.0)
            {
                int m = (int)c.durationSec / 60;
                int s = (int)c.durationSec % 60;
                ImGui::Text("%d:%02d", m, s);
            }
            else
                ImGui::TextUnformatted("--");

            ImGui::TableNextColumn();
            // Play button — use row index for unique ImGui ID across A/B merge.
            // Playing -> "||" pauses; paused -> ">" resumes where it was;
            // otherwise ">" plays from the start.
            const bool loaded = app.audioPlayer.isLoaded() && !c.filename.empty() &&
                                app.audioPlayer.currentPath().find(c.filename) != std::string::npos;
            const bool sel = loaded && app.audioPlayer.isPlaying();
            char label[24];
            std::snprintf(label, sizeof(label), "%s##vcp%d", sel ? "||" : ">", rowIdx);
            if (ImGui::SmallButton(label))
            {
                if (sel)
                    app.audioPlayer.pause();
                else if (loaded && app.audioPlayer.isPaused())
                    app.audioPlayer.resume();
                else if (!c.filename.empty())
                {
                    std::string fullPath = std::string(app.recordDir) + "/" + c.filename;
                    app.audioPlayer.play(fullPath);
                    selFile = c.filename;
                }
            }
            ImGui::TableNextColumn();
            if (!c.filename.empty()) {
                const TxEntry te = txGet(c.filename);
                if (te.state == 1) ImGui::TextDisabled("queued");
                else if (te.state == 3) ImGui::TextDisabled("(no speech)");
                else if (te.state == 4) ImGui::TextDisabled("(failed)");
                else if (te.state == 2) {
                    ImGui::TextDisabled("[%s]", te.lang.c_str());
                    ImGui::SameLine(0, 4);
                    ImGui::TextUnformatted(te.text.c_str());
                    if (ImGui::IsItemHovered()) {
                        ImGui::BeginTooltip();
                        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40.0f);
                        ImGui::TextUnformatted(te.text.c_str());
                        if (!te.en.empty()) { ImGui::Separator(); ImGui::TextDisabled("English:"); ImGui::TextUnformatted(te.en.c_str()); }
                        ImGui::PopTextWrapPos();
                        ImGui::EndTooltip();
                    }
                }
            }
            copyRows.push_back(copyFmt("%02d:%02d:%02d\t%.4f\t%s\t%s",
                tm.tm_hour, tm.tm_min, tm.tm_sec, c.freqMHz,
                c.icao.empty() ? (c.aesId ? copyFmt("%06X", c.aesId).c_str() : "--") : c.icao.c_str(),
                c.filename.c_str()));
            rowIdx++;
        }
        handleTableCopy(copyRows);
        vcCopy = copyJoin(copyRows);
        ImGui::EndTable();
    }

    ImGui::End();
    drawTranscriptionWindow(app);
}

// ---------------------------------------------------------------------------
// LES Frequencies browser
// ---------------------------------------------------------------------------

void drawLesFreq(App& app)
{
    ImGui::Begin((std::string(_L("LES Freq")) + "###LES Freq").c_str());

    if (app.autoAddLes && app.active->running())
    {
        double center = app.active->centerFreq();
        double halfSpan = app.active->sampleRate() / 2.0;
        auto ents = app.decoders.lesFreqTable().snapshot();
        int added = 0;
        for (auto& e : ents)
        {
            if (e.hasDecoder) continue;
            if (added >= app.maxLesAutoDecoders) break;
            double offset = std::fabs(e.freqMHz * 1e6 - center);
            if (offset > halfSpan * 0.95) continue;
            bool dup = false;
            for (auto& s : app.decoders.status())
                if (std::fabs(s.freqMHz - e.freqMHz) < 0.001 && s.baud == kEgcBaud)
                    { dup = true; break; }
            if (dup) continue;
            int id = app.decoders.addDecoder(e.freqMHz * 1e6, kEgcBaud);
            if (id >= 0)
            {
                app.decoders.lesFreqTable().setHasDecoder(e.freqMHz, true);
                ++added;
            }
        }
        if (app.dualMode && app.activeB->running())
        {
            double centerB = app.activeB->centerFreq();
            double halfSpanB = app.activeB->sampleRate() / 2.0;
            for (auto& e : ents)
            {
                if (e.hasDecoder) continue;
                if (added >= app.maxLesAutoDecoders) break;
                double offset = std::fabs(e.freqMHz * 1e6 - centerB);
                if (offset > halfSpanB * 0.95) continue;
                bool dup = false;
                for (auto& s : app.decodersB.status())
                    if (std::fabs(s.freqMHz - e.freqMHz) < 0.001 && s.baud == kEgcBaud)
                        { dup = true; break; }
                if (dup) continue;
                int id = app.decodersB.addDecoder(e.freqMHz * 1e6, kEgcBaud);
                if (id >= 0)
                {
                    app.decodersB.lesFreqTable().setHasDecoder(e.freqMHz, true);
                    ++added;
                }
            }
        }
    }

    auto ents = app.decoders.lesFreqTable().snapshot();
    if (app.dualMode)
    {
        auto b = app.decodersB.lesFreqTable().snapshot();
        for (auto& e : b)
        {
            bool dup = false;
            for (auto& ea : ents)
                if (std::fabs(ea.freqMHz - e.freqMHz) < 0.001) { dup = true; break; }
            if (!dup) ents.push_back(e);
        }
    }
    std::sort(ents.begin(), ents.end(),
              [](const LesFreqEntry& a, const LesFreqEntry& b) { return a.freqMHz < b.freqMHz; });

    ImGui::Text("%d discovered", (int)ents.size());
    static std::string lesfCopy;
    ImGui::SameLine();
    copyAllButton(lesfCopy);
    ImGui::SameLine();
    ImGui::Checkbox("Auto-add", &app.autoAddLes);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(42);
    ImGui::SliderInt("max", &app.maxLesAutoDecoders, 0, 8);

    if (ImGui::BeginTable("##lesft", 6,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable))
    {
        ImGui::TableSetupColumn("Freq", ImGuiTableColumnFlags_WidthFixed, 78);
        ImGui::TableSetupColumn("Sat");
        ImGui::TableSetupColumn("LES");
        ImGui::TableSetupColumn("Svc", ImGuiTableColumnFlags_WidthFixed, 72);
        ImGui::TableSetupColumn("Seen");
        ImGui::TableSetupColumn("+");
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        std::vector<std::string> copyRows;
        for (auto& e : ents)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%.3f", e.freqMHz);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(e.satName.c_str());
            ImGui::TableNextColumn();
            if (e.lesLabel.empty())
                ImGui::TextDisabled("LES %02d", e.lesId);
            else
                ImGui::TextUnformatted(e.lesLabel.c_str());
            ImGui::TableNextColumn();
            char svc[32];
            std::snprintf(svc, sizeof(svc), "%04X", e.services);
            ImGui::TextUnformatted(svc);
            ImGui::TableNextColumn();
            double age = (double)std::time(nullptr) - e.lastSeen;
            if (age < 60.0)
                ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%.0fs", age);
            else
                ImGui::TextDisabled("%.0fs", age);
            ImGui::TableNextColumn();
            if (e.hasDecoder)
                ImGui::TextDisabled("ON");
            else
            {
                char lbl[24];
                std::snprintf(lbl, sizeof(lbl), "Add##lesf%d", (int)(e.freqMHz * 10000));
                if (ImGui::SmallButton(lbl))
                {
                    double offsetA = app.active->running() ? std::fabs(e.freqMHz * 1e6 - app.active->centerFreq()) : 1e12;
                    double offsetB = (app.dualMode && app.activeB->running()) ? std::fabs(e.freqMHz * 1e6 - app.activeB->centerFreq()) : 1e12;
                    if (offsetA < app.active->sampleRate() / 2.0)
                    {
                        app.decoders.addDecoder(e.freqMHz * 1e6, kEgcBaud);
                        app.decoders.lesFreqTable().setHasDecoder(e.freqMHz, true);
                    }
                    else if (offsetB < app.activeB->sampleRate() / 2.0)
                    {
                        app.decodersB.addDecoder(e.freqMHz * 1e6, kEgcBaud);
                        app.decodersB.lesFreqTable().setHasDecoder(e.freqMHz, true);
                    }
                }
            }
            copyRows.push_back(copyFmt("%.3f\t%s\t%s\t%04X",
                e.freqMHz, e.satName.c_str(),
                e.lesLabel.empty() ? copyFmt("LES %02d", e.lesId).c_str() : e.lesLabel.c_str(),
                e.services));
        }
        handleTableCopy(copyRows);
        lesfCopy = copyJoin(copyRows);
        ImGui::EndTable();
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
// About dialog
// ---------------------------------------------------------------------------

void drawAbout(App& app)
{
    if (!app.showAbout)
        return;

    ImGui::SetNextWindowSize(ImVec2(520, 480), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::Begin((std::string(_L("About InmarScope")) + "###About InmarScope").c_str(), &app.showAbout,
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoDocking |
                     ImGuiWindowFlags_NoCollapse))
    {
        ImGui::BeginChild("##about_scroll", ImVec2(0, 0));
        ImGui::TextWrapped("InmarScope v" INMARSCOPE_VERSION);
        ImGui::Separator();
        ImGui::TextWrapped("InmarScope was created by Sarah Rose, who wrote and maintains the core decoder, receiver and UI.");
        ImGui::Spacing();
        ImGui::TextWrapped("Contributors:");
        ImGui::Bullet();
        ImGui::SameLine();
        ImGui::TextWrapped("blkph0x (ben) - SDRplay receiver support, cross-platform build/release CI, regional band plans, the received-aircraft flight map, and UI workflow improvements (copyable panes, pop-out windows, PPM adjustment).");
        ImGui::Bullet();
        ImGui::SameLine();
        ImGui::TextWrapped("z1000biker - native ADALM-Pluto / Pluto+ (AD936x) support and ICAO population from Classic Aero AES IDs.");
        ImGui::Bullet();
        ImGui::SameLine();
        ImGui::TextWrapped("Brumi-2021 - touchpad pinch-to-zoom on Windows and Linux, and a crash fix for the JFFT header mismatch.");
        ImGui::Bullet();
        ImGui::SameLine();
        ImGui::TextWrapped("gprime31 - RTL-TCP source fixes.");
        ImGui::Bullet();
        ImGui::SameLine();
        ImGui::TextWrapped("KiwifruitDev - documentation and build instructions, plus README and screenshot updates.");
        ImGui::Spacing();
        ImGui::TextWrapped("Built with components from:");
        ImGui::TextDisabled("  JAERO (Jontio)");
        ImGui::TextDisabled("  inmarsat-sniffer");
        ImGui::TextDisabled("  libaeroambe");
        ImGui::TextDisabled("  mbelib (dsd)");
        ImGui::TextDisabled("  scytaleC (Thierry Leconte)");
        ImGui::TextDisabled("  DeDECTive (Sarah Rose)");
        ImGui::Spacing();
        ImGui::TextWrapped("Thanks to Arclamp VK4SUS for providing a server for accessing the satellite during development.");
        ImGui::Spacing();
        ImGui::TextWrapped("Thanks to Mike AA8IA for donating an Airspy R2 and Airspy Mini for development.");
        ImGui::EndChild();
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Persistent settings: serialized into inmarscope.ini alongside the ImGui dock
// layout via a custom settings handler.
// ---------------------------------------------------------------------------

void drawDockHost(App& app)
{
    // Default to NOT forcing a rebuild: if inmarscope.ini holds a saved layout,
    // the dock node already exists and we keep it. Only build the default layout
    // on first run (no node) or when explicitly forced (Reset Layout / dual /
    // a layout-version bump).
    static bool forceLayout = false;
    const auto& layoutIo = ImGui::GetIO();
    if (!layoutIo.WantTextInput && layoutIo.KeyCtrl && layoutIo.KeyShift && ImGui::IsKeyPressed(ImGuiKey_R, false))
        forceLayout = true;
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
    ImGui::DockSpace(dockId, ImVec2(0, 0), ImGuiDockNodeFlags_None);

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

        ImGui::DockBuilderDockWindow((std::string(_L("Control")) + "###Control").c_str(), ctrl);
        ImGui::DockBuilderDockWindow((std::string(_L("Decoders")) + "###Decoders").c_str(), dec);

        ImGui::DockBuilderDockWindow((std::string(_L("Spectrum")) + "###Spectrum").c_str(), rtop);
        ImGui::DockBuilderDockWindow((std::string(_L("Waterfall")) + "###Waterfall").c_str(), rmid);
        // Always split for potential dual-mode: B windows are invisible when
        // not in dual mode, and the A windows fill the space.
        {
            ImGuiID rtopR, rmidR;
            ImGui::DockBuilderSplitNode(rtop, ImGuiDir_Right, 0.5f, &rtopR, &rtop);
            ImGui::DockBuilderSplitNode(rmid, ImGuiDir_Right, 0.5f, &rmidR, &rmid);
            ImGui::DockBuilderDockWindow((std::string(_L("Spectrum")) + "###Spectrum").c_str(), rtop);
            ImGui::DockBuilderDockWindow((std::string(_L("Waterfall")) + "###Waterfall").c_str(), rmid);
            ImGui::DockBuilderDockWindow((std::string(_L("Spectrum (B)")) + "###Spectrum (B)").c_str(), rtopR);
            ImGui::DockBuilderDockWindow((std::string(_L("Waterfall (B)")) + "###Waterfall (B)").c_str(), rmidR);
        }
#if defined(_WIN32)
        ImGui::DockBuilderDockWindow((std::string(_L("Flight Map")) + "###Flight Map").c_str(), rmid);
#endif
        ImGui::DockBuilderDockWindow((std::string(_L("SUs")) + "###SUs").c_str(), rbot);
        ImGui::DockBuilderDockWindow((std::string(_L("Messages")) + "###Messages").c_str(), rbot);
        ImGui::DockBuilderDockWindow((std::string(_L("C-Channel")) + "###C-Channel").c_str(), rbot);
        ImGui::DockBuilderDockWindow((std::string(_L("Network")) + "###Network").c_str(), rbot);
        ImGui::DockBuilderDockWindow((std::string(_L("EGC")) + "###EGC").c_str(), rbot);
        ImGui::DockBuilderDockWindow((std::string(_L("MES")) + "###MES").c_str(), rbot);
        ImGui::DockBuilderDockWindow((std::string(_L("LES")) + "###LES").c_str(), rbot);
        ImGui::DockBuilderDockWindow((std::string(_L("Aircraft")) + "###Aircraft").c_str(), rbot);
        ImGui::DockBuilderDockWindow((std::string(_L("Voice Calls")) + "###Voice Calls").c_str(), rbot);
        ImGui::DockBuilderDockWindow((std::string(_L("LES Freq")) + "###LES Freq").c_str(), rbot);
        ImGui::DockBuilderDockWindow((std::string(_L("Constellation")) + "###Constellation").c_str(), rcon);
        ImGui::DockBuilderFinish(dockId);
        ImGui::MarkIniSettingsDirty();
    }

    if (ImGui::BeginMenuBar())
    {
        if (ImGui::Button("Reset pane layout")) forceLayout = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Restore the original pane positions and sizes, including floating panes. Radio settings are preserved. Ctrl+Shift+R");
        if (ImGui::BeginMenu(_L("View")))
        {
            if (ImGui::MenuItem("Reset pane layout to default", "Ctrl+Shift+R"))
                forceLayout = true;
            ImGui::Separator();
            ImGui::MenuItem("Transcription...", nullptr, &app.showStt);
            ImGui::MenuItem(_L("Drag a tab out to float a pane on the desktop"), nullptr, false, false);
            ImGui::MenuItem(_L("Right-click a table row (or Ctrl+C) to copy"), nullptr, false, false);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(_L("Help")))
        {
            if (ImGui::BeginMenu(_L("Languages")))
            {
                for (int i = 0; i < (int)Lang::KOUNT; ++i)
                {
                    Lang l = (Lang)i;
                    if (ImGui::MenuItem(i18nName(l), nullptr, app.languageIdx == i))
                    {
                        app.languageIdx = i;
                        i18nSet(l);
                    }
                }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem(_L("About")))
                app.showAbout = true;
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    ImGui::End();
}
