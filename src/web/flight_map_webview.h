// Embedded flight-map browser using Microsoft Edge WebView2.
// COM must be initialised as STA on the main thread before GLFW (see main.cpp).
#pragma once
#include <string>
#include <vector>
struct AircraftEntry;

class FlightMapWebView {
public:
    FlightMapWebView() = default;
    ~FlightMapWebView();
    FlightMapWebView(const FlightMapWebView&) = delete;
    FlightMapWebView& operator=(const FlightMapWebView&) = delete;
    void init(void* nativeHwnd);
    void updateAircraft(const std::vector<AircraftEntry>& aircraft);
    void setBounds(int x, int y, int w, int h, bool visible = true);
    bool isReady() const;
    struct Impl;
    Impl* impl_ = nullptr;
};
