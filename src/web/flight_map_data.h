#pragma once
#include "decode/message_log.h"
#include <string>
#include <vector>
#include <map>
#include <set>

struct FlightMapPosition { double lat=0, lon=0, time=0; int alt=0; };
using FlightMapPositions = std::map<std::string, FlightMapPosition>;
std::string flightMapIcao(const std::string& value);
std::set<std::string> flightMapLookupIds(const std::vector<AircraftEntry>& aircraft, double now);
FlightMapPositions parseFlightMapPositions(const std::string& body, const std::set<std::string>& requested, double now);
// Membership always comes from receiver snapshots, never from an online feed.
std::string flightMapJson(const std::vector<AircraftEntry>& aircraft,
                         const FlightMapPositions& positions = {}, double now = 0);
