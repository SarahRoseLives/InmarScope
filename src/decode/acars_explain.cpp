#include "decode/acars_explain.h"

#include "decode/message_log.h"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace {

bool isUpperAlpha(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) if (c < 'A' || c > 'Z') return false;
    return true;
}
bool allDigits(const std::string& s, size_t from = 0, size_t n = std::string::npos) {
    if (n == std::string::npos) n = s.size() - from;
    if (from + n > s.size() || n == 0) return false;
    for (size_t i = from; i < from + n; ++i) if (!std::isdigit((unsigned char)s[i])) return false;
    return true;
}
std::vector<std::string> tokens(const std::string& s) {
    std::vector<std::string> t; std::istringstream is(s); std::string w;
    while (is >> w) t.push_back(w);
    return t;
}
std::string fmt(const char* f, ...) {
    char b[512];
    va_list ap; va_start(ap, f); std::vsnprintf(b, sizeof(b), f, ap); va_end(ap);
    return b;
}

// ---- METAR / SPECI / TAF ----------------------------------------------------

std::string weatherCode(const std::string& w) {
    // Intensity / proximity, descriptor, phenomena (WMO / ICAO Annex 3).
    static const char* codes[][2] = {
        {"MI", "shallow"}, {"PR", "partial"}, {"BC", "patches of"}, {"DR", "low drifting"},
        {"BL", "blowing"}, {"SH", "showers of"}, {"TS", "thunderstorm"}, {"FZ", "freezing"},
        {"DZ", "drizzle"}, {"RA", "rain"}, {"SN", "snow"}, {"SG", "snow grains"},
        {"IC", "ice crystals"}, {"PL", "ice pellets"}, {"GR", "hail"}, {"GS", "small hail"},
        {"UP", "unknown precipitation"}, {"BR", "mist"}, {"FG", "fog"}, {"FU", "smoke"},
        {"VA", "volcanic ash"}, {"DU", "dust"}, {"SA", "sand"}, {"HZ", "haze"}, {"PY", "spray"},
        {"PO", "dust whirls"}, {"SQ", "squalls"}, {"FC", "funnel cloud"}, {"SS", "sandstorm"},
        {"DS", "duststorm"}};
    std::string s = w, out;
    if (s.rfind("RE", 0) == 0 && s.size() > 2) { out = "recent "; s = s.substr(2); }
    if (!s.empty() && s[0] == '-') { out += "light "; s = s.substr(1); }
    else if (!s.empty() && s[0] == '+') { out += "heavy "; s = s.substr(1); }
    bool vc = false;
    if (s.rfind("VC", 0) == 0) { vc = true; s = s.substr(2); }
    if (s.empty() || s.size() % 2) return "";
    std::string parts;
    for (size_t i = 0; i < s.size(); i += 2) {
        const std::string c = s.substr(i, 2);
        const char* m = nullptr;
        for (auto& e : codes) if (c == e[0]) { m = e[1]; break; }
        if (!m) return "";
        if (!parts.empty() && parts.back() != ' ') parts += (c == "TS" || i == 2) ? " " : " and ";
        if (c == "TS" && i + 2 < s.size()) { parts += "thunderstorm with "; continue; }
        parts += m;
    }
    out += parts;
    if (vc) out += " in the vicinity";
    return out;
}

