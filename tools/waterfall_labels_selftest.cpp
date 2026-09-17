#include "gui/waterfall_labels.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>

static void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
static void bounds(const WaterfallLabels& labels, ImVec2 size) {
    for (const auto& item : labels.items) {
        require(item.x0 >= -.01f && item.x1 <= size.x+.01f, "marker outside viewport");
        if (item.row < 0) continue;
        require(item.labelMin.x >= 0 && item.labelMax.x <= size.x &&
                item.labelMin.y >= 0 && item.labelMax.y <= size.y, "label outside viewport");
        for (const auto& other : labels.items) {
            if (&item == &other || other.row != item.row) continue;
            require(item.labelMin.x >= other.labelMax.x || item.labelMax.x <= other.labelMin.x,
                    "overlapping labels");
        }
    }
}
int main(int argc, char** argv) {
    ImGui::CreateContext();
    auto& io = ImGui::GetIO(); io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1600, 1000); io.DeltaTime = 1.f/60;
    unsigned char* pixels; int w, h;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h); io.Fonts->SetTexID(1);
    int result = 0;
    try {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0,0));
        ImGui::SetNextWindowSize(ImVec2(1600,1000));
        ImGui::Begin("Waterfall label test");
        BandPlan plan; plan.valid = true; plan.name = "Test satellite";
        plan.entries = {{1544,1544.001,"Aero",0xffffffff,1544,"Aero data",1200},
                        {1545,1545.001,"Aero",0xffffffff,1545,"Aero data",1200},
                        {1546,1546.001,"Aero",0xffffffff,1546,"Aero data",1200}};
        const ImVec2 size(1000,400);
        auto labels = layoutWaterfallLabels(&plan, {}, 1544,1546,1543,1547,size);
        require(labels.items.size() == 3, "missing endpoint channel markers");
        require(labels.items[0].x0 == 0 && labels.items[1].x0 == 500 && labels.items[2].x0 == 1000,
                "incorrect frequency projection");
        bounds(labels,size);
        labels = layoutWaterfallLabels(&plan, {}, 1544.5,1545.5,1543,1547,size);
        require(labels.items.size() == 1 && labels.items[0].x0 == 500, "zoom filtering/anchor failed");
        labels = layoutWaterfallLabels(&plan, {}, 1544.75,1545.75,1543,1547,size);
        require(labels.items.size() == 1 && labels.items[0].x0 == 250, "pan anchor failed");
        labels = layoutWaterfallLabels(&plan, {}, 1544,1546,1544.5,1545.5,size);
        require(labels.items.size() == 1 && labels.items[0].x0 == 500, "capture clipping failed");
        labels = layoutWaterfallLabels(&plan, {{1545,42,1200,true}},1544,1546,1543,1547,size);
        require(labels.items.size() == 3 && labels.items[0].active && labels.items[0].row == 0 &&
                labels.items[0].text.find("CH 42") == 0 && labels.items[0].detail.find("Locked") != std::string::npos,
                "active channel merge/priority failed");
        // Distinct frequencies or modes must never merge merely because their pixels coincide.
        labels = layoutWaterfallLabels(&plan, {{1545.000001,42,1200,true},{1545,43,600,false}},0,3000,0,3000,size);
        require(labels.items.size() == 5, "unrelated channels merged");
        BandPlan allocation; allocation.valid = true; allocation.name = "National";
        allocation.entries = {{1500,1550,"Mobile satellite allocation",0xffffffff}};
        labels = layoutWaterfallLabels(&allocation, {},1544,1546,1544.5,1545.5,size);
        require(labels.items.size() == 1 && labels.items[0].x0 == 250 && labels.items[0].x1 == 750,
                "allocation range clipping failed");
        require(layoutWaterfallLabels(&plan,{},0,0,0,1,size).items.empty(), "zero span accepted");
        require(layoutWaterfallLabels(&plan,{},NAN,1,0,1,size).items.empty(), "NaN view accepted");
        require(layoutWaterfallLabels(&plan,{},0,1,2,3,size).items.empty(), "disjoint capture accepted");
        std::vector<WaterfallChannel> crowded;
        for (int i=0; i<100; ++i) crowded.push_back({1545+i*.00001,i,1200,false});
        labels = layoutWaterfallLabels(nullptr,crowded,1544,1546,1544,1546,size);
        require(labels.items.size() == 100 && std::count_if(labels.items.begin(),labels.items.end(),
            [](const auto& item) { return item.row >= 0; }) <= 3, "crowded label suppression failed");
        bounds(labels,size);
        require(argc == 2, "band plan directory required");
        std::vector<std::string> names, paths; scanBandPlans(argv[1],names,paths);
        require(paths.size() == 27, "expected all 27 plans");
        int layouts = 0;
        for (const auto& path : paths) {
            auto bundled = loadBandPlan(path); require(bundled.valid,"invalid bundled plan");
            double low = std::numeric_limits<double>::max(), high = 0;
            for (const auto& entry : bundled.entries) {
                low = std::min(low,entry.loMHz); high = std::max(high,entry.hiMHz);
            }
            auto full = layoutWaterfallLabels(&bundled,{},low,high,low,high,size);
            require(full.items.size() == bundled.entries.size(),"bundled plan entries missing");
            bounds(full,size);
            for (float scale : {1.f,1.5f,2.f}) {
                ImGui::SetWindowFontScale(scale);
                for (const auto viewport : {ImVec2(5,5),ImVec2(40,40),ImVec2(240,160),ImVec2(1000,400),ImVec2(1600,800)}) {
                    for (double span : {.0625, .2, 2., 10., 2000.}) {
                        auto overlay = layoutWaterfallLabels(&bundled,crowded,1545-span/2,1545+span/2,
                                                           1540,1550,viewport);
                        bounds(overlay,viewport);
                        drawWaterfallLabels(overlay,ImVec2(10,10),viewport);
                        ++layouts;
                    }
                }
            }
        }
        ImGui::SetWindowFontScale(1);
        ImGui::End(); ImGui::Render();
        require(ImGui::GetDrawData()->TotalVtxCount > 0,"no annotations rendered");
        std::printf("Waterfall labels PASS: %d layouts across 27 plans; zoom, pan, capture clipping, density and UI scaling\n",layouts);
    } catch (const std::exception& error) {
        std::fprintf(stderr,"Waterfall labels FAIL: %s\n",error.what()); result=1;
    }
    ImGui::DestroyContext(); return result;
}
