#include "decode/band_plan.h"
#include <filesystem>
#include <fstream>
#include <chrono>
#include <iostream>
#define REQUIRE(x) do { if(!(x)) { std::cerr << "FAILED line " << __LINE__ << ": " #x << '\n'; return 1; } } while(0)
int main(int argc, char** argv) {
    REQUIRE(argc == 2);
    std::vector<std::string> names, paths, errors;
    scanBandPlans(argv[1], names, paths, &errors);
    for (const auto& error : errors) std::cerr << error << '\n';
    REQUIRE(errors.empty()); REQUIRE(paths.size() == 26);
    size_t entries = 0;
    for (const auto& path : paths) { auto bp=loadBandPlan(path); REQUIRE(bp.valid); entries+=bp.entries.size(); }
    REQUIRE(entries == 1738);
    const auto au=loadBandPlan((std::filesystem::path(argv[1]) / "national/australia.json").string());
    REQUIRE(au.valid && au.entries.front().loMHz == 0.1357 && au.entries.front().hiMHz == 0.1378);
    REQUIRE(bandPlanLabel(au).find("Australia") != std::string::npos);
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
    std::filesystem::remove(file);std::filesystem::remove(root/"region");std::filesystem::remove(root);
    std::cout << "PASS: all 26 bundled plans / 1738 entries; Hz-to-MHz conversion; recursive discovery; malformed data rejected atomically\n";
}
