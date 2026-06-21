#include "decode/decoder.h"

#include "jaero_demod.h"
#include "voice/ambe_decoder.h"
#include "voice/wav_writer.h"
#include "audio/audio_output.h"
#include "decode/egc/egc_decoder.h"

#include <cstdio>
#include <ctime>
#include <filesystem>

// A voice call is considered finished when no AMBE frames arrive for this long.
static constexpr double kCallGapSec = 1.5;

// JAERO DSP config knob referenced by jaero_demod.cpp (0 = use the demod's
// built-in default locking bandwidth).
extern "C" {
double oqpsk_lockingbw = 0.0;
}

static double ddcRate(int baud)
{
    (void)baud;
    return 48000.0; // JAERO demods are tuned for ~48 kHz (target_output_rate)
}

static double ddcBw(int baud)
{
    // Signal bandwidth fb*(1+alpha): ~6 kHz for 600/1200 MSK, ~14 kHz for
    // 8400 OQPSK (alpha 0.6), ~21 kHz for 10500 OQPSK (alpha 1.0).
    if (baud == 10500)
        return 21000.0;
    if (baud == 8400)
        return 14000.0;
    return 6000.0;
}

// Human-readable name for a P-channel signal-unit type (first decoded byte).
static const char* suTypeName(uint8_t t)
{
    switch (t)
    {
    case 0x00: return "Reserved";
    case 0x01: return "Fill-in";
    case 0x05: return "Sys table: Psmc/Rsmc channels";
    case 0x07: return "Sys table: beam support";
    case 0x0A: return "Sys table: index";
    case 0x0C: return "Sys table: satellite ID";
    case 0x10: return "Log-on request";
    case 0x11: return "Log-on confirm";
    case 0x12: return "Log-off request";
    case 0x13: return "Log-on reject";
    case 0x14: return "Log-on interrogation";
    case 0x15: return "Log-on/off acknowledge";
    case 0x16: return "Log-on prompt";
    case 0x17: return "Data channel reassignment";
    case 0x21: return "Call announcement";
    case 0x28: return "Data EIRP table broadcast";
    case 0x30: return "Call progress";
    case 0x31: return "C-channel assign (distress)";
    case 0x32: return "C-channel assign (flight safety)";
    case 0x33: return "C-channel assign (other safety)";
    case 0x34: return "C-channel assign (non-safety)";
    case 0x40: return "P/R channel control";
    case 0x41: return "T channel control";
    case 0x51: return "T channel assignment";
    case 0x61: return "Request for acknowledgement";
    case 0x62: return "Acknowledge (RACK/TACK)";
    case 0x71: return "User data (ISU RLS)";
    case 0x74: return "User data (3-octet LSDU)";
    case 0x76: return "User data (4-octet LSDU)";
    default: return "Unknown SU";
    }
}

Decoder::Decoder(double subRate, double subCenterHz, double chanFreqHz, int baud,
                 int channelId, MessageLog* log, MessageLog* suLog, AudioOutput* audioSink,
                 CassignLog* cassignLog, ChannelTable* netTable, EgcLog* egcLog)
    : ddc_(subRate, chanFreqHz - subCenterHz, ddcRate(baud), ddcBw(baud)),
      log_(log),
      suLog_(suLog),
      cassignLog_(cassignLog),
      netTable_(netTable),
      subCenterHz_(subCenterHz),
      chanFreqHz_(chanFreqHz),
      baud_(baud),
      channelId_(channelId),
      audioSink_(audioSink),
      egcLog_(egcLog)
{
    if (baud == kEgcBaud)
    {
        egc_ = std::make_unique<EgcDecoder>(channelId, chanFreqHz / 1e6, egcLog_);
    }
    else if (baud == 10500 || baud == 8400)
    {
        oqpsk_ = jaero_oqpsk_cont_create(ddc_.outputRate(), (double)baud, channelId,
                                         nullptr, nullptr);
        if (oqpsk_)
        {
            jaero_oqpsk_cont_set_acars2_callback(oqpsk_, &Decoder::acars2Trampoline, this);
            jaero_oqpsk_cont_set_decoded_callback(oqpsk_, &Decoder::decodedTrampoline, this);
            jaero_oqpsk_cont_set_cassign_callback(oqpsk_, &Decoder::cassignTrampoline, this);
        }
        if (baud == 8400)
        {
            ambe_ = std::make_unique<AmbeDecoder>();
            if (oqpsk_)
                jaero_oqpsk_cont_set_voice_callback(oqpsk_, &Decoder::voiceTrampoline, this);
        }
    }
    else
    {
        pmsk_ = jaero_pmsk_create(ddc_.outputRate(), (double)baud, channelId,
                                  nullptr, nullptr);
        if (pmsk_)
        {
            jaero_pmsk_set_acars2_callback(pmsk_, &Decoder::acars2Trampoline, this);
            jaero_pmsk_set_decoded_callback(pmsk_, &Decoder::decodedTrampoline, this);
            jaero_pmsk_set_cassign_callback(pmsk_, &Decoder::cassignTrampoline, this);
        }
    }
    ddcOut_.reserve(8192);
}

