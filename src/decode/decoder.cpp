#include "decode/decoder.h"

#include "jaero_demod.h"
#include "voice/ambe_decoder.h"
#include "voice/wav_writer.h"
#include "audio/audio_output.h"
#include "decode/egc/egc_decoder.h"
#include "decode/acars_apps.h"
#include "util/log.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>


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
                 CassignLog* cassignLog, ChannelTable* netTable, EgcLog* egcLog,
                 AircraftTable* acTable, MesLog* mesLog, LesLog* lesLog,
                 LesFreqTable* lesFreqTable, VoiceCallLog* voiceCallLog)
    : ddc_(subRate, chanFreqHz - subCenterHz, ddcRate(baud), ddcBw(baud)),
      log_(log),
      suLog_(suLog),
      cassignLog_(cassignLog),
      netTable_(netTable),
      acTable_(acTable),
      subCenterHz_(subCenterHz),
      chanFreqHz_(chanFreqHz),
      baud_(baud),
      channelId_(channelId),
      audioSink_(audioSink),
      egcLog_(egcLog),
      voiceCallLog_(voiceCallLog)
{
    if (baud == kEgcBaud)
    {
            egc_ = std::make_unique<EgcDecoder>(channelId, chanFreqHz / 1e6, ddc_.outputRate(), egcLog_, mesLog, lesLog, lesFreqTable);
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

void Decoder::setCpuReduce(bool on)
{
    if (pmsk_)
        jaero_pmsk_set_cpu_reduce(pmsk_, on ? 1 : 0);
    if (oqpsk_)
        jaero_oqpsk_cont_set_cpu_reduce(oqpsk_, on ? 1 : 0);
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
int Decoder::egcChannelType() const { return egc_ ? egc_->channelType() : 0; }

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
        log_->addAndStore(m, baud_);
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

    // Decode the embedded application (CPDLC / ADS-C / MIAM ...) via libacars.
    // Use the raw text bytes (not the space-folded copy) so encoded payloads parse.
    if (msg->text_len > 0 && msg->label[0] != '\0')
    {
        std::string raw(reinterpret_cast<const char*>(msg->text), (size_t)msg->text_len);
        AcarsAppResult app = decodeAcarsApps(m.label, raw, m.downlink != 0);
        if (app.decoded)
        {
            m.decoded = app.text;
            m.hasPos = app.hasPos;
            m.lat = app.lat;
            m.lon = app.lon;
            m.alt = app.alt;
            m.icao = app.icaoHex;
            m.flight = app.flightId;
        }
    }

    if (log_)
        log_->addAndStore(m, baud_);
    if (acTable_)
        acTable_->update(m, (double)std::time(nullptr));
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
    // Fill-in SUs (0x01) are just GES padding — skip them entirely.
    if (data[0] == 0x01)
        return;
    DecodedMessage m;
    m.channelId = channelId_;
    m.freqMHz = chanFreqHz_ / 1e6;
    m.suType = data[0];

    // First decoded byte is the P-channel SU type descriptor.
    m.text = suTypeName(data[0]);

    char hexbuf[8];
    for (int i = 0; i < len; ++i)
    {
        std::snprintf(hexbuf, sizeof(hexbuf), "%02X ", data[i]);
        m.hex += hexbuf;
    }

    // System-table SUs broadcast the network channel plan -> identify channels.
    // Decode happens before suLog_->add so we can annotate the SU type text.
    if (netTable_ && len >= 10)
    {
        if (data[0] == 0x07) // Beam support
        {
            logWrite("SU 0x07 hex=%s", m.hex.c_str());
        }
        else if (data[0] == 0x30 && len >= 8) // Call progress
        {
            uint32_t aes = ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) | data[3];
            m.aesId = aes;
            // For C-channel (8400) decoders the Call_progress SU carries the
            // aircraft's 24‑bit identity (ICAO / AES) — store it so voice
            // recordings get tagged with the correct hex.
            if (baud_ == 8400)
            {
                voiceAesId_ = aes;
                char icaoBuf[8];
                std::snprintf(icaoBuf, sizeof(icaoBuf), "%06X", aes);
                if (acTable_)
                    acTable_->setIcao(aes, icaoBuf, (double)std::time(nullptr));
            }
            uint8_t ges = data[4];
            uint8_t refNo = data[5];
            uint8_t stat = (len > 6) ? data[6] : 0;
            std::string icao = acTable_ ? acTable_->icao(aes) : "";
            char annot[128];
            std::snprintf(annot, sizeof(annot),
                "Call progress — AES %06X%s%s  GES %02X  ref=%02X stat=%02X",
                aes, icao.empty() ? "" : " (", icao.empty() ? "" : icao.c_str(),
                ges, refNo, stat);
            if (!icao.empty()) std::strncat(annot, ")", sizeof(annot) - std::strlen(annot) - 1);
            m.text = annot;
        }
        else if (data[0] == 0x21 && len >= 10) // Call announcement — same layout as C-assign
        {
            uint32_t aes = ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) | data[3];
            m.aesId = aes;
            uint8_t ges = data[4];
            int b7 = data[6], b8 = data[7], b9 = data[8], b10 = data[9];
            int chRx = ((b7 & 0x7F) << 8) | b8;
            int chTx = ((b9 & 0x7F) << 8) | b10;
            double rxMHz = chRx * 0.0025 + 1510.0;
            double txMHz = chTx * 0.0025 + 1611.5;
            bool rxsb = (b7 & 0x80) != 0;
            bool txsb = (b9 & 0x80) != 0;
            std::string icao = acTable_ ? acTable_->icao(aes) : "";
            char annot[148];
            std::snprintf(annot, sizeof(annot),
                "Call announcement — AES %06X%s%s  GES %02X  RX %.4f%s  TX %.4f%s",
                aes, icao.empty() ? "" : " (", icao.empty() ? "" : icao.c_str(),
                ges, rxMHz, rxsb ? " (spot)" : "", txMHz, txsb ? " (spot)" : "");
            if (!icao.empty()) std::strncat(annot, ")", sizeof(annot) - std::strlen(annot) - 1);
            m.text = annot;
        }
        else if (data[0] >= 0x10 && data[0] <= 0x17 && len >= 6) // Log-on SUs
        {
            uint32_t aes = ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) | data[3];
            m.aesId = aes;
            uint8_t ges = (len > 4) ? data[4] : 0;
            std::string icao = acTable_ ? acTable_->icao(aes) : "";
            char annot[128];
            std::snprintf(annot, sizeof(annot),
                "%s — AES %06X%s%s  GES %02X",
                suTypeName(data[0]), aes,
                icao.empty() ? "" : " (", icao.empty() ? "" : icao.c_str(), ges);
            if (!icao.empty()) std::strncat(annot, ")", sizeof(annot) - std::strlen(annot) - 1);
            m.text = annot;
        }
        else if (data[0] == 0x05) // GES Psmc/Rsmc channels
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

            // Diagnostic: dump the raw bytes and computed channels so we can
            // reverse-engineer the correct byte layout against known frequencies.
            logWrite("SU 0x05 hex=%s", m.hex.c_str());
            logWrite("  b3=%02X ges=%02X lsu=%d  ch1=%d(%.4f) ch2=%d(%.4f) ch3=%d(%.4f)",
                     b3, ges, lsu, ch1, f1, ch2, f2, ch3, f3);

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

            // Annotate the SU type text with the decoded satellite + beam info
            // so the Call Progress tab shows it inline.
            char sattext[64];
            std::snprintf(sattext, sizeof(sattext), "Sys table: satellite ID — sat=%d %s  CAC %.4f",
                          satid, lonbuf, cac1);
            if (ch2)
            {
                size_t n = std::strlen(sattext);
                std::snprintf(sattext + n, sizeof(sattext) - n, " / %.4f", cac2);
            }
            m.text = sattext;

            logWrite("SU 0x0C hex=%s", m.hex.c_str());
            logWrite("  satid=%d lon=%.1f  cac1=%d(%.4f) cac2=%d(%.4f)",
                     satid, lon, ch1, cac1, ch2, cac2);
            netTable_->setSatellite(satid, lonbuf);
            netTable_->addChannel(cac1, "CAC/Psmc1 (P-ch RX)", 0, true, 1200);
            if (ch2)
                netTable_->addChannel(cac2, "CAC/Psmc2 (P-ch RX)", 0, true, 1200);
        }
        else if (data[0] >= 0x31 && data[0] <= 0x34 && len >= 10)
        {
            // C-channel assignment SU — decode RX/TX frequencies and spot-beam
            // flags so the Call Progress tab shows useful info inline.
            uint32_t aes = ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) | data[3];
            m.aesId = aes;
            if (baud_ == 8400)
            {
                voiceAesId_ = aes;
                char icaoBuf[8];
                std::snprintf(icaoBuf, sizeof(icaoBuf), "%06X", aes);
                if (acTable_)
                    acTable_->setIcao(aes, icaoBuf, (double)std::time(nullptr));
            }
            uint8_t ges = data[4];
            int b7 = data[6], b8 = data[7], b9 = data[8], b10 = data[9];
            int chRx = ((b7 & 0x7F) << 8) | b8;
            int chTx = ((b9 & 0x7F) << 8) | b10;
            double rxMHz = chRx * 0.0025 + 1510.0;
            double txMHz = chTx * 0.0025 + 1611.5;
            bool rxsb = (b7 & 0x80) != 0;
            bool txsb = (b9 & 0x80) != 0;
            std::string icao = acTable_ ? acTable_->icao(aes) : "";

            char annot[160];
            std::snprintf(annot, sizeof(annot),
                "%s — AES %06X%s%s  GES %02X  RX %.4f%s  TX %.4f%s",
                suTypeName(data[0]), aes,
                icao.empty() ? "" : " (", icao.empty() ? "" : icao.c_str(),
                ges, rxMHz, rxsb ? " (spot)" : "", txMHz, txsb ? " (spot)" : "");
            if (!icao.empty()) std::strncat(annot, ")", sizeof(annot) - std::strlen(annot) - 1);
            // If the SU already carries decoded ACARS text, keep it too.
            if (!m.text.empty() && m.text != suTypeName(data[0]))
            {
                m.text = annot;
                m.text += " | ";
                for (int k = 0; k < len; ++k) if (data[k] >= 0x20 && data[k] < 0x7F) m.text += (char)data[k];
            }
            else
            {
                m.text = annot;
            }
        }
    }

    // Diagnostic: dump every SU type + hex so we can reverse-engineer.

    logWrite("SU %s  hex=%s", suTypeName(data[0]), m.hex.c_str());

    suLog_->addSuAndStore(m, baud_);
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
    int errs2 = ambe_->decode(frame, pcm);
    // Count even damaged frames for voice-follow activity (so marginal calls
    // don't timeout prematurely).
    voiceFrames_.fetch_add(1);

    // Drop severely ECC-damaged frames (mbelib would conceal them anyway).
    if (errs2 > 3)
        return;

    // Drop frames where the decoded PCM energy is abnormally high: corrupted
    // frames can produce full-scale white noise that sounds like a screech.
    // Real AMBE voice at normal levels peaks well below this.
    {
        double sumSq = 0.0;
        for (int i = 0; i < AmbeDecoder::kPcmSamples; ++i)
            sumSq += (double)pcm[i] * pcm[i];
        double rms = std::sqrt(sumSq / (double)AmbeDecoder::kPcmSamples);
        if (rms > 18000.0) // ~55 % of full scale
            return;
    }

    if (record_.load())
        recordPcm(pcm, AmbeDecoder::kPcmSamples);
    if (monitored_.load() && audioSink_)
        audioSink_->push(pcm, AmbeDecoder::kPcmSamples);
}

