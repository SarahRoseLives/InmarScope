// Shared function declarations — everything that was originally in main.cpp.
// Definitions live in signal/processing.cpp, voice/voice_ops.cpp, gui/gui_panels.cpp,
// session/session.cpp, and state/config.cpp.
#pragma once

#include "core/app.h"

// signal/processing.cpp
void buildWindow(SpectrumView&, int N, float initDb);
void updateFreqAxis(SpectrumView&, double fc, double fs, int N);
void patchDcBins(std::vector<float>& a, int N, int w);
void processFft(SpectrumView&, App&, double fc, double fs);
void updateRateChange(App&);

// voice/voice_ops.cpp
bool isVoiceAssign(uint8_t type);
void retuneActive(App&, double centerMHz);
void retunePreserving(App&, double centerMHz);
void updateVoiceFollow(App&);
void tuneToVoice(App&, double rxMHz, uint32_t aesId);
void updateCallHunter(App&);

// session/session.cpp
void updateFeed(App&);
void startActive(App&);

// gui/gui_panels.cpp
void drawDockHost(App&);
void drawControls(App&);
void drawSpectrum(App&, SpectrumView&, DecoderManager&, const char*, bool, bool);
void drawWaterfall(App&, SpectrumView&, const char*);
void drawDecoders(App&);
void drawSUs(App&);
void drawMessages(App&);
void drawCChannel(App&);
void drawNetwork(App&);
void drawEgc(App&);
void drawMes(App&);
void drawLes(App&);
void drawAircraft(App&);
void drawFlightMap(App&);
void drawConstellation(App&);
void drawVoiceCalls(App&);
void drawLesFreq(App&);
void drawAbout(App&);

// state/config.cpp
void cfgWriteAll(App&, struct ImGuiTextBuffer*);
void cfgReadLine(App&, const char*);
void cfgRegisterHandler(App&);

// main.cpp
bool openWavDialog(char* out, int outLen);
