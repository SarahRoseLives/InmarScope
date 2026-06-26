// InmarScope shared application state — App struct, spectrum state, and constants.
#pragma once

#include "dsp/iq_ring.h"
#include "dsp/jfft.h"
#include "gui/waterfall.h"
#include "sdr/rtl_sdr_source.h"
#include "sdr/hackrf_source.h"
#include "sdr/wav_file_source.h"
#include "sdr/sdrpp_server_source.h"
#include "sdr/iq_recorder.h"
#include "decode/band_plan.h"
#include "decode/decoder_manager.h"
#include "output/message_feed.h"
#include "update/version_check.h"
#if defined(_WIN32)
#include "web/flight_map_webview.h"
#endif

#include <chrono>
#include <string>
#include <utility>
#include <vector>

struct SpectrumView
{
    IqRing ring{1u << 21};
    JFFT   fft;
    Waterfall waterfall;
    std::vector<std::complex<double>> iq;
    std::vector<double> window;
    std::vector<float> inst;
    std::vector<float> avg;
    std::vector<float> sortbuf;
    std::vector<float> freqMHz;
    int   curN = 0;
    float rmsDbfs = -120.0f;
    float frameDbMin = 0.0f, frameDbMax = -120.0f;
    double viewXminMHz = 0.0, viewXmaxMHz = 0.0;
    bool   resetView = true;
    float  specLeftInset = 0.0f, specRightInset = 0.0f;
};

struct CallHunterCand
{
    double freqMHz = 0.0;
    double peakDB = -999.0;
    int    confirmCount = 0;
    int    lostCount = 0;
    int    channelId = -1;
    bool   matched = false;
};

struct App
{
    RtlSdrSource    sdr;
    WavFileSource   wav;
    SdrppServerSource server;
    HackRfSource    hack;
    SdrSource*      active = &sdr;
    int  sourceMode = 0; // 0 = RTL-SDR, 1 = WAV, 2 = SDR++ Server, 3 = HackRF
    char wavPath[512] = "";
    bool wavLoop = true;
    char serverHost[128] = "localhost";
    int  serverPort = 5259;
    bool serverCompression = true;
    int  serverSampleType = 1;
    double serverSampleRateMHz = 2.0;

    // HackRF
    double hackSampleRateMHz = 10.0;
    int    hackLna = 16;
    int    hackVga = 16;
    bool   hackAmp = false;
    bool   hackBias = false;

    SpectrumView     viewA;
    SpectrumView     viewB;
    DecoderManager   decoders;
    DecoderManager   decodersB;
    RtlSdrSource     sdrB;

    // Dual-SDR
    bool   voiceSdrEnabled = false;
    bool   dualMode = false;
    int    deviceIndexB = 1;
    double voiceCenterMHz = 1545.0;
    int    sampleRateIdxB = 0;
    bool   autoGainB = false;
    float  gainDbB = 40.0f;
    bool   biasTeeB = false;
    float  ppmB = 0.0f;

    int  newBaud = 1;
    bool placingDecoder = false;
    bool placingVoiceView = false; // true = started on voice SDR, false = primary
    double placingFreqMHz = 0.0;
    int  selectedDecoder = -1;
    std::vector<float> constBuf;
    double constLim = 1.0;
    std::chrono::steady_clock::time_point constLimTime;

    // Recording
    bool recordVoice = false;
    int  recordFormat = 0; // 0=WAV, 1=OGG
    char recordDir[256] = "recordings";
    bool saveDecoders = false; // save non-8400 decoders in INI for restart
    std::vector<std::pair<double,int>> savedDecoders; // freqMHz, baud

    // Country blacklist — voice calls from these 2-letter country codes
    // will not be monitored (they still record if recording is on).
    std::vector<std::string> blacklistCountries;

    // IQ recorder
    IqRecorder iqRecorder;
    char iqRecPath[512] = "iq_record.wav";

