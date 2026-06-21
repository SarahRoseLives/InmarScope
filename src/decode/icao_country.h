#pragma once
#include <cstdint>
// Returns a 2-letter country code (null-terminated) for the given 24-bit ICAO
// aircraft address, or nullptr if not found.
const char* icaoCountry(uint32_t icao);
