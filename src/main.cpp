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

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    App app;

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
    {
        RecordFormat rf = (app.recordFormat == 1) ? RecordFormat::OGG : RecordFormat::WAV;
        app.decoders.setRecordFormat(rf);
        app.decodersB.setRecordFormat(rf);
    }
    app.verCheck.start("inmarscope", INMARSCOPE_VERSION);
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

        app.decoders.autoMonitor();
        if (app.dualMode)
            app.decodersB.autoMonitor();

        updateFeed(app);

        drawControls(app);
        drawSpectrum(app, app.viewA, app.decoders, "Spectrum", true, false);
        drawWaterfall(app, app.viewA, "Waterfall");
        if (app.dualMode)
        {
            drawSpectrum(app, app.viewB, app.decodersB, "Spectrum (Voice)", false, true);
            drawWaterfall(app, app.viewB, "Waterfall (Voice)");
        }
        drawDecoders(app);
        drawSUs(app);
        drawMessages(app);
        drawCChannel(app);
        drawNetwork(app);
        drawEgc(app);
        drawAircraft(app);
        drawFlightMap(app);
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

    // Persist settings + dock layout to inmarscope.ini before shutting down.
    ImGui::SaveIniSettingsToDisk(io.IniFilename);

    app.decoders.stop();
    app.decodersB.stop();
    app.sdr.stop();
    app.sdrB.stop();
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
