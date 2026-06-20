#include "decode/decoder.h"

#include "jaero_demod.h"
#include "voice/ambe_decoder.h"
#include "audio/audio_output.h"

#include <cstdio>

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
                 int channelId, MessageLog* log, MessageLog* suLog, AudioOutput* audioSink)
    : ddc_(subRate, chanFreqHz - subCenterHz, ddcRate(baud), ddcBw(baud)),
      log_(log),
      suLog_(suLog),
      subCenterHz_(subCenterHz),
      chanFreqHz_(chanFreqHz),
      baud_(baud),
      channelId_(channelId),
      audioSink_(audioSink)
{
    if (baud == 10500 || baud == 8400)
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
    if (pmsk_)
        jaero_pmsk_destroy(pmsk_);
    if (oqpsk_)
        jaero_oqpsk_cont_destroy(oqpsk_);
}

void Decoder::process(const double* iq, int nComplex)
{
    ddcOut_.clear();
    ddc_.process(iq, nComplex, ddcOut_);
    if (ddcOut_.empty())
        return;
    int n = (int)(ddcOut_.size() / 2);
    if (oqpsk_)
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
    if (oqpsk_)
        return jaero_oqpsk_cont_get_constellation(oqpsk_, iqOut, maxPairs);
    if (pmsk_)
        return jaero_pmsk_get_constellation(pmsk_, iqOut, maxPairs);
    return 0;
}

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
}

void Decoder::cassignTrampoline(int, uint8_t type, uint32_t aes_id, uint8_t ges_id,
                                double rx_mhz, double tx_mhz, void* user)
{
    static_cast<Decoder*>(user)->onCassign(type, aes_id, ges_id, rx_mhz, tx_mhz);
}

void Decoder::onCassign(uint8_t type, uint32_t aes_id, uint8_t ges_id,
                        double rx_mhz, double tx_mhz)
{
    if (!suLog_)
        return;
    DecodedMessage m;
    m.channelId = channelId_;
    m.freqMHz = chanFreqHz_ / 1e6;
    m.aesId = aes_id;
    m.gesId = ges_id;
    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "C-CHANNEL ASSIGN type=0x%02X  AES=%06X GES=%02X  RX=%.4f MHz  TX=%.4f MHz",
                  type, aes_id, ges_id, rx_mhz, tx_mhz);
    m.text = buf;
    suLog_->add(m);
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
    if (monitored_.load() && audioSink_)
        audioSink_->push(pcm, AmbeDecoder::kPcmSamples);
}
