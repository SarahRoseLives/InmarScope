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

void drawControls(App& app)
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

    // Band plan bar along bottom of spectrum
    if (ImGui::Checkbox("Band Plan", &app.showBandPlan));
    if (app.showBandPlan)
    {
        ImGui::SameLine();
        if (ImGui::SmallButton("Reload##bpr"))
            scanBandPlans(app.bandPlanDir, app.bandPlanNames, app.bandPlanPaths);
        ImGui::SameLine();
        if (ImGui::SmallButton("Folder##bpf"))
        {
#if defined(_WIN32)
            ShellExecuteA(nullptr, "open", app.bandPlanDir, nullptr, nullptr, SW_SHOW);
#endif
        }
        if (app.bandPlanNames.empty())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("(no .json bandplans in bandplans/)");
        }
        else
        {
            if (app.bandPlanIdx >= (int)app.bandPlanNames.size())
                app.bandPlanIdx = 0;
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##bplan-sel", &app.bandPlanIdx,
                             [](void* data, int idx) -> const char* {
                                 auto& v = *(std::vector<std::string>*)data;
                                 return idx >= 0 && idx < (int)v.size() ? v[idx].c_str() : "";
                             },
                             &app.bandPlanNames, (int)app.bandPlanNames.size()))
            {
                if (app.bandPlanIdx >= 0 && app.bandPlanIdx < (int)app.bandPlanPaths.size())
                    app.bandPlanLoaded = loadBandPlan(app.bandPlanPaths[app.bandPlanIdx]);
            }
        }
    }

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
    if (ImGui::CollapsingHeader("CallHunter (auto-scan for voice)"))
    {
        ImGui::Checkbox("Enable CallHunter", &app.callHunterMode);
        ImGui::SliderFloat("Threshold (dB above baseline)", &app.callHunterThreshDB, 1.0f, 20.0f, "%.1f");
        ImGui::SliderInt("Confirm frames", &app.callHunterConfirm, 5, 60);
        ImGui::SliderInt("Lost frames", &app.callHunterLost, 10, 120);

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

    ImGui::Separator();
    if (ImGui::CollapsingHeader("IQ Recorder"))
    {
        ImGui::SetNextItemWidth(-70.0f);
        ImGui::InputText("IQ file", app.iqRecPath, sizeof(app.iqRecPath));
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
            if (app.placingDecoder)
            {
                app.placingFreqMHz = mp.x;
            }
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && app.placingDecoder)
            {
                app.placingDecoder = false;
                int baud;
                if (voiceView)
                    baud = 8400;
                else
                {
                    static const int kBaudVals[] = {600, 1200, 8400, 10500, kEgcBaud};
                    int idx = app.newBaud < 0 ? 0 : (app.newBaud > 4 ? 4 : app.newBaud);
                    baud = kBaudVals[idx];
                }
                mgr.addDecoder(mp.x * 1e6, baud);
            }
        }
        else if (app.placingDecoder && app.placingVoiceView == voiceView)
        {
            // Ctrl released or cursor left the plot that started the placement.
            app.placingDecoder = false;
        }

        // White preview line while drag-placing (drawn on the ImDrawList over
        // the plot so it appears in both the spectrum and waterfall).
        if (app.placingDecoder)
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

        // --- Band plan: solid coloured bar along the bottom (on top of everything) ---
        if (app.showBandPlan && app.bandPlanLoaded.valid && v.curN > 0 &&
            app.bandPlanIdx >= 0 && app.bandPlanIdx < (int)app.bandPlanNames.size())
        {
            const auto& bp = app.bandPlanLoaded;
            const ImPlotRect vp = ImPlot::GetPlotLimits();
            double viewLo = vp.X.Min, viewHi = vp.X.Max;
            if (viewHi <= viewLo) { viewLo = v.freqMHz.front(); viewHi = v.freqMHz.back(); }
            auto* dl = ImPlot::GetPlotDrawList();
            constexpr float kBandH = 28.0f;
            ImVec2 pp = ImPlot::GetPlotPos(), ps = ImPlot::GetPlotSize();
            float bandTop = pp.y + ps.y - kBandH;
            float bandBot = bandTop + kBandH;
            float pxPerMHz = (float)(ps.x / (viewHi - viewLo));
            for (auto& e : bp.entries)
            {
                if (e.hiMHz < viewLo || e.loMHz > viewHi) continue;
                float loPx = pp.x + (float)((std::max(e.loMHz, viewLo) - viewLo) * pxPerMHz);
                float hiPx = pp.x + (float)((std::min(e.hiMHz, viewHi) - viewLo) * pxPerMHz);
                dl->AddRectFilled(ImVec2(loPx, bandTop), ImVec2(hiPx, bandBot), e.color);
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
        }

        ImPlot::EndPlot();
    }
    ImGui::End();
}

