#include "decode/band_plan.h"
#include <jansson.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {
std::string text(json_t* object, const char* key) {
    const char* value = json_string_value(json_object_get(object, key));
    return value ? value : "";
}
bool scopes(json_t* root, const char* plural, const char* singular, std::vector<std::string>& out) {
    if (auto* value = json_object_get(root, plural)) {
        if (!json_is_array(value)) return false;
        size_t i; json_t* item;
        json_array_foreach(value, i, item) {
            if (!json_is_string(item) || !*json_string_value(item)) return false;
            out.emplace_back(json_string_value(item));
        }
    }
    if (auto* value = json_object_get(root, singular)) {
        if (!json_is_string(value) || !*json_string_value(value)) return false;
        out.emplace_back(json_string_value(value));
    }
    std::sort(out.begin(), out.end()); out.erase(std::unique(out.begin(), out.end()), out.end());
    return true;
}
bool color(std::string value, uint32_t& result) {
    if (!value.empty() && value.front() == '#') value.erase(0, 1);
    if (value.size() != 6 || !std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isxdigit(c); })) return false;
    const auto v = static_cast<uint32_t>(std::stoul(value, nullptr, 16));
    result = 0xFF000000u | ((v & 255u) << 16) | (v & 0xFF00u) | ((v >> 16) & 255u);
    return true;
}
}
BandPlan loadBandPlan(const std::string& path) {
    BandPlan bp; bp.filePath = path;
    std::ifstream file(std::filesystem::u8path(path), std::ios::binary);
    if (!file) { bp.error = "Cannot open file"; return bp; }
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    json_error_t error{};
    json_t* root = json_loads(content.c_str(), JSON_REJECT_DUPLICATES, &error);
    if (!root) { bp.error = "Invalid JSON at line " + std::to_string(error.line) + ": " + error.text; return bp; }
    auto reject = [&](const std::string& why) { bp.error = why; bp.entries.clear(); json_decref(root); return bp; };
    if (!json_is_object(root)) return reject("Expected a JSON object");
    bp.name = text(root, "name"); bp.designator = text(root, "designator");
    bp.notes = text(root, "notes");
    if (bp.name.empty()) return reject("A non-empty name is required");
    if (auto* p = json_object_get(root, "position")) {
        if (!json_is_number(p) || !std::isfinite(json_number_value(p)) || std::abs(json_number_value(p)) > 180)
            return reject("Satellite position must be degrees from -180 to 180");
        bp.position = json_number_value(p);
    }
    if (!scopes(root, "regions", "region", bp.regions) || !scopes(root, "countries", "country", bp.countries))
        return reject("Regions/countries must contain non-empty text names");
    auto* bands = json_object_get(root, "bands");
    if (!json_is_array(bands) || !json_array_size(bands)) return reject("A non-empty bands array is required");
    size_t i; json_t* entry;
    json_array_foreach(bands, i, entry) {
        const std::string prefix = "Band " + std::to_string(i + 1) + ": ";
        if (!json_is_object(entry)) return reject(prefix + "expected an object");
        auto* lo = json_object_get(entry, "lo"); auto* hi = json_object_get(entry, "hi");
        BandPlanEntry e{}; e.label = text(entry, "label");
        if (!json_is_number(lo) || !json_is_number(hi)) return reject(prefix + "lo and hi must be numbers in MHz");
        e.loMHz = json_number_value(lo); e.hiMHz = json_number_value(hi);
        if (!std::isfinite(e.loMHz) || !std::isfinite(e.hiMHz) || e.loMHz < 0 || e.loMHz >= e.hiMHz || e.label.empty())
            return reject(prefix + "require 0 <= lo < hi in MHz and a non-empty label");
        if (auto* frequency = json_object_get(entry, "frequency")) {
            if (!json_is_number(frequency)) return reject(prefix + "frequency must be a channel center in MHz");
            e.frequencyMHz = json_number_value(frequency);
            if (!std::isfinite(e.frequencyMHz) || e.frequencyMHz <= 0 || e.frequencyMHz < e.loMHz || e.frequencyMHz > e.hiMHz)
                return reject(prefix + "channel frequency must be inside lo/hi");
        }
        e.service = text(entry, "service");
        const auto* colorValue = json_object_get(entry, "color");
        if ((colorValue && !json_is_string(colorValue)) || !color(colorValue ? text(entry, "color") : "888888", e.color))
            return reject(prefix + "color must contain six hexadecimal digits (RRGGBB)");
        bp.entries.push_back(e);
    }
    json_decref(root);
    std::sort(bp.entries.begin(), bp.entries.end(), [](const auto& a, const auto& b) { return a.loMHz < b.loMHz; });
    bp.valid = true; return bp;
}
std::vector<BandPlanGroup> bandPlanGroups(const BandPlan& plan, double sampleRateHz) {
    std::vector<BandPlanGroup> result;
    if (!plan.valid || !std::isfinite(sampleRateHz) || sampleRateHz <= 0) return result;
    std::vector<std::string> services;
    // Default to the Aero data group, rather than the first (often STD-C) frequency.
    for (const auto& preferred : {"Aero data", "Aero voice", "STD-C"})
        for (const auto& e : plan.entries)
            if (e.frequencyMHz > 0 && e.service == preferred) { services.emplace_back(preferred); break; }
    for (const auto& e : plan.entries)
        if (e.frequencyMHz > 0 && std::find(services.begin(), services.end(), e.service) == services.end())
            services.push_back(e.service);
    const double usable = sampleRateHz / 1e6 * 0.8; // leave both filter edges clear
    for (const auto& service : services) {
        std::vector<size_t> indices;
        for (size_t i = 0; i < plan.entries.size(); ++i)
            if (plan.entries[i].frequencyMHz > 0 && plan.entries[i].service == service) indices.push_back(i);
        std::sort(indices.begin(), indices.end(), [&](size_t a, size_t b) { return plan.entries[a].frequencyMHz < plan.entries[b].frequencyMHz; });
        for (size_t begin = 0; begin < indices.size();) {
            size_t end = begin + 1;
            const double lo = plan.entries[indices[begin]].frequencyMHz;
            while (end < indices.size() && plan.entries[indices[end]].frequencyMHz - lo <= usable) ++end;
            const double hi = plan.entries[indices[end-1]].frequencyMHz;
            // Offset a single channel slightly to avoid the receiver's DC notch.
            result.push_back({service.empty() ? "Channels" : service, lo, hi,
                              (lo+hi)*0.5 + (lo == hi ? usable*0.05 : 0.0),
                              std::vector<size_t>(indices.begin()+begin, indices.begin()+end)});
            begin = end;
        }
    }
    return result;
}
std::string bandPlanLabel(const BandPlan& bp) {
    std::string label;
    for (const auto& scope : bp.regions) { if (!label.empty()) label += ", "; label += scope; }
    for (const auto& scope : bp.countries) { if (!label.empty()) label += ", "; label += scope; }
    if (!label.empty()) label += " / ";
    if (!bp.designator.empty()) label += bp.designator + " - ";
    return label + bp.name;
}
void scanBandPlans(const char* dir, std::vector<std::string>& names, std::vector<std::string>& paths,
                   std::vector<std::string>* errors) {
    names.clear(); paths.clear(); if (errors) errors->clear();
    std::vector<std::pair<std::string, std::string>> found;
    std::error_code ec;
    const auto root = std::filesystem::u8path(dir);
    std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, ec), end;
    if (ec && errors) errors->push_back(std::string(dir) + ": " + ec.message());
    while (!ec && it != end) {
        if (it->is_regular_file(ec) && !ec) {
            auto extension = it->path().extension().string();
            std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return std::tolower(c); });
            if (extension == ".json") {
                const auto path = it->path().generic_u8string();
                auto bp = loadBandPlan(path);
                if (bp.valid) found.emplace_back(bandPlanLabel(bp) + " [" + it->path().lexically_relative(root).generic_u8string() + "]", path);
                else if (errors) errors->push_back(path + ": " + bp.error);
            }
        }
        it.increment(ec);
    }
    if (ec && errors) errors->push_back(std::string(dir) + ": " + ec.message());
    std::sort(found.begin(), found.end());
    for (const auto& item : found) { names.push_back(item.first); paths.push_back(item.second); }
}
