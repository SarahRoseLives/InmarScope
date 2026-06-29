#pragma once
#include <cstdint>
#include <string>

// Returns a 2-letter country code (null-terminated) for the given 24-bit ICAO
// aircraft address, or nullptr if not found.
const char* icaoCountry(uint32_t icao);

// Convert a 2-letter ISO country code to a Unicode Regional Indicator pair
// (UTF-8 encoded, 8 bytes). Requires a font covering U+1F1E6..U+1F1FF such
// as BabelStone Flags to render.  "US" -> 🇺🇸
// Returns empty string if cc is null or not exactly 2 uppercase letters.
std::string ccToFlag(const char* cc);

// Returns true if the ICAO falls within a known military sub-range.
bool isMilitaryIcao(uint32_t icao);
