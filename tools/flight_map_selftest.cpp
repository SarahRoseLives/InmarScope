#include "web/flight_map_data.h"
#include <jansson.h>
#include <iostream>
#include <limits>
#include "decode/acars_apps.h"
#include <fstream>
#define REQUIRE(x) do { if(!(x)) { std::cerr << "FAILED line " << __LINE__ << std::endl; return 1; } } while(0)
int main(int argc, char** argv) {
    REQUIRE(flightMapJson({}) == "[]");
    AircraftEntry position; position.aesId=1;position.hasPos=true;position.lat=-33;position.lon=151;position.posTime=20;position.lastSeen=20;
    AircraftEntry identity;identity.aesId=1;identity.icao="ABC123";identity.flight="</script>\"test";identity.lastSeen=30;
    AircraftEntry noPosition;noPosition.aesId=2;
    AircraftEntry invalid=position;invalid.aesId=3;invalid.lat=std::numeric_limits<double>::quiet_NaN();
    AircraftEntry origin=position;origin.aesId=4;origin.lat=0;origin.lon=0;
    auto payload=flightMapJson({position,identity,noPosition,invalid,origin});
    json_error_t error;auto* rows=json_loads(payload.c_str(),0,&error);REQUIRE(rows);
    REQUIRE(json_array_size(rows)==4);
    auto* row=json_array_get(rows,0);
    REQUIRE(json_number_value(json_object_get(row,"lat"))==-33);
    REQUIRE(std::string(json_string_value(json_object_get(row,"flight")))==identity.flight);
    REQUIRE(!json_object_get(json_array_get(rows,1),"lat"));
    REQUIRE(!json_object_get(json_array_get(rows,2),"lat"));
    REQUIRE(json_object_get(json_array_get(rows,3),"lat"));
    json_decref(rows);
    REQUIRE(flightMapJson({identity,position})==flightMapJson({position,identity}));
    position.posTime=40;position.lat=-34;
    auto newer=position;newer.posTime=50;newer.lat=-35;newer.lastSeen=10;
    rows=json_loads(flightMapJson({position,newer}).c_str(),0,&error);
    REQUIRE(json_number_value(json_object_get(json_array_get(rows,0),"lat"))==-35);json_decref(rows);
    // Regression: identity/voice reception has no ADS-C coordinates. Only
    // this received ICAO may acquire an online marker, not adjacent traffic.
    AircraftTable table;
    table.setIcao(0xABC123, "aBc123", 1000);
    auto received = table.snapshot();
    REQUIRE(flightMapLookupIds(received, 1000) == std::set<std::string>{"ABC123"});
    REQUIRE(flightMapLookupIds(received, 2801).empty());
    REQUIRE(flightMapIcao("ABC123/other").empty());
    const auto external = parseFlightMapPositions(R"({"ac":[
        {"hex":"abc123","lat":-33.5,"lon":151.2,"alt_baro":32000,"seen_pos":2},
        {"hex":"ffffff","lat":10,"lon":20,"seen_pos":1}]})", {"ABC123"}, 1000);
    REQUIRE(external.size()==1 && external.count("ABC123"));
    rows=json_loads(flightMapJson(received,external,1000).c_str(),0,&error);
    REQUIRE(json_array_size(rows)==1);
    row=json_array_get(rows,0);
    REQUIRE(json_number_value(json_object_get(row,"lat"))==-33.5);
    REQUIRE(std::string(json_string_value(json_object_get(row,"positionSource")))=="ADSB.lol (online)");
    json_decref(rows);
    REQUIRE(flightMapJson({},external,1000)=="[]"); // late response after clear
    rows=json_loads(flightMapJson(received,external,1301).c_str(),0,&error);
    REQUIRE(!json_object_get(json_array_get(rows,0),"lat"));json_decref(rows);
    REQUIRE(parseFlightMapPositions(R"({"ac":[{"hex":"abc123","lat":91,"lon":20,"seen_pos":1}]})", {"ABC123"},1000).empty());
    REQUIRE(parseFlightMapPositions(R"({"ac":[{"hex":"abc123","lat":1,"lon":20,"seen_pos":301}]})", {"ABC123"},1000).empty());
    REQUIRE(parseFlightMapPositions(R"({"ac":[{"hex":"abc123","lat":"1","lon":20,"seen_pos":1}]})", {"ABC123"},1000).empty());
    bool rejected=false;
    try { parseFlightMapPositions("{broken", {"ABC123"},1000); } catch (...) { rejected=true; }
    REQUIRE(rejected);
    // Public fixture from vendored libacars/examples/adsc_get_position.c:
    // exercise the actual application decoder -> AircraftTable -> map JSON.
    const auto app=decodeAcarsApps("H1", "/BOMASAI.ADS.VT-ANB072501A070A988CA73248F0E5DC10200000F5EE1ABC000102B885E0A19F5", true);
    REQUIRE(app.decoded && app.hasPos);
    DecodedMessage message; message.aesId=0xABC123;message.icao="ABC123";
    message.hasPos=app.hasPos;message.lat=app.lat;message.lon=app.lon;message.alt=app.alt;
    table.update(message,1001);
    REQUIRE(flightMapLookupIds(table.snapshot(),1001).empty());
    rows=json_loads(flightMapJson(table.snapshot(),external,1001).c_str(),0,&error);
    row=json_array_get(rows,0);
    REQUIRE(json_number_value(json_object_get(row,"lat"))==app.lat);
    REQUIRE(std::string(json_string_value(json_object_get(row,"positionSource")))=="Decoded ADS-C");
    json_decref(rows);
    table.setIcao(0x123456,"123456",1001);
    const auto second=parseFlightMapPositions(R"({"ac":[{"hex":"123456","lat":-33.5,"lon":151.2,"alt_baro":32000,"seen_pos":2}]})",{"123456"},1001);
    std::ofstream fixture(argc==2 ? argv[1] : "flight-map-fixture.json");
    fixture << flightMapJson(table.snapshot(),second,1001); REQUIRE(fixture.good());
    table.clear(); REQUIRE(flightMapJson(table.snapshot(),external,1001)=="[]");
    std::cout << "PASS: ADS-C decode to aircraft table to map; identity-only online fallback; received-ID allowlist; stale/invalid responses; decoded precedence; clear; A/B merge; safe JSON\n";
}
