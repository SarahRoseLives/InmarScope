#include "web/flight_map_data.h"
#include <jansson.h>
#include <cmath>
#include <cstdlib>
#include <map>
#include <cctype>
#include <stdexcept>

namespace {
bool validPosition(const AircraftEntry& a) {
    return a.hasPos && std::isfinite(a.lat) && std::isfinite(a.lon) &&
           std::abs(a.lat) <= 90 && std::abs(a.lon) <= 180;
}
}
std::string flightMapIcao(const std::string& value) {
    if (value.size() != 6) return {};
    std::string result;
    for (unsigned char c : value) {
        if (!std::isxdigit(c)) return {};
        result += static_cast<char>(std::toupper(c));
    }
    return result == "000000" ? std::string{} : result;
}
std::set<std::string> flightMapLookupIds(const std::vector<AircraftEntry>& aircraft, double now) {
    std::set<std::string> ids, decoded;
    for (const auto& a : aircraft) {
        const auto id = flightMapIcao(a.icao);
        if (!a.aesId || id.empty()) continue;
        if (validPosition(a)) decoded.insert(id);
        else if (std::isfinite(a.lastSeen) && now >= a.lastSeen && now - a.lastSeen <= 1800) ids.insert(id);
    }
    for (const auto& id : decoded) ids.erase(id);
    return ids;
}
FlightMapPositions parseFlightMapPositions(const std::string& body, const std::set<std::string>& requested, double now) {
    json_error_t error{};
    json_t* root = json_loads(body.c_str(), JSON_REJECT_DUPLICATES, &error);
    auto* rows = root ? json_object_get(root, "ac") : nullptr;
    if (!json_is_array(rows)) { if (root) json_decref(root); throw std::runtime_error("Invalid position response"); }
    FlightMapPositions result;
    size_t i; json_t* row;
    json_array_foreach(rows, i, row) {
        const char* hex = json_string_value(json_object_get(row, "hex"));
        const auto id = flightMapIcao(hex ? hex : "");
        if (id.empty() || !requested.count(id)) continue;
        auto* lat = json_object_get(row, "lat"); auto* lon = json_object_get(row, "lon");
        auto* age = json_object_get(row, "seen_pos");
        if (!json_is_number(lat) || !json_is_number(lon) || !json_is_number(age)) continue;
        const double la = json_number_value(lat), lo = json_number_value(lon), seconds = json_number_value(age);
        if (!std::isfinite(la) || !std::isfinite(lo) || !std::isfinite(seconds) ||
            std::abs(la) > 90 || std::abs(lo) > 180 || seconds < 0 || seconds > 300) continue;
        auto* alt = json_object_get(row, "alt_baro");
        const double altitude = json_is_number(alt) ? json_number_value(alt) : 0;
        result[id] = {la, lo, now-seconds, static_cast<int>(std::isfinite(altitude) && altitude >= -1500 && altitude <= 100000 ? altitude : 0)};
    }
    json_decref(root); return result;
}
std::string flightMapJson(const std::vector<AircraftEntry>& aircraft, const FlightMapPositions& positions, double now) {
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
        auto p = positions.find(flightMapIcao(a.icao));
        const bool decoded = validPosition(a);
        const bool online = p != positions.end() && now >= p->second.time && now-p->second.time <= 300 &&
                            now >= a.lastSeen && now-a.lastSeen <= 1800;
        if (decoded || online) {
            json_object_set_new(row, "lat", json_real(decoded ? a.lat : p->second.lat));
            json_object_set_new(row, "lon", json_real(decoded ? a.lon : p->second.lon));
            json_object_set_new(row, "alt", json_integer(decoded ? a.alt : p->second.alt));
            json_object_set_new(row, "posTime", json_real(decoded ? (std::isfinite(a.posTime) ? a.posTime : 0) : p->second.time));
            json_object_set_new(row, "positionSource", json_string(decoded ? "Decoded ADS-C" : "ADSB.lol (online)"));
        }
        json_array_append_new(array, row);
    }
    char* text = json_dumps(array, JSON_COMPACT | JSON_ENSURE_ASCII);
    std::string result = text ? text : "[]";
    std::free(text); json_decref(array); return result;
}
