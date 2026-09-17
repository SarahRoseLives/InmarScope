#include "gui/waterfall_labels.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace {
std::string oneLine(std::string text) {
    for (char& c : text) if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    return text;
}
std::string fit(std::string text, float width) {
    if (ImGui::CalcTextSize(text.c_str()).x <= width) return text;
    const std::string suffix = "...";
    if (ImGui::CalcTextSize(suffix.c_str()).x > width) return {};
    while (!text.empty() && ImGui::CalcTextSize((text+suffix).c_str()).x > width) {
        size_t end = text.size()-1;
        while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) --end;
        text.resize(end);
    }
    return text+suffix;
}
std::string frequency(double mhz) {
    char text[40]; std::snprintf(text, sizeof(text), "%.6f MHz", mhz); return text;
}
std::string mode(int baud) {
    return baud == 1 ? "EGC" : baud > 0 ? std::to_string(baud)+" baud" : "channel";
}
ImVec2 point(ImVec2 a, ImVec2 b) { return ImVec2(a.x+b.x, a.y+b.y); }
}

WaterfallLabels layoutWaterfallLabels(const BandPlan* plan,
    const std::vector<WaterfallChannel>& channels, double viewLo, double viewHi,
    double captureLo, double captureHi, ImVec2 size)
{
    WaterfallLabels result;
    if (!std::isfinite(viewLo) || !std::isfinite(viewHi) || viewHi <= viewLo ||
        !std::isfinite(captureLo) || !std::isfinite(captureHi) || captureHi <= captureLo ||
        !std::isfinite(size.x) || !std::isfinite(size.y) || size.x <= 4 || size.y <= 4) return result;
    const double visibleLo = std::max(viewLo, captureLo), visibleHi = std::min(viewHi, captureHi);
    if (visibleHi <= visibleLo) return result;
    auto pixel = [&](double mhz) { return float((mhz-viewLo)/(viewHi-viewLo)*size.x); };
    if (plan && plan->valid) {
        result.fullTitle = "Band plan: "+oneLine(plan->name);
        for (const auto& entry : plan->entries) {
            WaterfallLabel item;
            item.channel = entry.frequencyMHz > 0;
            if (item.channel) {
                if (entry.frequencyMHz < visibleLo || entry.frequencyMHz > visibleHi) continue;
                item.x0 = item.x1 = pixel(entry.frequencyMHz);
                item.frequencyMHz = entry.frequencyMHz;
                item.baud = entry.baud;
                item.text = frequency(entry.frequencyMHz)+" / "+mode(entry.baud);
            } else {
                if (entry.hiMHz <= visibleLo || entry.loMHz >= visibleHi) continue;
                item.x0 = pixel(std::max(entry.loMHz, visibleLo));
                item.x1 = pixel(std::min(entry.hiMHz, visibleHi));
                item.text = oneLine(entry.label);
            }
            item.detail = entry.label+"\n"+(item.channel ? frequency(entry.frequencyMHz)+" / "+mode(entry.baud)
                : frequency(entry.loMHz)+" - "+frequency(entry.hiMHz));
            item.color = entry.color;
            result.items.push_back(std::move(item));
        }
    }
    for (const auto& channel : channels) {
        if (!std::isfinite(channel.frequencyMHz) || channel.frequencyMHz < visibleLo || channel.frequencyMHz > visibleHi) continue;
        WaterfallLabel* item = nullptr;
        const float x = pixel(channel.frequencyMHz);
        // Merge active decoders with their plan marker rather than stacking two labels.
        for (auto& candidate : result.items)
            if (candidate.channel && !candidate.active && candidate.baud == channel.baud &&
                std::abs(candidate.frequencyMHz-channel.frequencyMHz) < 1e-7) { item = &candidate; break; }
        if (!item) { result.items.push_back({}); item = &result.items.back(); }
        item->channel = item->active = true; item->x0 = item->x1 = x;
        item->frequencyMHz = channel.frequencyMHz; item->baud = channel.baud;
        item->text = "CH "+std::to_string(channel.id)+"  "+frequency(channel.frequencyMHz)+" / "+mode(channel.baud);
        if (!item->detail.empty()) item->detail += "\n";
        item->detail += item->text+(channel.locked ? "\nLocked" : "\nAcquiring");
        item->color = channel.locked ? IM_COL32(65, 220, 120, 255) : IM_COL32(255, 190, 65, 255);
    }
    if (result.items.empty() && result.fullTitle.empty()) return result;
    if (result.fullTitle.empty()) result.fullTitle = "Decoder channels";
    result.rowHeight = ImGui::GetTextLineHeight()+6;
    result.titleHeight = size.y >= result.rowHeight*2 ? result.rowHeight : 0;
    result.title = fit(result.fullTitle, size.x-8);
    const int rows = std::clamp(int((size.y*.35f-result.titleHeight)/result.rowHeight), 0, 3);
    result.headerHeight = result.titleHeight+rows*result.rowHeight;
    std::stable_sort(result.items.begin(), result.items.end(), [](const auto& a, const auto& b) {
        if (a.active != b.active) return a.active > b.active;
        if (a.channel != b.channel) return a.channel > b.channel;
        return a.x0 < b.x0;
    });
    std::vector<std::vector<ImVec2>> occupied(rows);
    for (auto& item : result.items) {
        const float maxWidth = item.channel ? size.x-8 : std::max(0.0f, item.x1-item.x0-4);
        if (maxWidth < ImGui::GetFontSize()*3) continue;
        item.text = fit(item.text, maxWidth-6);
        if (item.text.empty()) continue;
        const float width = ImGui::CalcTextSize(item.text.c_str()).x+6;
        const float x = std::clamp((item.x0+item.x1-width)*.5f, 2.0f, size.x-width-2);
        for (int row = 0; row < rows; ++row) {
            bool free = true;
            for (const auto& span : occupied[row])
                if (x < span.y+4 && x+width+4 > span.x) { free = false; break; }
            if (!free) continue;
            item.row = row;
            item.labelMin = ImVec2(x, result.titleHeight+row*result.rowHeight);
            item.labelMax = ImVec2(x+width, item.labelMin.y+result.rowHeight-2);
            occupied[row].push_back(ImVec2(x, x+width));
            break;
        }
    }
    return result;
}

