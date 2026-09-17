// JSON band plans: name/designator, optional regions/countries, and bands in MHz.
// See bandplans/README.md for the schema. Nested region/country folders are scanned.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct BandPlanEntry
{
    double loMHz;
    double hiMHz;
    std::string label;
    uint32_t color; // RGBA8 (A=0xFF)
    double frequencyMHz = 0.0; // explicit channel center; zero = allocation only
    std::string service;
};

struct BandPlan
{
    std::string name;
    std::string designator;
    std::string notes;
    double position = 0.0;      // orbital degrees (negative = west)
    std::string filePath;
    std::vector<BandPlanEntry> entries;
    std::vector<std::string> regions, countries;
    std::string error;
    bool valid = false;
};

// Parse a JSON file.  Returns a BandPlan with valid==false on error.
BandPlan loadBandPlan(const std::string& path);

// Recursively scan JSON plans and return stable, sorted display names.
// Each entry is "designator  —  Name" for use in a combo box.
// The companion vector 'paths' receives the full file path for each entry.
void scanBandPlans(const char* dir, std::vector<std::string>& names,
                   std::vector<std::string>& paths,
                   std::vector<std::string>* errors = nullptr);
std::string bandPlanLabel(const BandPlan& plan);

struct BandPlanGroup {
    std::string service;
    double loMHz, hiMHz, centerMHz;
    std::vector<size_t> channels;
};
// Partition channel services into windows that fit the receiver's sample rate.
// Broad allocations never become channel/tuning presets.
std::vector<BandPlanGroup> bandPlanGroups(const BandPlan& plan, double sampleRateHz);
