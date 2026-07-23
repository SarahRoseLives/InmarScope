# InmarScope

Multi-protocol L-band satellite communications decoder for Windows (and Linux if you compile it).

Decodes Inmarsat Aero (Classic Aero, Aero-H/H+) and Inmarsat-C/EGC signals in real time using RTL-SDR, HackRF, or SDR++ server sources. Voice call recording, aircraft tracking, and message output in one application.

[![Animation showing InmarScope decoding messages and displaying waterfall graphics](screenshot.gif)](https://sarahsforge.dev/products/inmarscope)

## Download

Windows builds can be found on official project page: **https://sarahsforge.dev/products/inmarscope**

## Features

- **Inmarsat Aero** — 600/1200/8400 bps OQPSK signal decoding, voice AMBE decoding, ACARS/ADS-C/CPDLC message parsing
- **Inmarsat-C / EGC** — 1200 bps BPSK decoding with EGC SafetyNet/FleetNet message output
- **Dual-SDR voice follow** — dedicate a second RTL-SDR to automatically follow and record voice calls
- **Voice call recording** — WAV and OGG Vorbis output, tagged with aircraft ICAO
- **Live spectrum & waterfall** — real-time FFT with drag-to-place decoder placement
- **Embedded flight map** — tracks aircraft positions on globe.airplanes.live via Microsoft Edge WebView2
- **SBS/BaseStation output** — TCP server on port 30003 for virtual radar clients
- **JAERO-compatible output** — JSON dump, text log, and UDP forwarding
- **Country blacklist** — mute and skip recording for selected countries using ICAO address lookup

## Building

InmarScope supports building for Windows (MSYS2 MINGW64) and Linux (Debian/Ubuntu, Arch, Fedora).

View build instructions at **[COMPILE.md](COMPILE.md)**.

## License

This software is licensed under the **GNU General Public License v3.0**, available to view at **[LICENSE](LICENSE)**.