bool decodeWeatherGroup(const std::string& t, std::string& out) {
    // Wind dddssKT, dddssGggKT, VRBssKT (also MPS / KMH)
    {
        size_t u = std::string::npos; const char* unit = nullptr;
        if (t.size() >= 7 && t.compare(t.size() - 2, 2, "KT") == 0) { u = t.size() - 2; unit = "kt"; }
        else if (t.size() >= 8 && t.compare(t.size() - 3, 3, "MPS") == 0) { u = t.size() - 3; unit = "m/s"; }
        else if (t.size() >= 8 && t.compare(t.size() - 3, 3, "KMH") == 0) { u = t.size() - 3; unit = "km/h"; }
        if (unit) {
            std::string b = t.substr(0, u);
            std::string dir = b.substr(0, 3);
            if ((allDigits(dir) || dir == "VRB") && b.size() >= 5) {
                std::string rest = b.substr(3), spd = rest, gust;
                size_t g = rest.find('G');
                if (g != std::string::npos) { spd = rest.substr(0, g); gust = rest.substr(g + 1); }
                if (allDigits(spd) && (gust.empty() || allDigits(gust))) {
                    if (dir == "000" && std::atoi(spd.c_str()) == 0) { out = "Wind: calm"; return true; }
                    out = "Wind: " + (dir == "VRB" ? std::string("variable direction") : "from " + std::to_string(std::atoi(dir.c_str())) + "\xC2\xB0")
                        + " at " + std::to_string(std::atoi(spd.c_str())) + " " + unit;
                    if (!gust.empty()) out += ", gusting " + std::to_string(std::atoi(gust.c_str())) + " " + unit;
                    return true;
                }
            }
        }
    }
    if (t.size() == 7 && allDigits(t, 0, 3) && t[3] == 'V' && allDigits(t, 4, 3)) {
        out = "Wind direction varying between " + std::to_string(std::atoi(t.substr(0, 3).c_str())) + "\xC2\xB0 and " +
              std::to_string(std::atoi(t.substr(4).c_str())) + "\xC2\xB0"; return true;
    }
    if (t == "CAVOK") { out = "Ceiling and visibility OK (10 km or more, no significant cloud or weather)"; return true; }
    if (t.size() == 4 && allDigits(t)) {
        out = t == "9999" ? "Visibility: 10 km or more" : "Visibility: " + std::to_string(std::atoi(t.c_str())) + " m";
        return true;
    }
    if (t.size() > 2 && t.compare(t.size() - 2, 2, "SM") == 0) {
        std::string v = t.substr(0, t.size() - 2), pre;
        if (!v.empty() && (v[0] == 'P' || v[0] == 'M')) { pre = v[0] == 'P' ? "more than " : "less than "; v = v.substr(1); }
        out = "Visibility: " + pre + v + " statute mile" + (v == "1" ? "" : "s"); return true;
    }
    if (t == "SKC" || t == "CLR") { out = "Sky clear"; return true; }
    if (t == "NSC") { out = "No significant cloud"; return true; }
    if (t == "NCD") { out = "No cloud detected"; return true; }
    if (t == "NOSIG") { out = "No significant change expected"; return true; }
    if (t == "AUTO") { out = "Automated observation"; return true; }
    if (t == "COR") { out = "Corrected report"; return true; }
    {
        static const char* cov[][2] = {{"FEW", "Few clouds"}, {"SCT", "Scattered clouds"}, {"BKN", "Broken clouds"},
                                       {"OVC", "Overcast"}, {"VV", "Sky obscured, vertical visibility"}};
        for (auto& c : cov) {
            const size_t n = std::string(c[0]).size();
            if (t.size() >= n + 3 && t.compare(0, n, c[0]) == 0 && allDigits(t, n, 3)) {
                out = std::string(c[1]) + " at " + std::to_string(std::atoi(t.substr(n, 3).c_str()) * 100) + " ft";
                const std::string ty = t.substr(n + 3);
                if (ty == "CB") out += " (cumulonimbus)";
                else if (ty == "TCU") out += " (towering cumulus)";
                return true;
            }
        }
    }
    // Temperature / dew point: TT/DD with M for minus.
    {
        size_t sl = t.find('/');
        if (sl != std::string::npos && sl >= 2 && t.find('/', sl + 1) == std::string::npos) {
            auto tv = [](std::string v, int& o) {
                bool neg = !v.empty() && v[0] == 'M'; if (neg) v = v.substr(1);
                if (v.size() != 2 || !allDigits(v)) return false; o = std::atoi(v.c_str()) * (neg ? -1 : 1); return true; };
            int a, d;
            const std::string dp = t.substr(sl + 1);
            if (tv(t.substr(0, sl), a) && (dp.empty() || tv(dp, d))) {
                out = "Temperature " + std::to_string(a) + "\xC2\xB0" "C";
                if (!dp.empty()) out += ", dew point " + std::to_string(d) + "\xC2\xB0" "C";
                return true;
            }
        }
    }
    if (t.size() == 5 && t[0] == 'A' && allDigits(t, 1)) {
        out = fmt("Altimeter %.2f inHg", std::atoi(t.substr(1).c_str()) / 100.0); return true;
    }
    if (t.size() == 5 && t[0] == 'Q' && allDigits(t, 1)) {
        out = "Pressure (QNH) " + std::to_string(std::atoi(t.substr(1).c_str())) + " hPa"; return true;
    }
    if (t.size() > 1 && t[0] == 'R' && std::isdigit((unsigned char)t[1]) && t.find('/') != std::string::npos) {
        out = "Runway visual range on runway " + t.substr(1, t.find('/') - 1) + ": " + t.substr(t.find('/') + 1); return true;
    }
    {
        const std::string w = weatherCode(t);
        if (!w.empty()) { out = "Weather: " + w; return true; }
    }
    return false;
}