void drawWaterfall(App& app, SpectrumView& v, const char* title)
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

    ImVec2 wfP0 = ImGui::GetCursorScreenPos();
    v.waterfall.draw(ImVec2(w, avail.y), uMin, uMax, xLo, xHi);

    // Drag-to-place preview line: white vertical line through the waterfall
    // at the frequency the user is hovering, so they can centre on a signal.
    if (app.placingDecoder && v.curN > 0)
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

void drawDecoders(App& app)
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
    float lvl = app.decoders.audioLevel() * 5.0f;
    if (lvl > 1.0f) lvl = 1.0f;
    ImGui::ProgressBar(lvl, ImVec2(110, 0), "");
    ImGui::SameLine();
    if (ImGui::Checkbox("Mute", &app.voiceMuted))
    {
        app.decoders.setVoiceMute(app.voiceMuted);
        app.decodersB.setVoiceMute(app.voiceMuted);
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

    if (ImGui::Checkbox("Record voice calls", &app.recordVoice))
    {
        app.decoders.setRecording(app.recordVoice, app.recordDir);
        app.decodersB.setRecording(app.recordVoice, app.recordDir);
        // Re-apply the format so it always matches the current combo choice.
        RecordFormat rf = (app.recordFormat == 1) ? RecordFormat::OGG : RecordFormat::WAV;
        app.decoders.setRecordFormat(rf);
        app.decodersB.setRecordFormat(rf);
    }
    ImGui::SameLine();
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
            {
                app.selectedDecoder = d.channelId;
                if (d.isVoice)
                    app.decoders.setVoiceMonitor(d.channelId);
            }
            ImGui::SameLine();
            ImVec4 c = d.monitored ? ImVec4(0.3f, 0.5f, 1.0f, 1.0f)   // blue = active monitor
                     : d.locked    ? ImVec4(0.2f, 1.0f, 0.3f, 1.0f)   // green = locked
                                   : ImVec4(0.5f, 0.5f, 0.5f, 1.0f);  // gray
            ImGui::TextColored(c, "%s", d.monitored ? "MON" : d.locked ? "LOCK" : "--");
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
                {
                    ImVec4 berCol = d.egcBer < 0  ? ImVec4(0.5f, 0.5f, 0.5f, 1.0f)
                                  : d.egcBer <= 10 ? ImVec4(0.2f, 1.0f, 0.3f, 1.0f)
                                  : d.egcBer <= 50 ? ImVec4(1.0f, 0.85f, 0.2f, 1.0f)
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
                             : d.ebno < hiGreen ? ImVec4(1.0f, 0.85f, 0.2f, 1.0f)
                                                : ImVec4(0.2f, 1.0f, 0.3f, 1.0f);
                ImGui::TextColored(ebCol, "%.1f", d.ebno);
            }
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

void drawSUs(App& app)
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

void drawMessages(App& app)
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
            ImGui::TextWrapped("%s", it->text.c_str());
            if (it->hasPos)
                ImGui::TextColored(ImVec4(0.3f, 0.9f, 1.0f, 1.0f),
                                   "POS %.4f, %.4f  %d ft", it->lat, it->lon, it->alt);
            if (!it->decoded.empty())
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 1.0f, 0.6f, 1.0f));
                ImGui::TextWrapped("%s", it->decoded.c_str());
                ImGui::PopStyleColor();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::End();
}

void drawAircraft(App& app)
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
            if (!a.icao.empty())
            {
                uint32_t ihex = (uint32_t)std::strtoul(a.icao.c_str(), nullptr, 16);
                const char* cc = icaoCountry(ihex);
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
        }
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
                    tuneToVoice(app, it.rxMHz, it.aesId);
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