    int  audioDevice = 0;
    bool voiceMuted = false;
    std::vector<std::string> audioDevs;

    // Output
    MessageFeed feed;
    VersionCheck verCheck;
    #if defined(_WIN32)
    FlightMapWebView flightMapWv;
    #endif
    uint64_t lastAcarsFed = 0, lastEgcFed = 0, lastLesFed = 0;
    bool   outFile = false;
    char   outFilePath[512] = "messages.jsonl";
    bool   outUdp = false;
    char   outUdpHost[128] = "127.0.0.1";
    int    outUdpPort = 5556;
    int    outFormat = 0;
    char   outStation[64] = "";
    bool   outSbs = false;
    char   outSbsHost[128] = "127.0.0.1";
    int    outSbsPort = 30003;

    // Voice follow
    bool   voiceFollow = false;
    float  followHoldSec = 6.0f;
    bool   following = false;
    bool   followRetuned = false;
    bool   followEverLocked = false;
    int    followChannelId = -1;
    double followHomeMHz = 0.0;
    uint64_t followSeenCount = 0;
    uint64_t followVoiceFrames = 0;
    std::vector<std::pair<double, int>> followHome;
    std::chrono::steady_clock::time_point followActivity;

    std::vector<SdrDeviceInfo> devices;
    int deviceIndex = 0;

    double centerFreqMHz = 1545.0;
    int    sampleRateIdx = 9;
    bool   autoGain = false;
    float  gainDb = 40.0f;
    bool   biasTee = false;
    float  ppm = 0.0f;

    int   fftSizeIdx = 2;
    float avgAlpha = 0.6f;
    float dbMin = -80.0f;
    float dbMax = 0.0f;
    bool  dcBlock = true;
    bool  autoScale = true;

    std::string status = "Idle";

    bool   bandBrowse = true;
    std::chrono::steady_clock::time_point lastRetune;
    double lastRetuneCtr = 0.0;
    float  browseEdgePct = 24.5f;
    float  browseThrottleMs = 20.0f;
    float  browseMinMovePct = 0.10f;
    bool   acPosOnly = false;
    bool   showEmptyMsgs = false;
    bool   showBandPlan = false;
    std::vector<std::string> bandPlanNames;  // display names
    std::vector<std::string> bandPlanPaths;  // full file paths
    int    bandPlanIdx = 0;
    BandPlan bandPlanLoaded;
    char   bandPlanDir[256] = "bandplans";

    // CallHunter
    bool  callHunterMode = false;
    float callHunterThreshDB = 2.0f;
    int   callHunterConfirm = 10;
    int   callHunterLost = 30;
    std::vector<CallHunterCand> callHunterCands;
    std::vector<float> callHunterBaseline;
    int    callHunterWarmup = 0;
    double callHunterLastCenter = 0.0;

    int  layoutVersion = 0;
    bool forceDefaultLayout = false;

    double lastConfiguredFs = 0.0;
};

// ---- shared constants ----
constexpr double kRates[] = {
    0.25e6, 0.9e6, 1.024e6, 1.2e6, 1.4e6, 1.536e6,
    1.8e6, 1.92e6, 2.048e6, 2.4e6, 2.56e6, 2.88e6, 3.2e6};
constexpr const char* kRateLabels[] = {
    "0.25", "0.9", "1.024", "1.2", "1.4", "1.536",
    "1.8", "1.92", "2.048", "2.4", "2.56", "2.88", "3.2"};
constexpr int kNumRates = (int)(sizeof(kRates) / sizeof(kRates[0]));

constexpr int    kFftSizes[] = {1024, 2048, 4096, 8192, 16384, 32768, 65536};
constexpr const char* kFftLabels[] = {"1024", "2048", "4096", "8192", "16384", "32768", "65536"};
constexpr int kNumFftSizes = (int)(sizeof(kFftSizes) / sizeof(kFftSizes[0]));

// Dock layout version: bump when the built-in default layout changes.
constexpr int kLayoutVersion = 7;
