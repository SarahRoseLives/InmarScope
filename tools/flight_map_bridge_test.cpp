// Manual native WebView2 integration harness. It exercises the production bridge
// without RF hardware; test-only synthetic aircraft never enter the application.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include "web/flight_map_webview.h"
#include "web/flight_map_data.h"
#include <chrono>
#include <ctime>
#include <iostream>
int main() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    HWND hwnd = CreateWindowExW(0, L"STATIC", L"InmarScope map bridge test", WS_OVERLAPPEDWINDOW,
                                0, 0, 1000, 700, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) return 1;
    {
        FlightMapWebView view;
        view.init(hwnd);
        AircraftTable table;
        DecodedMessage decoded;decoded.aesId=1;decoded.icao="000001";
        decoded.hasPos=true;decoded.lat=-33;decoded.lon=151;decoded.alt=32000;
        table.update(decoded, static_cast<double>(std::time(nullptr)));
        table.setIcao(0xABC123,"ABC123",static_cast<double>(std::time(nullptr)));
        const auto start=std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now()-start < std::chrono::seconds(25)) {
            MSG message;
            while (PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&message);DispatchMessageW(&message); }
            view.updateAircraft(table.snapshot());
            view.setBounds(0,0,960,640,true);
            Sleep(25);
        }
        std::cout << view.positionStatus() << '\n';
        if (!view.isReady()) return 2;
    }
    DestroyWindow(hwnd);CoUninitialize();return 0;
}