void drawWaterfallLabels(const WaterfallLabels& labels, ImVec2 origin, ImVec2 size)
{
    auto* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(origin, point(origin, size), true);
    if (labels.titleHeight > 0 && !labels.title.empty()) {
        draw->AddRectFilled(origin, point(origin, ImVec2(size.x, labels.titleHeight)), IM_COL32(12, 17, 24, 205));
        draw->AddText(point(origin, ImVec2(4, 3)), IM_COL32(235, 240, 245, 255), labels.title.c_str());
    }
    const auto mouse = ImGui::GetMousePos();
    const bool hovered = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(origin, point(origin, size));
    const WaterfallLabel* hover = nullptr;
    float bestDistance = std::numeric_limits<float>::max();
    for (const auto& item : labels.items) {
        const float top = labels.titleHeight;
        if (item.channel) {
            draw->AddLine(point(origin, ImVec2(item.x0, top)), point(origin, ImVec2(item.x0, size.y)), (item.color & 0x00ffffffu) | (45u << 24));
            draw->AddLine(point(origin, ImVec2(item.x0, top)), point(origin, ImVec2(item.x0, std::min(top+6, size.y))), item.color, 2);
        } else {
            draw->AddRectFilled(point(origin, ImVec2(item.x0, top)), point(origin, ImVec2(item.x1, std::min(top+3, size.y))), item.color);
        }
        if (item.row >= 0 && item.channel)
            draw->AddLine(point(origin, ImVec2(item.x0, top)), point(origin, ImVec2((item.labelMin.x+item.labelMax.x)*.5f, item.labelMin.y)), item.color);
    }
    // Draw text after all guides so crowded markers cannot paint over labels.
    for (const auto& item : labels.items) {
        const float top = labels.titleHeight;
        if (item.row >= 0) {
            draw->AddRectFilled(point(origin, item.labelMin), point(origin, item.labelMax), IM_COL32(12, 17, 24, 215), 2);
            draw->AddText(point(origin, ImVec2(item.labelMin.x+3, item.labelMin.y+2)), item.color, item.text.c_str());
        }
        if (hovered && mouse.y >= origin.y+top) {
            const float x = mouse.x-origin.x;
            const bool onLabel = item.row >= 0 && ImGui::IsMouseHoveringRect(point(origin, item.labelMin), point(origin, item.labelMax));
            float distance = onLabel ? -1 : item.channel ? std::abs(x-item.x0) : x >= item.x0 && x <= item.x1 ? 4.f : 1000.f;
            if (distance <= 5 && distance < bestDistance) { hover = &item; bestDistance = distance; }
        }
    }
    draw->PopClipRect();
    if (hover) ImGui::SetTooltip("%s", hover->detail.c_str());
    else if (hovered && mouse.y < origin.y+labels.titleHeight) ImGui::SetTooltip("%s", labels.fullTitle.c_str());
}