Decoder::~Decoder()
{
    if (rec_)
        rec_->close();
    if (pmsk_)
        jaero_pmsk_destroy(pmsk_);
    if (oqpsk_)
        jaero_oqpsk_cont_destroy(oqpsk_);
}

void Decoder::process(const double* iq, int nComplex)
{
    maintainRecording();
    ddcOut_.clear();
    ddc_.process(iq, nComplex, ddcOut_);
    if (ddcOut_.empty())
        return;
    int n = (int)(ddcOut_.size() / 2);
    if (egc_)
    {
        egc_->process(ddcOut_.data(), n);
        msgCount_.store(egc_->messageCount());
    }
    else if (oqpsk_)
        jaero_oqpsk_cont_feed_iq(oqpsk_, ddcOut_.data(), n);
    else if (pmsk_)
        jaero_pmsk_feed_iq(pmsk_, ddcOut_.data(), n);
}

void Decoder::setFreq(double chanFreqHz)
{
    chanFreqHz_ = chanFreqHz;
    ddc_.setOffset(chanFreqHz - subCenterHz_);
}

bool Decoder::locked() const
{
    if (egc_)
        return egc_->locked();
    if (oqpsk_)
        return jaero_oqpsk_cont_is_locked(oqpsk_);
    return pmsk_ && jaero_pmsk_is_locked(pmsk_);
}

double Decoder::ebno() const
{
    if (oqpsk_)
        return jaero_oqpsk_cont_get_ebno(oqpsk_);
    return pmsk_ ? jaero_pmsk_get_ebno(pmsk_) : 0.0;
}

double Decoder::mse() const
{
    if (oqpsk_)
        return jaero_oqpsk_cont_get_mse(oqpsk_);
    return pmsk_ ? jaero_pmsk_get_mse(pmsk_) : 0.0;
}

int Decoder::getConstellation(double* iqOut, int maxPairs) const
{
    if (egc_)
        return egc_->getConstellation(iqOut, maxPairs);
    if (oqpsk_)
        return jaero_oqpsk_cont_get_constellation(oqpsk_, iqOut, maxPairs);
    if (pmsk_)
        return jaero_pmsk_get_constellation(pmsk_, iqOut, maxPairs);
    return 0;
}

int Decoder::egcBer() const { return egc_ ? egc_->lastBer() : -1; }
int Decoder::egcFrames() const { return egc_ ? egc_->framesSynced() : 0; }

void Decoder::acarsTrampoline(const uint8_t* data, int len, int, uint32_t aes_id,
                              uint8_t ges_id, uint8_t, uint8_t, int downlink,
                              void* user)
{
    static_cast<Decoder*>(user)->onAcars(data, len, aes_id, ges_id, downlink);
}

void Decoder::onAcars(const uint8_t* data, int len, uint32_t aes_id,
                      uint8_t ges_id, int downlink)
{
    DecodedMessage m;
    m.channelId = channelId_;
    m.freqMHz = chanFreqHz_ / 1e6;
    m.aesId = aes_id;
    m.gesId = ges_id;
    m.downlink = downlink;

    m.text.reserve(len);
    char hexbuf[8];
    for (int i = 0; i < len; ++i)
    {
        unsigned char c = data[i];
        m.text.push_back((c >= 0x20 && c < 0x7f) ? (char)c : '.');
        std::snprintf(hexbuf, sizeof(hexbuf), "%02X ", c);
        m.hex += hexbuf;
    }

    if (log_)
        log_->add(m);
    ++msgCount_;
}

