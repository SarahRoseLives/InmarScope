// InmarScope - Inmarsat decoder
// Phase 1: RTL-SDR -> IQ ring -> FFT -> spectrum + scrolling waterfall.

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"

#include <GLFW/glfw3.h>
#if defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

#include "core/app.h"
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
#include <commdlg.h>
#include <shellapi.h>

bool openWavDialog(char* out, int outLen)
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
bool openWavDialog(char*, int) { return false; }
#endif

static void glfw_error_callback(int error, const char* description)
{
    std::fprintf(stderr, "GLFW error %d: %s\n", error, description);
}


#include "core/app.h"
#include "core/main_funcs.h"

int main(int, char**)
{
#if defined(_WIN32)
    // WebView2 requires STA — init before GLFW so the UI thread IS the STA thread.
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
#endif
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
    App app;

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

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
    // Message store: wire to both decoder managers (but enabled_ starts false).
    app.decoders.setMessageStore(&app.writeDb);
    app.decodersB.setMessageStore(&app.writeDb);
    // Cleanup old archive databases.
    app.writeDb.cleanup("databases", app.maxDbAgeDays);
    {
        RecordFormat rf = (app.recordFormat == 1) ? RecordFormat::OGG : RecordFormat::WAV;
        app.decoders.setRecordFormat(rf);
        app.decodersB.setRecordFormat(rf);
    }
    app.verCheck.start("inmarscope", INMARSCOPE_VERSION);
    scanBandPlans(app.bandPlanDir, app.bandPlanNames, app.bandPlanPaths);
    if (app.bandPlanIdx >= 0 && app.bandPlanIdx < (int)app.bandPlanPaths.size())
        app.bandPlanLoaded = loadBandPlan(app.bandPlanPaths[app.bandPlanIdx]);
    if (app.bandPlanIdxB >= 0 && app.bandPlanIdxB < (int)app.bandPlanPaths.size())
        app.bandPlanLoadedB = loadBandPlan(app.bandPlanPaths[app.bandPlanIdxB]);
    app.decoders.voiceCallLog().scanDir(app.recordDir);
    // Start web server if previously enabled
    if (app.webServerEnabled)
    {
        app.webServer.decodersA = &app.decoders;
        app.webServer.decodersB = &app.decodersB;
        app.webServer.dualMode = &app.dualMode;
        app.webServer.active = &app.active;
        app.webServer.start(app.webServerPort);
    }
#if defined(_WIN32)
    app.flightMapWv.init(glfwGetWin32Window(window));
#endif

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
            updateCallHunter(app);

        if (app.active->running())
            updateRateChange(app);

        // Log-to-DB toggle: start/stop per-session database.
        if (app.logToDb != app.writeDb.enabled())
        {
            if (app.logToDb)
                app.writeDb.openSession("databases");
            else
                app.writeDb.setEnabled(false);
        }

        app.decoders.autoMonitor(app.blacklistCountries);
        if (app.dualMode)
            app.decodersB.autoMonitor(app.blacklistCountries);

        // Refresh saved decoder list for persistent restart (non-8400 only)
        if (app.saveDecoders && app.active->running())
        {
            app.savedDecoders.clear();
            for (auto& st : app.decoders.status())
                if (st.baud != 8400)
                    app.savedDecoders.push_back({st.freqMHz, st.baud});
            app.savedDecodersB.clear();
            for (auto& st : app.decodersB.status())
                if (st.baud != 8400)
                    app.savedDecodersB.push_back({st.freqMHz, st.baud});
        }

        updateFeed(app);

        drawControls(app);
        drawSpectrum(app, app.viewA, app.decoders, "Spectrum", true, false);
        drawWaterfall(app, app.viewA, "Waterfall");
        if (app.dualMode)
        {
            drawSpectrum(app, app.viewB, app.decodersB, "Spectrum (B)", true, true);
            drawWaterfall(app, app.viewB, "Waterfall (B)");
        }
        drawDecoders(app);
        drawSUs(app);
        drawMessages(app);
        drawCChannel(app);
        drawNetwork(app);
        drawEgc(app);
        drawMes(app);
        drawLes(app);
        drawAircraft(app);
        drawVoiceCalls(app);
        drawLesFreq(app);
        drawFlightMap(app);
        drawConstellation(app);
        drawAbout(app);

        // Auto-mute live audio during playback, restore after
        static bool wasPlaying = false;
        bool isPlaying = app.audioPlayer.isPlaying();
        if (isPlaying && !wasPlaying)
        {
            app.decoders.setVoiceMute(true);
            app.decodersB.setVoiceMute(true);
        }
        else if (!isPlaying && wasPlaying)
        {
            app.decoders.setVoiceMute(app.voiceMuted);
            app.decodersB.setVoiceMute(app.voiceMuted);
        }
        wasPlaying = isPlaying;

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
    app.webServer.stop();

    app.writeDb.closeCurrent();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
