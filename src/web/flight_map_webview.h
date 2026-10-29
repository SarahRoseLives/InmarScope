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
    bool onlinePositions = true;
    // Map source: 0 = all live traffic on globe.airplanes.live (default),
    //             1 = airplanes.live filtered to locally received aircraft,
    //             2 = local Leaflet page with decoded ADS-C positions.
    int  mode = 0;
    // Mode 1: ICAO hex IDs of received aircraft to show (call every frame).
    void setReceived(const std::vector<std::string>& icaoHex);
    // Live mode: jump to the aircraft whose voice call is being monitored.
    bool followVoice = true;
    void applyMode();                              // navigate if mode changed
    void showAircraft(const std::string& icaoHex); // live mode: focus this ICAO
    std::string shownIcao() const;
    std::string positionStatus() const;
    struct Impl;
    Impl* impl_ = nullptr;
};