void Decoder::setRecording(bool on, const std::string& dir, RecordFormat fmt)
{
    if (on && !dir.empty())
        recordDir_ = dir;
    // Format changed while a call is being recorded: close the current file so
    // the very next voice frame opens a new one in the new format.
    if (record_.load() && fmt != recordFmt_)
    {
        if (rec_) rec_->close();
        recActive_.store(false);
    }
    recordFmt_ = fmt;
    record_.store(on);
}

// Close the current call's file if recording was turned off or the call has
// gone idle. Runs on the worker thread (from process()).
void Decoder::maintainRecording()
{
    if (!recActive_.load())
        return;
    // Call boundaries are determined by decoder lifetime (voice-follow
    // creates/removes decoders). The destructor closes the file. Only
    // close here if recording was explicitly stopped via the checkbox.
    if (!record_.load())
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
        // Buffer early PCM until we learn the ICAO (from Call Progress /
        // C-assign) or 2 s have elapsed, so the filename carries the tail tag.
        if (pcmBuf_.size() < 160 * 100) // max 2 s buffer
        {
            if (!voiceAesId_)
            {
                auto now = std::chrono::steady_clock::now();
                if (pcmBuf_.empty())
                    firstPcmTime_ = now;
                double elapsed = std::chrono::duration<double>(now - firstPcmTime_).count();
                if (elapsed < 2.0)
                {
                    pcmBuf_.insert(pcmBuf_.end(), pcm, pcm + n);
                    return;
                }
            }
        }
        // Open the file now (ICAO known, or timeout expired).
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
        const char* ext = (recordFmt_ == RecordFormat::OGG) ? ".ogg" : ".wav";
        std::string icaoTag;
        if (acTable_ && voiceAesId_)
        {
            std::string i = acTable_->icao(voiceAesId_);
            if (!i.empty())
                icaoTag = i;
            else
            {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "%06X", voiceAesId_);
                icaoTag = buf;
            }
        }
        if (icaoTag.empty())
            std::snprintf(name, sizeof(name), "%s/%s_%.4fMHz_ch%d%s",
                          recordDir_.c_str(), ts, chanFreqHz_ / 1e6, channelId_, ext);
        else
            std::snprintf(name, sizeof(name), "%s/%s_%s_%.4fMHz_ch%d%s",
                          recordDir_.c_str(), icaoTag.c_str(), ts, chanFreqHz_ / 1e6, channelId_, ext);
        if (!rec_)
            rec_ = std::make_unique<WavWriter>();
        rec_->setFormat(recordFmt_);
        if (!rec_->open(name, 8000, 1))
        {
            pcmBuf_.clear();
            return;
        }
        recActive_.store(true);
        recordingPath_ = name;
        if (voiceCallLog_)
        {
            std::string icaoStr;
            if (acTable_ && voiceAesId_)
                icaoStr = acTable_->icao(voiceAesId_);
            voiceCallLog_->add({(double)std::chrono::system_clock::to_time_t(
                                   std::chrono::system_clock::now()),
                               0.0, chanFreqHz_ / 1e6, channelId_,
                               voiceAesId_, icaoStr, name, true});
        }
        // Flush buffered PCM (if any) then clear the buffer.
        if (!pcmBuf_.empty())
        {
            rec_->write(pcmBuf_.data(), (int)pcmBuf_.size());
            pcmBuf_.clear();
        }
    }
    if (rec_)
        rec_->write(pcm, n);
}
