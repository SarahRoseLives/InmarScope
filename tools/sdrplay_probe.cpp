// Read-only discovery probe: does not tune or enable antenna power.
#include "sdr/sdrplay_source.h"
#include <iostream>
int main() {
    SdrplaySource source;
    auto devices = source.listDevices();
    for (const auto& d : devices) std::cout << d.serial << " " << d.name << '\n';
    if (devices.empty()) { std::cerr << source.error() << '\n'; return 1; }
    return 0;
}
