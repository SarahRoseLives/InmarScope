#pragma once
#include "decode/message_log.h"
#include <string>
#include <vector>

// Merge receiver snapshots by AES ID and serialize only locally decoded data.
std::string flightMapJson(const std::vector<AircraftEntry>& aircraft);
