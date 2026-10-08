// Embedded flight-map browser using Microsoft Edge WebView2.
// COM must be initialised as STA on the main thread before GLFW (see main.cpp).
#pragma once
#include <cstdint>
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
    // Centre the map on one received aircraft (AES ID). Explicit user action.
    void focusAircraft(uint32_t aesId);
    void setBounds(int x, int y, int w, int h, bool visible = true);
    bool isReady() const;
    bool onlinePositions = true;
    // Opt-in: focus the monitored voice call's aircraft once when it appears.
    bool followVoice = false;
    uint32_t followAes = 0; // set by the GUI each frame; 0 = no call monitored
    std::string positionStatus() const;
    struct Impl;
    Impl* impl_ = nullptr;
};