void drawNetwork(App& app)
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

void drawFlightMap(App& app)
{
    ImGui::Begin("Flight Map");

    auto acs = app.decoders.aircraftTable().snapshot();
    std::sort(acs.begin(), acs.end(),
              [](const AircraftEntry& a, const AircraftEntry& b) { return a.lastSeen > b.lastSeen; });
    const AircraftEntry* pick = nullptr;

    // Prefer the ICAO of the voice call the user is currently listening to.
    uint32_t monitoredAes = app.decoders.voiceAes();
    if (monitoredAes) {
        std::string monitoredIcao = app.decoders.aircraftTable().icao(monitoredAes);
        if (!monitoredIcao.empty()) {
            for (auto& a : acs) {
                if (a.aesId == monitoredAes) { pick = &a; break; }
            }
        }
    }
    if (!pick) {
        for (auto& a : acs)
            if (!a.icao.empty()) { pick = &a; break; }
    }

    if (pick && !app.flightMapWv.isReady())
    {
        ImGui::Text("%s  %s  %06X",
                    pick->icao.c_str(),
                    pick->flight.empty() ? pick->reg.c_str() : pick->flight.c_str(),
                    pick->aesId);
        if (pick->hasPos)
            ImGui::SameLine(); ImGui::Text("  %.4f,%.4f  %d ft", pick->lat, pick->lon, pick->alt);
    }
    else if (!pick && !app.flightMapWv.isReady())
    {
        ImGui::TextDisabled("No aircraft with ICAO yet.");
    }

#if defined(_WIN32)
    if (!app.flightMapWv.isReady())
    {
        ImGui::TextDisabled("  Loading map...");
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
    app.flightMapWv.setBounds((int)pos.x, (int)pos.y, w, h, tabActive);

    static std::string lastIcao;
    if (pick && pick->icao != lastIcao)
    {
        lastIcao = pick->icao;
        app.flightMapWv.setIcao(pick->icao);
    }
#else
    ImGui::TextDisabled("WebView2 map requires Windows.");
#endif

    ImGui::End();
}

void drawEgc(App& app)
{
    ImGui::Begin("EGC");

    ImGui::Text("%llu message(s)", (unsigned long long)app.decoders.egcLog().count());
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear"))
        app.decoders.egcLog().clear();
    ImGui::SameLine();
    static bool showEgc = true, showTerminal = true;
    ImGui::Checkbox("EGC", &showEgc); ImGui::SameLine();
    ImGui::Checkbox("STDC", &showTerminal);
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
            bool isTerminal = (it->priority == "Terminal");
            if (isTerminal && !showTerminal) continue;
            if (!isTerminal && !showEgc) continue;
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

void drawMes(App& app)
{
    ImGui::Begin("MES");

    auto entries = app.decoders.mesLog().snapshot();
    ImGui::Text("%zu terminal(s)", entries.size());
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear"))
        app.decoders.mesLog().clear();
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
        }
        ImGui::EndTable();
    }

    ImGui::End();
}

void drawLes(App& app)
{
    ImGui::Begin("LES");

    ImGui::Text("%llu message(s)", (unsigned long long)app.decoders.lesLog().count());
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear"))
        app.decoders.lesLog().clear();
    ImGui::TextDisabled("LES private ship/shore messages (0xAA non-EGC).");
    ImGui::Separator();

    auto msgs = app.decoders.lesLog().snapshot();
    if (ImGui::BeginTable("##les", 6,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
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

        for (auto it = msgs.rbegin(); it != msgs.rend(); ++it)
        {
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
            ImGui::TextWrapped("%s", it->text.c_str());
        }
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

void drawDockHost(App& app)
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
        ImGui::DockBuilderDockWindow("Flight Map", rmid);
        ImGui::DockBuilderDockWindow("SUs", rbot);
        ImGui::DockBuilderDockWindow("Messages", rbot);
        ImGui::DockBuilderDockWindow("C-Channel", rbot);
        ImGui::DockBuilderDockWindow("Network", rbot);
        ImGui::DockBuilderDockWindow("EGC", rbot);
        ImGui::DockBuilderDockWindow("MES", rbot);
        ImGui::DockBuilderDockWindow("LES", rbot);
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

