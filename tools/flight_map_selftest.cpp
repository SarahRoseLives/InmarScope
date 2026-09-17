#include "web/flight_map_data.h"
#include <jansson.h>
#include <iostream>
#include <limits>
#define REQUIRE(x) do { if(!(x)) { std::cerr << "FAILED line " << __LINE__ << std::endl; return 1; } } while(0)
int main() {
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
    std::cout << "PASS: local aircraft only, A/B deduplication, newest valid position, no-position handling, safe JSON\n";
}
