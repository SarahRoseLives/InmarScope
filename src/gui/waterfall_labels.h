#pragma once
#include "imgui.h"
#include "decode/band_plan.h"
#include <string>
#include <vector>

struct WaterfallChannel {
    double frequencyMHz;
    int id, baud;
    bool locked;
};
struct WaterfallLabel {
    double frequencyMHz = 0;
    int baud = 0;
    float x0 = 0, x1 = 0; // marker/range position relative to waterfall origin
    ImVec2 labelMin{}, labelMax{};
    std::string text, detail;
    ImU32 color = 0;
    bool channel = false, active = false;
    int row = -1; // crowded markers retain their hover details without a label
};
struct WaterfallLabels {
    std::string title, fullTitle;
    float titleHeight = 0, rowHeight = 0, headerHeight = 0;
    std::vector<WaterfallLabel> items;
};

// Display-only projection. Uses the same MHz view and capture bounds as the
// waterfall texture; never changes receiver tuning, plot limits or decoders.
WaterfallLabels layoutWaterfallLabels(const BandPlan* plan,
    const std::vector<WaterfallChannel>& channels, double viewLo, double viewHi,
    double captureLo, double captureHi, ImVec2 size);
void drawWaterfallLabels(const WaterfallLabels&, ImVec2 origin, ImVec2 size);