std::string dayTime(const std::string& t) {   // ddhhmmZ
    return "day " + t.substr(0, 2) + " at " + t.substr(2, 2) + ":" + t.substr(4, 2) + " UTC";
}
bool isTime6Z(const std::string& t) { return t.size() == 7 && allDigits(t, 0, 6) && t[6] == 'Z'; }

// Decodes the weather report starting at tokens[i] (the station). Returns lines.
std::vector<std::string> decodeReport(const std::vector<std::string>& tk, size_t i, const std::string& kind, size_t& end) {
    std::vector<std::string> L;
    L.push_back(kind + " for " + tk[i] + (i + 1 < tk.size() && isTime6Z(tk[i + 1]) ? ", issued " + dayTime(tk[i + 1]) : ""));
    size_t k = i + 1;
    if (k < tk.size() && isTime6Z(tk[k])) k++;
    for (; k < tk.size(); ++k) {
        const std::string& t = tk[k];
        if (t == "METAR" || t == "SPECI" || t == "TAF" || t == "=" ) break;
        if (k > i + 1 && t.size() == 4 && isUpperAlpha(t) && k + 1 < tk.size() && isTime6Z(tk[k + 1])) break;   // next report
        std::string tt = t;
        bool last = false;
        if (!tt.empty() && tt.back() == '=') { tt.pop_back(); last = true; }
        if (tt == "RMK") { L.push_back("  (remarks follow - not decoded)"); while (k + 1 < tk.size() && tk[k + 1].back() != '=') k++; break; }
        if (tt.size() == 9 && allDigits(tt, 0, 4) && tt[4] == '/' && allDigits(tt, 5, 4))
            L.push_back("  Valid from day " + tt.substr(0, 2) + " " + tt.substr(2, 2) + ":00 to day " + tt.substr(5, 2) + " " + tt.substr(7, 2) + ":00 UTC");
        else if (tt.size() == 8 && tt.compare(0, 2, "FM") == 0 && allDigits(tt, 2))
            L.push_back("  From day " + tt.substr(2, 2) + " " + tt.substr(4, 2) + ":" + tt.substr(6, 2) + " UTC:");
        else if (tt == "TEMPO") L.push_back("  Temporarily:");
        else if (tt == "BECMG") L.push_back("  Becoming:");
        else if (tt.compare(0, 4, "PROB") == 0 && tt.size() == 6) L.push_back("  " + tt.substr(4) + "% probability:");
        else if (tt == "AMD") L.push_back("  (amended forecast)");
        else {
            std::string o;
            if (decodeWeatherGroup(tt, o)) L.push_back("  " + o);
            else L.push_back("  " + tt + " (not decoded)");
        }
        if (last) { ++k; break; }
    }
    end = k;
    return L;
}

std::vector<std::string> findWeather(const std::string& text) {
    std::vector<std::string> out;
    const auto tk = tokens(text);
    for (size_t i = 0; i < tk.size();) {
        std::string kind;
        size_t st = std::string::npos;
        if ((tk[i] == "METAR" || tk[i] == "SPECI" || tk[i] == "TAF") && i + 1 < tk.size()) {
            kind = tk[i] == "TAF" ? "Forecast (TAF)" : (tk[i] == "SPECI" ? "Special weather report (SPECI)" : "Weather report (METAR)");
            st = i + 1;
            while (st < tk.size() && (tk[st] == "AMD" || tk[st] == "COR")) st++;
        } else if (tk[i].size() == 4 && isUpperAlpha(tk[i]) && i + 2 < tk.size() && isTime6Z(tk[i + 1])) {
            // Bare report: station, time, then a wind group or a validity period.
            std::string w;
            if (decodeWeatherGroup(tk[i + 2], w) && w.rfind("Wind", 0) == 0) { kind = "Weather report (METAR)"; st = i; }
            else if (tk[i + 2].size() == 9 && tk[i + 2][4] == '/') { kind = "Forecast (TAF)"; st = i; }
        }
        if (st != std::string::npos && st < tk.size() && tk[st].size() == 4 && isUpperAlpha(tk[st])) {
            size_t end = st;
            for (auto& l : decodeReport(tk, st, kind, end)) out.push_back(l);
            i = std::max(end, i + 1);
        } else {
            ++i;
        }
    }
    return out;
}

