// Plain-English breakdown of an ACARS message: who sent it to whom, what its
// label means, the downlink header fields (message number, flight), and the
// content where it follows a known format - METAR/SPECI/TAF weather, OOOI
// departure/arrival reports, coordinates - plus libacars' decoding of
// CPDLC / ADS-C / media advisories.
#pragma once

#include <string>

struct DecodedMessage;

// One or more lines of explanation (newline separated).
std::string explainAcars(const DecodedMessage& m);

// Short meaning of an ACARS label ("" if not a well-known one).
std::string acarsLabelMeaning(const std::string& label);
