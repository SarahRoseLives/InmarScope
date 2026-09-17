#include "web/flight_map_data.h"
#include <jansson.h>
#include <cmath>
#include <cstdlib>
#include <map>

namespace {
bool validPosition(const AircraftEntry& a) {
    return a.hasPos && std::isfinite(a.lat) && std::isfinite(a.lon) &&
           std::abs(a.lat) <= 90 && std::abs(a.lon) <= 180;
}
}
std::string flightMapJson(const std::vector<AircraftEntry>& aircraft) {
    std::map<uint32_t, AircraftEntry> merged;
    for (const auto& a : aircraft) {
        if (!a.aesId) continue;
        auto it = merged.find(a.aesId);
        if (it == merged.end()) { merged[a.aesId] = a; continue; }
        const auto old = it->second;
        auto& target = it->second;
        if (a.lastSeen > old.lastSeen) target = a;
        const auto& other = a.lastSeen > old.lastSeen ? old : a;
        if (target.icao.empty()) target.icao = other.icao;
        if (target.reg.empty()) target.reg = other.reg;
        if (target.flight.empty()) target.flight = other.flight;
        // A later voice/identity message must not discard a received position.
        if (validPosition(other) && (!validPosition(target) || other.posTime > target.posTime)) {
            target.hasPos = true; target.lat = other.lat; target.lon = other.lon;
            target.alt = other.alt; target.posTime = other.posTime;
        }
    }
    json_t* array = json_array();
    for (const auto& item : merged) {
        const auto& a = item.second;
        char id[16]; std::snprintf(id, sizeof(id), "%06X", a.aesId);
        json_t* row = json_object();
        json_object_set_new(row, "id", json_string(id));
        json_object_set_new(row, "icao", json_string(a.icao.c_str()));
        json_object_set_new(row, "flight", json_string(a.flight.c_str()));
        json_object_set_new(row, "reg", json_string(a.reg.c_str()));
        if (validPosition(a)) {
            json_object_set_new(row, "lat", json_real(a.lat));
            json_object_set_new(row, "lon", json_real(a.lon));
            json_object_set_new(row, "alt", json_integer(a.alt));
            json_object_set_new(row, "posTime", json_real(std::isfinite(a.posTime) ? a.posTime : 0));
        }
        json_array_append_new(array, row);
    }
    char* text = json_dumps(array, JSON_COMPACT | JSON_ENSURE_ASCII);
    std::string result = text ? text : "[]";
    std::free(text); json_decref(array); return result;
}
