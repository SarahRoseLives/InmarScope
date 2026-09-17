// Read-only discovery probe: does not tune or enable antenna power.
#include "sdr/sdrplay_source.h"
#include "version.h"
#include <SoapySDR/Modules.h>
#include <SoapySDR/Version.h>
#include <iostream>
int main() {
    std::cout << "InmarScope " << INMARSCOPE_VERSION << " | SoapySDR " << SoapySDR_getLibVersion() << '\n';
    SdrplaySource source;
    auto devices = source.listDevices();
    std::cout << "Soapy root: " << SoapySDR_getRootPath() << '\n';
    size_t count=0;
    char** modules=SoapySDR_listModules(&count);
    for(size_t i=0;i<count;++i) {
        char* version=SoapySDR_getModuleVersion(modules[i]);
        std::cout << "Module: " << modules[i] << " | " << (version?version:"") << '\n';
        SoapySDR_free(version);
        auto status=SoapySDR_getLoaderResult(modules[i]);
        for(size_t j=0;j<status.size;++j)
            std::cout << "  " << status.keys[j] << ": " << (*status.vals[j]?status.vals[j]:"loaded") << '\n';
        SoapySDRKwargs_clear(&status);
    }
    SoapySDRStrings_clear(&modules,count);
    for (const auto& d : devices) std::cout << d.serial << " " << d.name << '\n';
    if (devices.empty()) { std::cerr << source.error() << '\n'; return 1; }
    return 0;
}