// ---- OOOI departure / arrival reports (DEP / ARR with an FI line) ----------
std::vector<std::string> findOooi(const std::string& text) {
    std::vector<std::string> out;
    const size_t fi = text.find("FI ");
    if (fi == std::string::npos || (text.find("DEP") == std::string::npos && text.find("ARR") == std::string::npos))
        return out;
    static const char* fields[][2] = {
        {"AN", "Aircraft registration"}, {"DA", "Departure airport"}, {"DS", "Destination"},
        {"AD", "Arrival airport"}, {"OT", "OUT (left the gate)"}, {"OUT", "OUT (left the gate)"},
        {"OF", "OFF (took off)"}, {"OFF", "OFF (took off)"}, {"ON", "ON (landed)"},
        {"IN", "IN (arrived at the gate)"}, {"FB", "Fuel on board"}, {"BF", "Fuel boarded"}};
    out.push_back(text.find("DEP") != std::string::npos ? "Departure report:" : "Arrival report:");
    std::string line = text.substr(fi + 3);
    std::string first = line.substr(0, line.find('/'));
    while (!first.empty() && first.back() == ' ') first.pop_back();
    out.push_back("  Flight " + first);
    size_t p = 0;
    while ((p = line.find('/', p)) != std::string::npos) {
        size_t q = line.find('/', p + 1);
        std::string item = line.substr(p + 1, q == std::string::npos ? std::string::npos : q - p - 1);
        while (!item.empty() && item.back() == ' ') item.pop_back();
        for (auto& f : fields) {
            const std::string k = f[0];
            if (item.compare(0, k.size(), k) == 0 && item.size() > k.size()) {
                std::string v = item.substr(k.size());
                while (!v.empty() && v[0] == ' ') v.erase(0, 1);
                if ((k == "OT" || k == "OUT" || k == "OF" || k == "OFF" || k == "ON" || k == "IN") && v.size() >= 4 && allDigits(v, 0, 4))
                    v = v.substr(0, 2) + ":" + v.substr(2, 2) + " UTC";
                out.push_back(std::string("  ") + f[1] + ": " + v);
                break;
            }
        }
        if (q == std::string::npos) break;
        p = q;
    }
    return out;
}

// ---- Coordinates like N43123W079456 / N4312.3 W07945.6 ---------------------
std::vector<std::string> findPositions(const std::string& text) {
    std::vector<std::string> out;
    for (size_t i = 0; i + 12 < text.size() && out.size() < 3; ++i) {
        const char ns = text[i];
        if ((ns != 'N' && ns != 'S') || (i > 0 && std::isdigit((unsigned char)text[i - 1]))) continue;
        size_t j = i + 1; std::string la;
        while (j < text.size() && (std::isdigit((unsigned char)text[j]) || text[j] == '.')) la += text[j++];
        if (j < text.size() && text[j] == ' ') j++;
        if (j >= text.size() || (text[j] != 'E' && text[j] != 'W')) continue;
        const char ew = text[j++]; std::string lo;
        while (j < text.size() && (std::isdigit((unsigned char)text[j]) || text[j] == '.')) lo += text[j++];
        auto conv = [](const std::string& s, int degDigits, double& o) {
            std::string d = s; d.erase(std::remove(d.begin(), d.end(), '.'), d.end());
            if ((int)d.size() < degDigits + 2) return false;
            const double deg = std::atof(d.substr(0, degDigits).c_str());
            std::string mm = d.substr(degDigits, 2) + "." + d.substr(degDigits + 2);   // minutes and decimals
            const double mn = std::atof(mm.c_str());
            if (mn >= 60) return false;
            o = deg + mn / 60.0; return true; };
        double lat, lon;
        if (la.size() >= 4 && lo.size() >= 5 && conv(la, 2, lat) && conv(lo, 3, lon) && lat <= 90 && lon <= 180) {
            if (ns == 'S') lat = -lat;
            if (ew == 'W') lon = -lon;
            out.push_back(fmt("Position: %.4f, %.4f (from \"%s\")", lat, lon, text.substr(i, j - i).c_str()));
            i = j;
        }
    }
    return out;
}

} // namespace

