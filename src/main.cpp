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
#include "i18n/i18n.h"
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
#include <cstdio>
#include <cstring>

// Native file picker via the desktop's dialog helper (zenity / kdialog / qarma).
// These ship with GNOME/KDE and most distros; avoids adding a GUI toolkit dep.
bool openWavDialog(char* out, int outLen)
{
    const char* cmds[] = {
        "zenity --file-selection "
            "--file-filter='WAV / IQ files | *.wav *.WAV' "
            "--file-filter='All files | *' 2>/dev/null",
        "qarma --file-selection "
            "--file-filter='WAV / IQ files | *.wav *.WAV' "
            "--file-filter='All files | *' 2>/dev/null",
        "kdialog --getopenfilename . "
            "'WAV / IQ files (*.wav *.WAV)|All files (*)' 2>/dev/null",
    };
    for (const char* cmd : cmds)
    {
        FILE* p = popen(cmd, "r");
        if (!p)
            continue;
        char buf[1024] = "";
        char* got = std::fgets(buf, sizeof(buf), p);
        int rc = pclose(p);
        // 127 == shell couldn't find the helper; try the next one.
        if (rc == 127 || !got)
            continue;
        // Strip trailing newline.
        size_t n = std::strlen(buf);
        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
            buf[--n] = 0;
        if (n == 0)
            return false; // helper ran, user cancelled
        std::strncpy(out, buf, outLen - 1);
        out[outLen - 1] = 0;
        return true;
    }
    return false;
}
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

#if defined(_WIN32)
	using RegisterTouchpadCapableThreadFn = BOOL (WINAPI *)(BOOL);

	HMODULE user32 = GetModuleHandleW(L"user32.dll");
	auto pRegisterTouchpadCapableThread =
		reinterpret_cast<RegisterTouchpadCapableThreadFn>(
			GetProcAddress(user32, "RegisterTouchpadCapableThread"));

	if (pRegisterTouchpadCapableThread)
	{
		if (!pRegisterTouchpadCapableThread(TRUE))
		{
			fprintf(stderr,
				"[WIN32] RegisterTouchpadCapableThread failed: %lu\n",
				GetLastError());
		}

	}
	else
	{
		fprintf(stderr,
			"[WIN32] RegisterTouchpadCapableThread not available\n");
	}

	// Register keyboard Raw Input to distinguish physical Ctrl
	// from the Ctrl synthesized by Windows for touchpad pinch gestures.
	HWND hWnd = glfwGetWin32Window(window);

	RAWINPUTDEVICE keyboardRid = {};
	keyboardRid.usUsagePage = 0x01;   // Generic Desktop Controls
	keyboardRid.usUsage     = 0x06;   // Keyboard
	keyboardRid.dwFlags     = RIDEV_INPUTSINK;
	keyboardRid.hwndTarget  = hWnd;

	if (!RegisterRawInputDevices(&keyboardRid, 1, sizeof(keyboardRid)))
	{
		fprintf(stderr,
			"[WIN32] Raw keyboard registration failed: %lu\n",
			GetLastError());
	}
#endif

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    App app;

    // Load persisted settings BEFORE building the font atlas so fontSize
    // from inmarscope.ini is available for the font loader.
    cfgRegisterHandler(app);
    io.IniFilename = "inmarscope.ini";
    ImGui::LoadIniSettingsFromDisk(io.IniFilename);

    // Apply persisted theme.
    if (app.lightMode)
    {
        ImGui::StyleColorsLight();
        // Darken light theme for readability — default StyleColorsLight() is
        // too low-contrast on most monitors.
        ImGuiStyle& st = ImGui::GetStyle();
        st.Colors[ImGuiCol_Text]                  = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
        st.Colors[ImGuiCol_TextDisabled]           = ImVec4(0.36f, 0.36f, 0.36f, 1.00f);
        st.Colors[ImGuiCol_WindowBg]               = ImVec4(0.94f, 0.94f, 0.94f, 1.00f);
        st.Colors[ImGuiCol_TableHeaderBg]          = ImVec4(0.73f, 0.73f, 0.73f, 1.00f);
        st.Colors[ImGuiCol_TableRowBg]             = ImVec4(0.97f, 0.97f, 0.97f, 1.00f);
        st.Colors[ImGuiCol_TableRowBgAlt]          = ImVec4(0.88f, 0.88f, 0.88f, 1.00f);
        // Frame backgrounds (combos, inputs) need to stay distinguishable.
        st.Colors[ImGuiCol_FrameBg]                = ImVec4(0.85f, 0.85f, 0.85f, 1.00f);
    }
    else
        ImGui::StyleColorsDark();

    // Language and font size.  Both require a restart to take full effect
    // for the font atlas, but strings pass through _L() immediately.
    i18nInit();
    i18nSet((Lang)app.languageIdx);

    // Font: use Roboto-Medium (vendored TTF, scales cleanly at any size).
    // Clamp to sensible range and scale the style sizes proportionally.
    if (app.fontSize < 8)  app.fontSize = 8;
    if (app.fontSize > 24) app.fontSize = 24;
    {
        const char* fontPaths[] = {
            "third_party/imgui/misc/fonts/Roboto-Medium.ttf",
            "../third_party/imgui/misc/fonts/Roboto-Medium.ttf",
        };
        bool loaded = false;
        for (auto& fp : fontPaths)
        {
            FILE* f = std::fopen(fp, "rb");
            if (f) { std::fclose(f); loaded = (io.Fonts->AddFontFromFileTTF(fp, (float)app.fontSize) != nullptr); break; }
        }
        if (!loaded)
            io.Fonts->AddFontDefault(); // fallback to built-in ProggyClean
    }
    ImGui::GetStyle().ScaleAllSizes((float)app.fontSize / 13.0f);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

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
        {
            std::string t = std::string(_L("Spectrum")) + "###Spectrum";
            drawSpectrum(app, app.viewA, app.decoders, t.c_str(), true, false);
        }
        {
            std::string t = std::string(_L("Waterfall")) + "###Waterfall";
            drawWaterfall(app, app.viewA, t.c_str());
        }
        if (app.dualMode)
        {
            {
                std::string t = std::string(_L("Spectrum")) + "###Spectrum (B)";
                drawSpectrum(app, app.viewB, app.decodersB, t.c_str(), true, true);
            }
            {
                std::string t = std::string(_L("Waterfall")) + "###Waterfall (B)";
                drawWaterfall(app, app.viewB, t.c_str());
            }
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
