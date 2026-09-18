#include "gui/waterfall_labels.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>

namespace {
std::string oneLine(std::string text) {
    for (char& c : text) if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    return text;
}
float textWidth(const std::string& text, float fontSize = 0) {
    return fontSize > 0 ? ImGui::GetFont()->CalcTextSizeA(fontSize, std::numeric_limits<float>::max(), 0, text.c_str()).x
                        : ImGui::CalcTextSize(text.c_str()).x;
}
std::string fit(std::string text, float width, float fontSize = 0) {
    if (textWidth(text,fontSize) <= width) return text;
    const std::string suffix = "...";
    if (textWidth(suffix,fontSize) > width) return {};
    while (!text.empty() && textWidth(text+suffix,fontSize) > width) {
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
    auto band = [&](double lo, double hi, const std::string& name, ImU32 color, const std::string& detail) {
        if (hi < visibleLo || lo > visibleHi) return;
        WaterfallLabel heading;
        heading.x0 = pixel(std::max(lo,visibleLo)); heading.x1 = pixel(std::min(hi,visibleHi));
        heading.text = oneLine(name); heading.detail = detail; heading.color = color;
        result.bands.push_back(std::move(heading));
    };
    // Service headings describe the span of known presets, not an allocation claim.
    if (plan && plan->valid) {
        std::map<std::string,std::vector<const BandPlanEntry*>> services;
        for (const auto& entry : plan->entries) {
            if (entry.frequencyMHz > 0) services[entry.service.empty() ? "Channels" : entry.service].push_back(&entry);
            else band(entry.loMHz,entry.hiMHz,entry.label,entry.color,
                      entry.label+"\nAllocation: "+frequency(entry.loMHz)+" - "+frequency(entry.hiMHz));
        }
        for (const auto& service : services) {
            double lo = std::numeric_limits<double>::max(), hi = 0;
            for (const auto* entry : service.second) { lo = std::min(lo,entry->frequencyMHz); hi = std::max(hi,entry->frequencyMHz); }
            band(lo,hi,service.first,service.second.front()->color,
                 service.first+"\nKnown channel span: "+frequency(lo)+" - "+frequency(hi));
        }
    }
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
    if (result.items.empty() && result.bands.empty() && result.fullTitle.empty()) return result;
    if (result.fullTitle.empty()) result.fullTitle = "Decoder channels";
    result.rowHeight = ImGui::GetTextLineHeight()+6;
    result.titleHeight = size.y >= result.rowHeight*2 ? result.rowHeight : 0;
    result.title = fit(result.fullTitle, size.x-8);
    result.bandFontSize = ImGui::GetFontSize()*1.25f;
    const float bandRowHeight = result.bandFontSize+8;
    int bandRows = result.bands.empty() ? 0 : std::clamp(int((size.y*.28f-result.titleHeight)/bandRowHeight),0,2);
    if (!result.bands.empty() && bandRows == 0 && result.titleHeight+bandRowHeight+result.rowHeight <= size.y*.7f)
        bandRows = 1;
    std::stable_sort(result.bands.begin(),result.bands.end(),[](const auto& a,const auto& b) {
        return a.x1-a.x0 > b.x1-b.x0; // broad service labels survive crowded views
    });
    std::vector<std::vector<ImVec2>> bandOccupied(bandRows);
    for (auto& heading : result.bands) {
        if (size.x < 50) continue;
        // Short service spans can have a wider label, connected to their range.
        const float maxWidth = std::min(size.x-8,std::max(heading.x1-heading.x0-4,160.f));
        heading.text = fit(heading.text,maxWidth-8,result.bandFontSize);
        const float width = textWidth(heading.text,result.bandFontSize)+8;
        const float x = std::clamp((heading.x0+heading.x1-width)*.5f,2.f,size.x-width-2);
        for (int row=0;row<bandRows;++row) {
            bool free=true;
            for (const auto& span : bandOccupied[row]) if(x<span.y+4 && x+width+4>span.x) {free=false;break;}
            if (!free) continue;
            heading.row=row; heading.labelMin=ImVec2(x,result.titleHeight+row*bandRowHeight);
            heading.labelMax=ImVec2(x+width,heading.labelMin.y+bandRowHeight-2);
            bandOccupied[row].push_back(ImVec2(x,x+width));
            result.bandHeight=std::max(result.bandHeight,(row+1)*bandRowHeight);
            break;
        }
    }
    result.channelTop = result.titleHeight+result.bandHeight;
    const float headerBudget = std::max(size.y*.45f,std::min(size.y*.7f,result.channelTop+result.rowHeight));
    const int rows = std::clamp(int((headerBudget-result.channelTop)/result.rowHeight), 0, 3);
    result.headerHeight = result.channelTop+rows*result.rowHeight;
    std::stable_sort(result.items.begin(), result.items.end(), [](const auto& a, const auto& b) {
        if (a.active != b.active) return a.active > b.active;
        if (a.channel != b.channel) return a.channel > b.channel;
        return a.x0 < b.x0;
    });
    std::vector<std::vector<ImVec2>> occupied(rows);
    for (auto& item : result.items) {
        if (!item.channel) continue; // allocations occupy the larger service tier
        const float maxWidth = size.x-8;
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
            item.labelMin = ImVec2(x, result.channelTop+row*result.rowHeight);
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
        const float top = labels.channelTop;
        if (item.channel) {
            draw->AddLine(point(origin, ImVec2(item.x0, top)), point(origin, ImVec2(item.x0, size.y)), (item.color & 0x00ffffffu) | (45u << 24));
            draw->AddLine(point(origin, ImVec2(item.x0, top)), point(origin, ImVec2(item.x0, std::min(top+6, size.y))), item.color, 2);
        }
        if (item.row >= 0 && item.channel)
            draw->AddLine(point(origin, ImVec2(item.x0, top)), point(origin, ImVec2((item.labelMin.x+item.labelMax.x)*.5f, item.labelMin.y)), item.color);
    }
    // Draw text after all guides so crowded markers cannot paint over labels.
    for (const auto& item : labels.items) {
        const float top = labels.channelTop;
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
    for (const auto& heading : labels.bands) {
        const float y = heading.row >= 0 ? heading.labelMax.y : labels.titleHeight;
        draw->AddLine(point(origin,ImVec2(heading.x0,y)),point(origin,ImVec2(heading.x1,y)),heading.color,2);
        if (hovered && mouse.x >= origin.x+heading.x0 && mouse.x <= origin.x+heading.x1 &&
            std::abs(mouse.y-origin.y-y) <= 3) hover=&heading;
    }
    for (const auto& heading : labels.bands) {
        if (heading.row < 0) continue;
        draw->AddRectFilled(point(origin,heading.labelMin),point(origin,heading.labelMax),IM_COL32(12,17,24,230),2);
        draw->AddText(ImGui::GetFont(),labels.bandFontSize,point(origin,ImVec2(heading.labelMin.x+4,heading.labelMin.y+2)),
                      IM_COL32(235,240,245,255),heading.text.c_str());
        if (hovered && ImGui::IsMouseHoveringRect(point(origin,heading.labelMin),point(origin,heading.labelMax))) hover=&heading;
    }
    draw->PopClipRect();
    if (hover) ImGui::SetTooltip("%s", hover->detail.c_str());
    else if (hovered && mouse.y < origin.y+labels.titleHeight) ImGui::SetTooltip("%s", labels.fullTitle.c_str());
}