void Decoder::acars2Trampoline(int, const jaero_acars_msg* msg, void* user)
{
    static_cast<Decoder*>(user)->onAcars2(msg);
}

void Decoder::onAcars2(const jaero_acars_msg* msg)
{
    DecodedMessage m;
    m.channelId = channelId_;
    m.freqMHz = chanFreqHz_ / 1e6;
    m.aesId = msg->aes_id;
    m.gesId = msg->ges_id;
    m.downlink = msg->downlink;
    m.mode = msg->mode;
    m.blockId = msg->bi;

    // Registration: strip leading/trailing padding ('.', space).
    std::string reg = msg->reg;
    size_t a = reg.find_first_not_of(". ");
    size_t b = reg.find_last_not_of(". ");
    if (a != std::string::npos)
        m.reg = reg.substr(a, b - a + 1);
    m.label = msg->label;

    // Clean text: keep printable, fold CR/LF to spaces, trim, hex alongside.
    std::string t;
    char hexbuf[8];
    for (int i = 0; i < msg->text_len; ++i)
    {
        unsigned char c = msg->text[i];
        if (c == '\r' || c == '\n' || c == '\t')
            t.push_back(' ');
        else if (c >= 0x20 && c < 0x7f)
            t.push_back((char)c);
        std::snprintf(hexbuf, sizeof(hexbuf), "%02X ", c);
        m.hex += hexbuf;
    }
    size_t e = t.find_last_not_of(' ');
    size_t s = t.find_first_not_of(' ');
    if (s != std::string::npos)
        t = t.substr(s, e - s + 1);
    m.text = t;

    if (log_)
        log_->add(m);
    ++msgCount_;
}

void Decoder::decodedTrampoline(const uint8_t* data, int len, int, void* user)
{
    static_cast<Decoder*>(user)->onDecoded(data, len);
}

void Decoder::onDecoded(const uint8_t* data, int len)
{
    if (!suLog_ || len <= 0)
        return;
    DecodedMessage m;
    m.channelId = channelId_;
    m.freqMHz = chanFreqHz_ / 1e6;

    // First decoded byte is the P-channel SU type descriptor.
    m.text = suTypeName(data[0]);

    char hexbuf[8];
    for (int i = 0; i < len; ++i)
    {
        std::snprintf(hexbuf, sizeof(hexbuf), "%02X ", data[i]);
        m.hex += hexbuf;
    }
    suLog_->add(m);

    // System-table SUs broadcast the network channel plan -> identify channels.
    if (netTable_ && len >= 10)
    {
        if (data[0] == 0x05) // GES Psmc/Rsmc channels
        {
            int b3 = data[2];
            uint8_t ges = data[3];
            int ch1 = (data[4] << 8) | data[5];
            int ch2 = (data[6] << 8) | data[7];
            int ch3 = (data[8] << 8) | data[9];
            double f1 = ch1 * 0.0025 + 1510.0;
            double f2 = ch2 * 0.0025 + 1510.0;
            double f3 = ch3 * 0.0025 + 1510.0;
            int lsu = b3 & 0x03;
            if (lsu <= 1)
            {
                f2 += 101.5;
                f3 += 101.5;
                netTable_->addChannel(f1, "Psmc (P-ch RX)", ges, true, 1200);
                netTable_->addChannel(f2, "Rsmc0 (R-ch TX)", ges, false, 0);
                netTable_->addChannel(f3, "Rsmc1 (R-ch TX)", ges, false, 0);
            }
            else if (lsu == 2)
            {
                f1 += 101.5; f2 += 101.5; f3 += 101.5;
                netTable_->addChannel(f1, "Rsmc2 (R-ch TX)", ges, false, 0);
                netTable_->addChannel(f2, "Rsmc3 (R-ch TX)", ges, false, 0);
                netTable_->addChannel(f3, "Rsmc4 (R-ch TX)", ges, false, 0);
            }
            else
            {
                f1 += 101.5; f2 += 101.5; f3 += 101.5;
                netTable_->addChannel(f1, "Rsmc5 (R-ch TX)", ges, false, 0);
                netTable_->addChannel(f2, "Rsmc6 (R-ch TX)", ges, false, 0);
                netTable_->addChannel(f3, "Rsmc7 (R-ch TX)", ges, false, 0);
            }
        }
        else if (data[0] == 0x0C) // satellite identification + CAC/Psmc
        {
            int b3 = data[2], b4 = data[3], b6 = data[5];
            double lon = b6 * 1.5;
            int b7 = data[6], b8 = data[7], b9 = data[8], b10 = data[9];
            int ch1 = ((b7 & 0x7F) << 8) | b8;
            int ch2 = ((b9 & 0x7F) << 8) | b10;
            double cac1 = ch1 * 0.0025 + 1510.0;
            double cac2 = ch2 * 0.0025 + 1510.0;
            int satid = ((b3 << 4) & 0x30) | ((b4 >> 4) & 0x0F);
            char lonbuf[24];
            if (lon > 180.0)
                std::snprintf(lonbuf, sizeof(lonbuf), "%.1fW", 360.0 - lon);
            else
                std::snprintf(lonbuf, sizeof(lonbuf), "%.1fE", lon);
            netTable_->setSatellite(satid, lonbuf);
            netTable_->addChannel(cac1, "CAC/Psmc1 (P-ch RX)", 0, true, 1200);
            if (ch2)
                netTable_->addChannel(cac2, "CAC/Psmc2 (P-ch RX)", 0, true, 1200);
        }
    }
}