std::string acarsLabelMeaning(const std::string& label) {
    if (label.size() == 2 && label[0] == '_' && (unsigned char)label[1] == 0x7f)
        return "General response / acknowledgement only (no information; keeps the link alive)";
    struct L { const char* l; const char* m; };
    static const L tbl[] = {
        {"Q0", "Link test"},
        {"SQ", "Squitter (ground station identification)"},
        {":;", "Data radio auto-tune (tells the aircraft to change frequency)"},
        {"H1", "Message to/from an onboard terminal (e.g. flight management computer, crew printer)"},
        {"SA", "Media advisory (which datalinks - VHF, SATCOM, HF - are up or down)"},
        {"B9", "ATIS information request"},
        {"52", "Ground UTC request"},
        {"A6", "ADS-C contract request (air traffic control asking for position reports)"},
        {"B6", "ADS-C report (automatic position report to air traffic control)"},
        {"AA", "CPDLC / ATC message to the aircraft"},
        {"BA", "CPDLC / ATC message from the aircraft"},
        {"C1", "Uplink to cockpit printer"}, {"C2", "Uplink to cockpit printer"}, {"C3", "Uplink to cockpit printer"},
    };
    for (auto& e : tbl) if (label == e.l) return e.m;
    if (label.size() == 2 && label[0] == 'Q' && label[1] >= 'A' && label[1] <= 'Z')
        return "OOOI event report (out of gate / off / on / into gate, fuel, delays)";
    if (!label.empty()) return "Airline- or application-specific label";
    return "";
}

std::string explainAcars(const DecodedMessage& m)
{
    std::vector<std::string> L;
    L.push_back(m.downlink ? "Sent BY the aircraft (downlink, air-to-ground)"
                           : "Sent TO the aircraft (uplink, ground-to-air)");
    {
        std::string who = "Aircraft:";
        if (!m.reg.empty()) who += " registration " + m.reg;
        if (m.aesId) who += fmt("%s ICAO address %06X", m.reg.empty() ? "" : ",", m.aesId & 0xFFFFFF);
        if (!m.flight.empty()) who += ", flight " + m.flight;
        if (who != "Aircraft:") L.push_back(who);
    }
    if (m.gesId) L.push_back(fmt("Ground earth station: %u", (unsigned)m.gesId));
    const std::string lm = acarsLabelMeaning(m.label);
    if (!m.label.empty()) {
        std::string lab = m.label;
        for (auto& c : lab) if ((unsigned char)c < 0x20 || (unsigned char)c >= 0x7f) c = '.';
        L.push_back("Label " + lab + ": " + lm);
    }
    std::string text = m.text;
    // Downlinks start with a 4-character message number and a 6-character flight number.
    if (m.downlink && text.size() >= 10 && std::isalpha((unsigned char)text[0]) && allDigits(text, 1, 2) &&
        std::isalpha((unsigned char)text[3])) {
        std::string flt = text.substr(4, 6);
        while (!flt.empty() && flt.back() == ' ') flt.pop_back();
        L.push_back("Message number " + text.substr(0, 4) + (flt.empty() ? "" : ", flight " + flt));
        text = text.substr(10);
    }
    if (!m.decoded.empty()) {
        L.push_back("Decoded application data:");
        std::istringstream is(m.decoded); std::string l;
        while (std::getline(is, l)) L.push_back("  " + l);
    }
    if (m.hasPos) L.push_back(fmt("Reported position: %.4f, %.4f at %d ft", m.lat, m.lon, m.alt));
    bool content = !m.decoded.empty();
    for (auto& l : findWeather(text)) { L.push_back(l); content = true; }
    for (auto& l : findOooi(text)) { L.push_back(l); content = true; }
    for (auto& l : findPositions(text)) { L.push_back(l); content = true; }
    if (!content) {
        if (text.empty()) L.push_back("No text (acknowledgement / link maintenance)");
        else L.push_back("Text: free text or an airline-specific format (shown as received above)");
    }
    std::string out;
    for (auto& l : L) { out += l; out += '\n'; }
    return out;
}
