#include "core/app.h"
#include "core/main_funcs.h"
#include "imgui.h"
#include <algorithm>
#include <cstdio>

namespace {
bool stringCombo(const char* label, std::string& value, const std::vector<std::string>& options,
                 const std::vector<std::string>& labels = {}) {
    bool changed = false;
    if (ImGui::BeginCombo(label, value.empty() ? "Device default" : value.c_str())) {
        for (size_t i = 0; i < options.size(); ++i)
            if (ImGui::Selectable((i < labels.size() ? labels[i] : options[i]).c_str(), value == options[i])) {
                value = options[i]; changed = true;
            }
        ImGui::EndCombo();
    }
    return changed;
}
void rateCombo(const char* label, double& value, const std::vector<double>& values, bool automatic = false) {
    char preview[64]; std::snprintf(preview, sizeof(preview), "%.6g MHz", value / 1e6);
    if (ImGui::BeginCombo(label, value == 0 ? "Automatic" : preview)) {
        if (automatic && ImGui::Selectable("Automatic", value == 0)) value = 0;
        for (double rate : values) {
            char text[64]; std::snprintf(text, sizeof(text), "%.6g MHz", rate / 1e6);
            if (ImGui::Selectable(text, value == rate)) value = rate;
        }
        ImGui::EndCombo();
    }
}
void receiver(App& app, SdrplaySource& source, RspConfig& cfg, bool second) {
    ImGui::PushID(second ? "rspB" : "rspA");
    ImGui::SeparatorText(second ? "Receiver B" : "Receiver A");
    bool running = app.rsp.running() || app.rspB.running();
    bool slave = second && app.rspSecond == 2;
    ImGui::BeginDisabled(running || slave);
    if (ImGui::BeginCombo("Device serial", cfg.serial.empty() ? "Select device" : cfg.serial.c_str())) {
        for (const auto& d : app.rspDevices) {
            if (ImGui::Selectable((d.name + " [" + d.serial + "]").c_str(), cfg.serial == d.serial)) {
                app.rspB.close(); app.rsp.close();
                cfg = RspConfig{}; cfg.serial = d.serial;
            }
        }
        ImGui::EndCombo();
    }
    const auto selected = std::find_if(app.rspDevices.begin(), app.rspDevices.end(),
        [&](const SdrDeviceInfo& d) { return d.serial == cfg.serial; });
    if (app.rspSecond != 2 && selected != app.rspDevices.end() && selected->hasTunerModes) {
        if (stringCombo("RSPduo mode", cfg.mode, {"ST", "MA", "MA8", "SL"},
                        {"Single tuner", "Master (6 MHz clock)", "Master (8 MHz clock)", "Slave"})) {
            app.rspB.close(); app.rsp.close(); cfg.antenna.clear();
        }
    }
    if (ImGui::Button("Load device controls")) {
        app.rspB.close(); app.rsp.close();
        if (!second && app.rspSecond == 2) cfg.mode = "MA";
        std::string err;
        source.setCenterFreq((second ? app.centerFreqMHzB : app.centerFreqMHz) * 1e6);
        if (!source.prepare(cfg, err)) app.status = err;
    }
    ImGui::EndDisabled();
    if (slave) ImGui::TextWrapped("Tuner B opens automatically after tuner A starts. Sample rate and PPM share A's clock. B's controls appear after the first start.");

    double& frequency = second ? app.centerFreqMHzB : app.centerFreqMHz;
    if (ImGui::InputDouble("Center (MHz)", &frequency, 0.1, 1.0, "%.6f")) {
        frequency = std::clamp(frequency, 0.001, 2000.0);
        if (running) {
            if (!second) retunePreserving(app, frequency);
            else {
                source.setCenterFreq(frequency * 1e6); frequency = source.centerFreq() / 1e6;
                app.decodersB.configure(source.sampleRate(), source.centerFreq());
            }
        }
        (second ? app.viewB : app.viewA).resetView = true;
    }
    if (!source.prepared()) {
        ImGui::TextWrapped("Load device controls to select antennas, gain and model-specific features. Settings are saved separately for A and B.");
    } else {
        ImGui::BeginDisabled(running);
        stringCombo("Antenna / input", cfg.antenna, source.antennas());
        ImGui::BeginDisabled(slave);
        rateCombo("Sample rate", cfg.rate, source.rates());
        ImGui::EndDisabled();
        rateCombo("IF bandwidth", cfg.bandwidth, source.bandwidths(), true);
        if (source.hasPpm && !slave) {
            ImGui::InputDouble("Frequency correction (PPM)", &cfg.ppm, 0.1, 1.0, "%.2f");
            cfg.ppm = std::clamp(cfg.ppm, -200.0, 200.0);
        }
        if (source.hasAgc) ImGui::Checkbox("Automatic gain", &cfg.agc);
        if (source.hasDc) ImGui::Checkbox("DC correction", &cfg.dc);
        if (source.hasIq) ImGui::Checkbox("IQ balance correction", &cfg.iq);
        for (const auto& gain : source.gains()) {
            double v = cfg.gains.count(gain.key) ? cfg.gains.at(gain.key) : std::atof(gain.value.c_str());
            ImGui::BeginDisabled(cfg.agc && gain.key == "IFGR");
            const std::string label = gain.key == "RFGR" ? "RF gain reduction (LNA state)" : gain.name + " gain (driver units)";
            if (ImGui::SliderScalar(label.c_str(), ImGuiDataType_Double, &v, &gain.minimum, &gain.maximum, "%.0f"))
                cfg.gains[gain.key] = v;
            ImGui::EndDisabled();
        }
        if (ImGui::CollapsingHeader("Model-specific controls", ImGuiTreeNodeFlags_DefaultOpen)) {
            for (const auto& c : source.controls()) {
                const auto key = (c.channel ? "channel:" : "device:") + c.key;
                auto value = cfg.settings.count(key) ? cfg.settings.at(key) : c.value;
                bool changed = false;
                ImGui::PushID(key.c_str());
                const char* label = c.name.empty() ? c.key.c_str() : c.name.c_str();
                if (!c.options.empty()) changed = stringCombo(label, value, c.options, c.labels);
                else if (c.type == 0) {
                    bool v = value == "true"; changed = ImGui::Checkbox(label, &v); value = v ? "true" : "false";
                } else {
                    char text[256]; std::snprintf(text, sizeof(text), "%s", value.c_str());
                    changed = ImGui::InputText(label, text, sizeof(text)); value = text;
                }
                if (ImGui::IsItemHovered() && !c.description.empty()) ImGui::SetTooltip("%s", c.description.c_str());
                if (changed) cfg.settings[key] = value;
                ImGui::PopID();
            }
        }
        ImGui::EndDisabled();
        ImGui::TextDisabled("Hardware settings apply on Start; stop to edit them.");
    }
    auto error = source.error();
    if (!error.empty()) ImGui::TextWrapped("%s", error.c_str());
    if (running) ImGui::Text("Actual rate: %.6g MHz | Overflows: %llu", source.sampleRate() / 1e6,
                             (unsigned long long)source.overflows());
    ImGui::PopID();
}
}
void drawSdrplayControls(App& app) {
#ifndef HAS_SOAPYSDR
    ImGui::TextWrapped("SDRplay requires a build with SoapySDR, the SoapySDRPlay3 module and the SDRplay API service. See COMPILE.md.");
#endif
    ImGui::BeginDisabled(app.rsp.running() || app.rspB.running());
    const char* paths[] = {"One receiver", "Two SDRplay devices", "RSPduo two independent tuners"};
    if (ImGui::Combo("Receive paths", &app.rspSecond, paths, 3)) {
        app.rspB.close(); app.rsp.close();
        app.rspConfig.mode = app.rspSecond == 2 ? "MA" : "ST";
        app.rspConfigB.mode = "ST";
        app.rspConfig.antenna.clear(); app.rspConfigB.antenna.clear();
    }
    if (ImGui::Button("Find SDRplay devices")) {
        app.rspB.close(); app.rsp.close();
        app.rspDevices = app.rsp.listDevices();
    }
    ImGui::EndDisabled();
    receiver(app, app.rsp, app.rspConfig, false);
    if (app.rspSecond) receiver(app, app.rspB, app.rspConfigB, true);
    ImGui::TextWrapped("Detects RSP1, RSP1A, RSP1B, RSP2 / RSP2pro, RSPduo, RSPdx and RSPdx-R2 through SDRplay API 3.15 or newer. IQ recording captures receiver A only.");
}