void Decoder::cassignTrampoline(int, uint8_t type, uint32_t aes_id, uint8_t ges_id,
                                double rx_mhz, double tx_mhz, void* user)
{
    static_cast<Decoder*>(user)->onCassign(type, aes_id, ges_id, rx_mhz, tx_mhz);
}

void Decoder::onCassign(uint8_t type, uint32_t aes_id, uint8_t ges_id,
                        double rx_mhz, double tx_mhz)
{
    if (!cassignLog_)
        return;
    CassignEntry e;
    e.channelId = channelId_;
    e.type = type;
    e.aesId = aes_id;
    e.gesId = ges_id;
    e.rxMHz = rx_mhz;
    e.txMHz = tx_mhz;
    cassignLog_->add(e);
}

void Decoder::voiceTrampoline(const uint8_t* frame, int len, int, void* user)
{
    static_cast<Decoder*>(user)->onVoice(frame, len);
}

void Decoder::onVoice(const uint8_t* frame, int len)
{
    if (!ambe_ || len != AmbeDecoder::kFrameBytes)
        return;
    int16_t pcm[AmbeDecoder::kPcmSamples];
    ambe_->decode(frame, pcm);
    if (record_.load())
        recordPcm(pcm, AmbeDecoder::kPcmSamples);
    if (monitored_.load() && audioSink_)
        audioSink_->push(pcm, AmbeDecoder::kPcmSamples);
}

void Decoder::setRecording(bool on, const std::string& dir)
{
    if (on && !dir.empty())
        recordDir_ = dir;
    record_.store(on);
}

// Close the current call's file if recording was turned off or the call has
// gone idle. Runs on the worker thread (from process()).
void Decoder::maintainRecording()
{
    if (!recActive_.load())
        return;
    bool idle = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - lastVoiceTime_)
                    .count() > kCallGapSec;
    if (!record_.load() || idle)
    {
        if (rec_)
            rec_->close();
        recActive_.store(false);
    }
}

void Decoder::recordPcm(const int16_t* pcm, int n)
{
    lastVoiceTime_ = std::chrono::steady_clock::now();
    if (!recActive_.load())
    {
        // Start a new call file: <dir>/<UTC time>_<freq>MHz_ch<id>.wav
        std::error_code ec;
        std::filesystem::create_directories(recordDir_, ec);
        std::time_t t = std::time(nullptr);
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char ts[32];
        std::strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tm);
        char name[256];
        std::snprintf(name, sizeof(name), "%s/%s_%.4fMHz_ch%d.wav",
                      recordDir_.c_str(), ts, chanFreqHz_ / 1e6, channelId_);
        if (!rec_)
            rec_ = std::make_unique<WavWriter>();
        if (!rec_->open(name, 8000, 1))
            return; // couldn't open: stay inactive, try again next frame
        recActive_.store(true);
    }
    if (rec_)
        rec_->write(pcm, n);
}
