// Clipboard helpers for table panes: right-click / Ctrl+C copies the hovered
// row; a Copy button copies every currently visible row.
#pragma once

#include "imgui.h"
#include "imgui_internal.h"
#include "i18n/i18n.h"

#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

inline std::string copyFmt(const char* fmt, ...)
{
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return std::string(buf);
}

inline std::string copyJoin(const std::vector<std::string>& rows)
{
    std::string out;
    out.reserve(256 * rows.size());
    for (size_t i = 0; i < rows.size(); ++i)
    {
        if (i)
            out.push_back('\n');
        out += rows[i];
    }
    return out;
}

inline bool copyAllButton(const std::string& text)
{
    if (ImGui::SmallButton(_L("Copy")))
    {
        if (!text.empty())
            ImGui::SetClipboardText(text.c_str());
        return true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("%s", _L("Copy visible rows to the clipboard"));
    return false;
}

// Call inside BeginTable / EndTable, after rows have been submitted.
inline void handleTableCopy(const std::vector<std::string>& rows, int headerRows = 1)
{
    const int hovered = ImGui::TableGetHoveredRow();
    const int idx = hovered - headerRows;
    const bool haveRow = idx >= 0 && idx < (int)rows.size();

    static std::string pending;
    if (haveRow && ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
    {
        pending = rows[(size_t)idx];
        ImGui::OpenPopup("##copy_table_row");
    }

    if (ImGui::BeginPopup("##copy_table_row"))
    {
        if (ImGui::MenuItem(_L("Copy row")) && !pending.empty())
            ImGui::SetClipboardText(pending.c_str());
        if (ImGui::MenuItem(_L("Copy all visible")))
            ImGui::SetClipboardText(copyJoin(rows).c_str());
        ImGui::EndPopup();
    }

    if (haveRow && !ImGui::GetIO().WantTextInput &&
        ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false))
        ImGui::SetClipboardText(rows[(size_t)idx].c_str());
}
