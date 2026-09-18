#include "decode/band_plan.h"
#include <filesystem>
#include <fstream>
#include <chrono>
#include <iostream>
#include <map>
#include <cmath>
#include <set>
#define REQUIRE(x) do { if(!(x)) { std::cerr << "FAILED line " << __LINE__ << ": " #x << '\n'; return 1; } } while(0)
int main(int argc, char** argv) {
    REQUIRE(argc == 2);
    std::vector<std::string> names, paths, errors;
    scanBandPlans(argv[1], names, paths, &errors);
    for (const auto& error : errors) std::cerr << error << '\n';
    REQUIRE(errors.empty()); REQUIRE(paths.size() == 27);
    size_t entries = 0;
    std::map<std::string, BandPlan> satellites;
    for (const auto& path : paths) {
        auto bp=loadBandPlan(path); REQUIRE(bp.valid); entries+=bp.entries.size();
        if (path.find("/satellite/") != std::string::npos) {
            REQUIRE(satellites.emplace(bp.designator, bp).second);
            REQUIRE(!bp.notes.empty());
        }
    }
    REQUIRE(entries == 1771);
    REQUIRE(satellites.size() == 6);
    for (const auto& expected : std::map<std::string, double>{{"I4A",25},{"4F2",143.5},{"3F5",-54},{"4F3",-98},{"6F1",83.5}}) {
        REQUIRE(satellites.count(expected.first));
        REQUIRE(satellites.at(expected.first).position == expected.second);
        REQUIRE(bandPlanLabel(satellites.at(expected.first)).find(expected.first) != std::string::npos);
    }
    REQUIRE(satellites.at("4F1").name.find("historical") != std::string::npos);
    REQUIRE(satellites.at("4F2").entries.size() == 33);
    REQUIRE(satellites.at("4F2").entries.front().frequencyMHz == 1541.45);
    for (const auto& item : satellites) {
        for (double rate : {62500., 96000., 125000., 192000., 250000., 384000., 500000., 768000., 1000000., 2000000., 6000000., 8000000., 10000000.}) {
            const auto groups = bandPlanGroups(item.second, rate);
            REQUIRE(!groups.empty() && groups.front().service == "Aero data");
            std::set<size_t> seen;
            for (const auto& group : groups) {
                REQUIRE(group.hiMHz - group.loMHz <= rate / 1e6 * .8 + 1e-9);
                for (size_t channel : group.channels) {
                    REQUIRE(seen.insert(channel).second);
                    const auto& entry = item.second.entries[channel];
                    REQUIRE(entry.baud == 1 || entry.baud == 600 || entry.baud == 1200 || entry.baud == 8400 || entry.baud == 10500);
                    REQUIRE((entry.service == "STD-C") == (entry.baud == 1));
                    REQUIRE((entry.service == "Aero voice") == (entry.baud == 8400));
                    REQUIRE(std::abs(entry.frequencyMHz-group.centerMHz) > 1e-7);
                    REQUIRE(std::abs(item.second.entries[channel].frequencyMHz - group.centerMHz) < rate / 2e6);
                }
            }
            REQUIRE(seen.size() == item.second.entries.size());
        }
    }
    const auto au=loadBandPlan((std::filesystem::path(argv[1]) / "national/australia.json").string());
    REQUIRE(au.valid && au.entries.front().loMHz == 0.1357 && au.entries.front().hiMHz == 0.1378);
    REQUIRE(bandPlanLabel(au).find("Australia") != std::string::npos);
    REQUIRE(bandPlanGroups(au, 2000000).empty()); // allocations are not channels
    const auto root=std::filesystem::temp_directory_path() / ("inmarscope-bandplan-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "region");
    const auto file=root / "region/test.JSON";
    auto write=[&](const std::string& value) { std::ofstream(file) << value; };
    write(R"({"name":"Test","regions":["Region"],"countries":["Country"],"bands":[{"lo":1,"hi":2,"label":"Valid","color":"#123456"}]})");
    auto bp=loadBandPlan(file.string()); REQUIRE(bp.valid && bp.entries[0].color==0xFF563412u);
    scanBandPlans(root.string().c_str(), names, paths, &errors); REQUIRE(paths.size()==1 && errors.empty());
    write(R"({"name":"Test","bands":[{"lo":1,"hi":2,"label":"Valid"},{"lo":4,"hi":3,"label":"Reversed"}]})");
    bp=loadBandPlan(file.string()); REQUIRE(!bp.valid && bp.entries.empty() && !bp.error.empty());
    scanBandPlans(root.string().c_str(), names, paths, &errors); REQUIRE(paths.empty() && errors.size()==1);
    write(R"({"name":"Test","bands":[{"lo":"1","hi":2,"label":"Invalid type"}]})");REQUIRE(!loadBandPlan(file.string()).valid);
    write(R"({"name":"Test","bands":[{"lo":1,"hi":2,"label":"Invalid color","color":"ZZZZZZ"}]})");REQUIRE(!loadBandPlan(file.string()).valid);
    write("{broken");REQUIRE(!loadBandPlan(file.string()).valid);
    write(R"({"name":"Test","bands":[{"lo":1,"hi":2,"frequency":3,"label":"Out of range"}]})");
    REQUIRE(!loadBandPlan(file.string()).valid);
    write(R"({"name":"Test","bands":[{"lo":1,"hi":2,"frequency":1.5,"baud":9600,"label":"Unsupported baud"}]})");
    REQUIRE(!loadBandPlan(file.string()).valid);
    write(R"({"name":"Test","bands":[{"lo":1,"hi":2,"frequency":1.5,"baud":1200,"decoder":"egc","label":"Ambiguous mode"}]})");
    REQUIRE(!loadBandPlan(file.string()).valid);
    std::filesystem::remove(file);std::filesystem::remove(root/"region");std::filesystem::remove(root);
    std::cout << "PASS: 27 plans / 1771 entries; channel groups fit 62.5 kHz to 10 MHz without missing/duplicating channels; allocations never auto-tune; malformed data rejected\n";
}
