#include "imgui.h"
#include "implot.h"
#include "core/main_funcs.h"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>

bool openWavDialog(char*, int) { return false; }
class TestSource : public SdrSource {
public:
    double frequency = 100e6, rate = 2e6;
    bool on = false;
    std::vector<SdrDeviceInfo> listDevices() override { return {}; }
    void setCenterFreq(double hz) override { frequency = hz; }
    void setSampleRate(double hz) override { rate = hz; }
    void setGain(double) override {}
    void setBiasTee(bool) override {}
    void setPpm(double) override {}
    double centerFreq() const override { return frequency; }
    double sampleRate() const override { return rate; }
    bool start(int, SdrSampleCb, std::string&) override { return on = true; }
    void stop() override { on = false; }
    bool running() const override { return on; }
};
static void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
static void frame(App& app) {
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(900, 300));
    drawSpectrum(app, app.viewA, app.decoders, "Spectrum test", false, false);
    ImGui::Render();
}
static void wav(const std::filesystem::path& path, unsigned rate) {
    std::ofstream out(path, std::ios::binary);
    auto u16 = [&](unsigned v) { out.put(char(v)); out.put(char(v >> 8)); };
    auto u32 = [&](unsigned v) { u16(v); u16(v >> 16); };
    out.write("RIFF", 4); u32(36 + 4096); out.write("WAVEfmt ", 8);
    u32(16); u16(1); u16(2); u32(rate); u32(rate * 4); u16(4); u16(16);
    out.write("data", 4); u32(4096);
    for (int i = 0; i < 2048; ++i) u16(0);
}
int main(int argc, char** argv) {
    ImGui::CreateContext(); ImPlot::CreateContext();
    auto& io = ImGui::GetIO(); io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1000, 700); io.DeltaTime = 1.0f / 60;
    unsigned char* pixels; int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    io.Fonts->SetTexID(1);
    const auto path = std::filesystem::temp_directory_path() / ("inmarscope-spectrum-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".wav");
    int result = 0;
    try {
        TestSource receiverA, receiverB;
        auto app = std::make_unique<App>();
        app->iqBufferSec = 0;
        app->sourceMode = 1; app->wavLoop = true; app->saveDecoders = false;
        app->fftSizeIdx = 0;
        std::snprintf(app->wavPath, sizeof(app->wavPath), "%s", path.string().c_str());
        require(argc == 2, "band-plan directory argument required");
        std::vector<std::string> names, paths;
        scanBandPlans(argv[1], names, paths);
        require(!paths.empty(), "no band plans found");
        // Seed the previous session with a narrow, unrelated frequency span.
        buildWindow(app->viewA, 1024, app->dbMin);
        updateFreqAxis(app->viewA, 100e6, 62500, 1024);
        frame(*app);
        app->centerFreqMHz = 1545;
        wav(path, 2000000);
        startActive(*app);
        require(app->active->running(), "WAV source did not start");
        // Match main.cpp: Start is clicked AFTER processFft, before drawSpectrum.
        // No FFT processing is allowed before this first plot frame.
        app->showBandPlan = true;
        app->bandPlanLoaded = loadBandPlan(paths.front());
        frame(*app);
        app->active->stop(); app->decoders.stop();
        std::printf("First start frame: %.6f .. %.6f MHz\n", app->viewA.viewXminMHz, app->viewA.viewXmaxMHz);
        require(std::abs(app->viewA.viewXminMHz - 1544) < .001,
                "startup used the previous session's frequency range");
        require(std::abs(app->viewA.viewXmaxMHz - (1546 - 2.0/1024)) < .001,
                "startup did not show the full new sample bandwidth");
        // All catalogue plans must leave the RF view unchanged over repeated frames.
        const double lo = app->viewA.viewXminMHz, hi = app->viewA.viewXmaxMHz;
        for (const auto& plan : paths) {
            app->bandPlanLoaded = loadBandPlan(plan);
            for (int i = 0; i < 3; ++i) frame(*app);
            require(std::abs(app->viewA.viewXminMHz-lo) < 1e-8 && std::abs(app->viewA.viewXmaxMHz-hi) < 1e-8,
                    "band plan changed the spectrum zoom");
        }
        app->active->stop(); app->decoders.stop();
        const auto apac = loadBandPlan((std::filesystem::path(argv[1]) / "satellite/inmarsat-4f2.json").string());
        const auto group = bandPlanGroups(apac, 2e6).front();
        app->sourceMode = 7; app->active = &receiverA; app->activeB = &receiverB;
        tuneBandPlan(*app, false, group.centerMHz);
        require(app->centerFreqMHz == group.centerMHz && receiverA.frequency == 100e6,
                "stopped selection should save the tuning without touching a device");
        receiverA.on = receiverB.on = true;
        tuneBandPlan(*app, false, group.centerMHz);
        frame(*app);
        require(std::abs(receiverA.frequency / 1e6 - group.centerMHz) < 1e-8 && receiverB.frequency == 100e6,
                "receiver A tuning leaked to B");
        require(app->viewA.viewXmaxMHz-app->viewA.viewXminMHz > 1.99,
                "channel markers narrowed the view below the sample bandwidth");
        tuneBandPlan(*app, true, 1541.49);
        require(receiverB.frequency == 1541.49e6 && std::abs(receiverA.frequency / 1e6-group.centerMHz) < 1e-8,
                "receiver B tuning leaked to A");
        require(app->viewB.resetView && app->viewB.freqMHz.front() > 1540,
                "receiver B did not publish the new range before drawing");
        app->sourceMode = 1;
        tuneBandPlan(*app, false, 500);
        require(std::abs(receiverA.frequency / 1e6-group.centerMHz) < 1e-8, "WAV tuning must be blocked");
        receiverA.stop(); receiverB.stop();
        std::printf("PASS: startup, %zu overlays, full-bandwidth tuning, A/B isolation and fixed WAV frequency\n", paths.size());
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what()); result = 1;
    }
    std::filesystem::remove(path);
    ImPlot::DestroyContext(); ImGui::DestroyContext();
    return result;
}
