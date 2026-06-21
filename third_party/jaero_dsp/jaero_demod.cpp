/*
 * jaero_demod.cpp -- C API wrapper for JAERO burst demodulators
 *
 * DSP code originally from JAERO by Jonathan Olds, MIT license.
 */
#include "jaero_demod.h"
#include "burstmskdemodulator.h"
#include "burstoqpskdemodulator.h"
#include "mskdemodulator.h"
#include "oqpskdemodulator.h"
#include "aerol.h"
#include "jfft.h"

#include <cmath>
#include <vector>

/* Shared FFT-based magnitude spectrum helper for both demods.
 * Takes up to 4096 audio samples, applies a Hann window, runs a real-input
 * FFT, and writes n_bins log-magnitude values (peak=0 dB, clamped at -80)
 * covering 0..Fs/2. n_bins can be less than half the FFT size — we
 * max-pool within each bin range for a cleaner display. Runs on the web
 * thread; no allocations outside this call. */
static int compute_spectrum(const double *audio, int num_samples,
                            float *mags_db, int n_bins)
{
    if (!audio || num_samples < 64 || !mags_db || n_bins < 2) return 0;
    /* Round down to next power of two for FFT */
    int fft_size = 1;
    while (fft_size * 2 <= num_samples && fft_size * 2 <= 4096) fft_size *= 2;
    if (fft_size < 64) return 0;

    std::vector<double> windowed(fft_size);
    double two_pi = 2.0 * M_PI;
    for (int i = 0; i < fft_size; i++) {
        double w = 0.5 * (1.0 - std::cos(two_pi * i / (fft_size - 1)));
        windowed[i] = audio[i] * w;
    }

    /* JFFT's real FFT interface takes size = 2*nfft, treats the input as
     * nfft complex values (packing pairs of reals), and writes 2*nfft
     * complex output bins (bins nfft..2*nfft-1 are the conjugate mirror).
     * For an N-sample real-input FFT: init(N/2), fft_real(real, spec, N),
     * and the one-sided spectrum lives in bins 0..N/2. */
    JFFT jfft;
    int nfft = fft_size / 2;
    jfft.init(nfft);  /* init() takes int& and may round to a supported size */
    std::vector<JFFT::cpx_type> spec(2 * nfft);
    jfft.fft_real(windowed.data(), spec.data(), 2 * nfft);

    /* One-sided magnitude^2 (bins 0..nfft inclusive, covers 0..Fs/2) */
    int half = nfft;
    std::vector<double> mag2(half);
    double peak = 0;
    for (int i = 0; i < half; i++) {
        double r = spec[i].real(), ii = spec[i].imag();
        mag2[i] = r * r + ii * ii;
        if (mag2[i] > peak) peak = mag2[i];
    }
    if (peak <= 0) peak = 1e-12;

    /* Max-pool from `half` bins down to n_bins */
    for (int b = 0; b < n_bins; b++) {
        int lo = (int)((long long)b * half / n_bins);
        int hi = (int)((long long)(b + 1) * half / n_bins);
        if (hi > half) hi = half;
        if (hi <= lo) hi = lo + 1;
        double m = 0;
        for (int i = lo; i < hi; i++) if (mag2[i] > m) m = mag2[i];
        double db = 10.0 * std::log10(m / peak + 1e-12);
        if (db < -80.0) db = -80.0;
        if (db > 0.0)   db = 0.0;
        mags_db[b] = (float)db;
    }
    return 1;
}

#define JAERO_BLOCK_SIZE 8192  /* large enough for JFastFir's overlap-save FFT */

struct jaero_msk_demod {
    BurstMskDemodulator *demod;
    AeroL *aerol;
    int channel_id;
    jaero_soft_bits_cb cb;
    void *user;
    jaero_acars_cb acars_cb;
    void *acars_user;
    int16_t buf[JAERO_BLOCK_SIZE];
    int buf_len;
};

struct jaero_oqpsk_demod {
    BurstOqpskDemodulator *demod;
    AeroL *aerol;
    int channel_id;
    jaero_soft_bits_cb cb;
    void *user;
    jaero_acars_cb acars_cb;
    void *acars_user;
};

/* internal: forward decoded ACARS data from AeroL to the C callback */
static void aerol_acars_adapter(ACARSItem &acarsitem, void *ctx)
{
    jaero_msk_demod_t *d = (jaero_msk_demod_t *)ctx;
    if (d->acars_cb && acarsitem.valid) {
        /* pack ISU userdata as the output payload */
        const uint8_t *data = acarsitem.isuitem.userdata.data();
        int len = (int)acarsitem.isuitem.userdata.size();
        d->acars_cb(data, len, d->channel_id,
                    acarsitem.isuitem.AESID, acarsitem.isuitem.GESID,
                    acarsitem.isuitem.QNO, acarsitem.isuitem.REFNO,
                    acarsitem.downlink ? 1 : 0,
                    d->acars_user);
    }
}

/* internal callback adapters */
static void msk_bits_adapter(const short *bits, int num_bits, void *ctx)
{
    jaero_msk_demod_t *d = (jaero_msk_demod_t *)ctx;

    /* feed soft bits through AeroL for frame decoding */
    if (d->aerol) {
        std::vector<short> sv(bits, bits + num_bits);
        d->aerol->processDemodulatedSoftBits(sv);
    }

    if (d->cb) {
        /* soft bits are stored as unsigned char values in short array */
        std::vector<unsigned char> buf(num_bits);
        for (int i = 0; i < num_bits; i++)
            buf[i] = (unsigned char)(bits[i] & 0xFF);
        d->cb(buf.data(), num_bits, d->channel_id, d->user);
    }
}

/* OQPSK -> AeroL ACARS adapter. */
static void oqpsk_aerol_acars_adapter(ACARSItem &acarsitem, void *ctx)
{
    jaero_oqpsk_demod_t *d = (jaero_oqpsk_demod_t *)ctx;
    if (d->acars_cb && acarsitem.valid) {
        const uint8_t *data = acarsitem.isuitem.userdata.data();
        int len = (int)acarsitem.isuitem.userdata.size();
        d->acars_cb(data, len, d->channel_id,
                    acarsitem.isuitem.AESID, acarsitem.isuitem.GESID,
                    acarsitem.isuitem.QNO, acarsitem.isuitem.REFNO,
                    acarsitem.downlink ? 1 : 0,
                    d->acars_user);
    }
}

static void oqpsk_bits_adapter(const short *bits, int num_bits, void *ctx)
{
    jaero_oqpsk_demod_t *d = (jaero_oqpsk_demod_t *)ctx;

    /* Feed AeroL for full frame decode (burst mode = true for OQPSK). */
    if (d->aerol) {
        std::vector<short> sv(bits, bits + num_bits);
        d->aerol->processDemodulatedSoftBits(sv);
    }

    if (d->cb) {
        std::vector<unsigned char> buf(num_bits);
        for (int i = 0; i < num_bits; i++)
            buf[i] = (unsigned char)(bits[i] & 0xFF);
        d->cb(buf.data(), num_bits, d->channel_id, d->user);
    }
}

extern "C" {

jaero_msk_demod_t *jaero_msk_create(double sample_rate, double symbol_rate, int channel_id,
                                     jaero_soft_bits_cb cb, void *user)
{
    jaero_msk_demod_t *d = new jaero_msk_demod_t;
    d->channel_id = channel_id;
    d->cb = cb;
    d->user = user;
    d->acars_cb = NULL;
    d->acars_user = NULL;
    d->demod = new BurstMskDemodulator();

    /* Create AeroL in CONTINUOUS (P-channel) mode. Inmarsat Aero P-channel
     * is a continuous framed signal (sync+header+data) not bursts.
     * Prior sessions had this as true (burst mode) which is wrong for P-channel. */
    d->aerol = new AeroL();
    d->aerol->setSettings(symbol_rate, false);

    BurstMskDemodulator::Settings s;
    s.Fs = sample_rate;
    s.fb = symbol_rate;
    s.freq_center = 0.0;     /* baseband IQ from channelizer */
    s.lockingbw = 2000.0;    /* ±1000 Hz for SDR PPM offset */
    s.coarsefreqest_fft_power = 13;
    s.symbolspercycle = (symbol_rate <= 600) ? 8 : 16;
    s.signalthreshold = 0.6;

    d->demod->setSettings(s);
    d->demod->setSoftBitsCallback(msk_bits_adapter, d);
    d->buf_len = 0;

    return d;
}

void jaero_msk_feed(jaero_msk_demod_t *d, const int16_t *audio, int num_samples)
{
    if (!d || !d->demod) return;
    for (int i = 0; i < num_samples; i++) {
        d->buf[d->buf_len++] = audio[i];
        if (d->buf_len >= JAERO_BLOCK_SIZE) {
            d->demod->feedAudio(d->buf, d->buf_len, 0);
            d->buf_len = 0;
        }
    }
}

void jaero_msk_feed_soft_bits(jaero_msk_demod_t *d, const short *bits, int num_bits)
{
    if (!d || !d->aerol) return;
    std::vector<short> sv(bits, bits + num_bits);
    d->aerol->processDemodulatedSoftBits(sv);
}

void jaero_msk_feed_iq(jaero_msk_demod_t *d, const double *iq_interleaved, int num_samples)
{
    if (!d || !d->demod) return;
    d->demod->feedIQ(iq_interleaved, num_samples);
}

void jaero_msk_destroy(jaero_msk_demod_t *d)
{
    if (!d) return;
    delete d->aerol;
    delete d->demod;
    delete d;
}

void jaero_msk_set_acars_callback(jaero_msk_demod_t *d, jaero_acars_cb cb, void *user)
{
    if (!d) return;
    d->acars_cb = cb;
    d->acars_user = user;
    if (d->aerol) {
        d->aerol->setACARSCallback(aerol_acars_adapter, d);
    }
}

jaero_msk_demod_t *jaero_aerol_create(double symbol_rate, int channel_id,
                                       jaero_acars_cb acars_cb, void *user)
{
    jaero_msk_demod_t *d = new jaero_msk_demod_t;
    d->channel_id = channel_id;
    d->cb = NULL;
    d->user = NULL;
    d->acars_cb = acars_cb;
    d->acars_user = user;
    d->demod = NULL;
    d->buf_len = 0;

    d->aerol = new AeroL();
    d->aerol->setSettings(symbol_rate, false);  /* P-channel continuous mode */
    if (acars_cb)
        d->aerol->setACARSCallback(aerol_acars_adapter, d);

    return d;
}

jaero_oqpsk_demod_t *jaero_oqpsk_create(double sample_rate, double symbol_rate, int channel_id,
                                          jaero_soft_bits_cb cb, void *user)
{
    jaero_oqpsk_demod_t *d = new jaero_oqpsk_demod_t;
    d->channel_id = channel_id;
    d->cb = cb;
    d->user = user;
    d->acars_cb = NULL;
    d->acars_user = NULL;
    d->demod = new BurstOqpskDemodulator();

    /* AeroL: 10500 forward link is continuous (not burst), same as
     * P-channel MSK. JAERO GUI confirms "10500" (continuous) decodes,
     * "10500 burst" does not on L-band forward link. */
    d->aerol = new AeroL();
    d->aerol->setSettings(symbol_rate, false);  /* continuous mode */

    BurstOqpskDemodulator::Settings s;
    s.Fs = sample_rate;
    s.fb = symbol_rate;
    /* JAERO desktop defaults: 8 kHz audio center, 10.5 kHz locking BW.
     * Matches AUDIO_CENTER_HZ in main.c — if one changes, update both.
     * signalthreshold lowered from JAERO default (0.6) to give weaker
     * L-band forward-link C-channel bursts a chance to sync. */
    s.freq_center = 8000.0;
    s.lockingbw = 10500.0;
    s.coarsefreqest_fft_power = 13;
    s.signalthreshold = 0.3;
    s.channel_stereo = false;

    d->demod->setSettings(s);
    d->demod->setSoftBitsCallback(oqpsk_bits_adapter, d);
    d->demod->setAFC(true);  /* on by default in JAERO GUI */

    return d;
}

void jaero_oqpsk_feed(jaero_oqpsk_demod_t *d, const int16_t *audio, int num_samples)
{
    if (!d || !d->demod) return;
    d->demod->feedAudio(audio, num_samples, 0);
}

void jaero_oqpsk_destroy(jaero_oqpsk_demod_t *d)
{
    if (!d) return;
    delete d->aerol;
    delete d->demod;
    delete d;
}

void jaero_oqpsk_set_acars_callback(jaero_oqpsk_demod_t *d,
                                     jaero_acars_cb cb, void *user)
{
    if (!d) return;
    d->acars_cb = cb;
    d->acars_user = user;
    if (d->aerol)
        d->aerol->setACARSCallback(oqpsk_aerol_acars_adapter, d);
}

/* ============================================================
 * Continuous MSK demodulator (P-channel)
 * ============================================================ */

struct jaero_pmsk_demod {
    MskDemodulator *demod;
    AeroL *aerol;
    int channel_id;
    jaero_soft_bits_cb cb;
    void *user;
    jaero_acars_cb acars_cb;
    void *acars_user;
    jaero_acars2_cb acars2_cb;
    void *acars2_user;
    jaero_cassign_cb cassign_cb;
    void *cassign_user;
    jaero_decoded_cb decoded_cb;
    void *decoded_user;
    bool sigstat_good;
    bool afc_on;          /* shadow of MskDemodulator::afc for UI readout */
    double lockingbw;     /* shadow of Settings.lockingbw so UI can pick a zoom window */
};

// Fill a jaero_acars_msg from a parsed ACARSItem.
static void fill_acars_msg(jaero_acars_msg &m, ACARSItem &a)
{
    m.aes_id = a.isuitem.AESID;
    m.ges_id = a.isuitem.GESID;
    m.downlink = a.downlink ? 1 : 0;
    m.nonacars = a.nonacars ? 1 : 0;
    m.mode = a.MODE;
    m.bi = (char)a.BI;
    size_t rn = a.PLANEREG.size();
    if (rn > 15) rn = 15;
    for (size_t i = 0; i < rn; ++i) m.reg[i] = (char)a.PLANEREG[i];
    m.reg[rn] = 0;
    size_t ln = a.LABEL.size();
    if (ln > 3) ln = 3;
    for (size_t i = 0; i < ln; ++i) m.label[i] = (char)a.LABEL[i];
    m.label[ln] = 0;
    m.text = a.message.data();
    m.text_len = (int)a.message.size();
}

static void pmsk_decoded_adapter(const uint8_t *data, int len, void *ctx)
{
    jaero_pmsk_demod_t *d = (jaero_pmsk_demod_t *)ctx;
    if (d->decoded_cb)
        d->decoded_cb(data, len, d->channel_id, d->decoded_user);
}

static void pmsk_bits_adapter(const short *bits, int num_bits, void *ctx)
{
    jaero_pmsk_demod_t *d = (jaero_pmsk_demod_t *)ctx;
    if (d->aerol) {
        std::vector<short> sv(bits, bits + num_bits);
        d->aerol->processDemodulatedSoftBits(sv);
    }
    if (d->cb) {
        std::vector<unsigned char> buf(num_bits);
        for (int i = 0; i < num_bits; i++)
            buf[i] = (unsigned char)(bits[i] & 0xFF);
        d->cb(buf.data(), num_bits, d->channel_id, d->user);
    }
}

static void pmsk_acars_adapter(ACARSItem &acarsitem, void *ctx)
{
    jaero_pmsk_demod_t *d = (jaero_pmsk_demod_t *)ctx;
    if (!acarsitem.valid)
        return;
    if (d->acars_cb) {
        const uint8_t *data = acarsitem.isuitem.userdata.data();
        int len = (int)acarsitem.isuitem.userdata.size();
        d->acars_cb(data, len, d->channel_id,
                    acarsitem.isuitem.AESID, acarsitem.isuitem.GESID,
                    acarsitem.isuitem.QNO, acarsitem.isuitem.REFNO,
                    acarsitem.downlink ? 1 : 0,
                    d->acars_user);
    }
    if (d->acars2_cb) {
        jaero_acars_msg m;
        fill_acars_msg(m, acarsitem);
        d->acars2_cb(d->channel_id, &m, d->acars2_user);
    }
}

static void pmsk_sigstat_adapter(bool signal_good, void *ctx)
{
    jaero_pmsk_demod_t *d = (jaero_pmsk_demod_t *)ctx;
    d->sigstat_good = signal_good;
    if (!signal_good && d->aerol)
        d->aerol->LostSignal();
}

jaero_pmsk_demod_t *jaero_pmsk_create(double sample_rate, double symbol_rate,
                                       int channel_id,
                                       jaero_soft_bits_cb cb, void *user)
{
    jaero_pmsk_demod_t *d = new jaero_pmsk_demod_t;
    d->channel_id = channel_id;
    d->cb = cb;
    d->user = user;
    d->acars_cb = NULL;
    d->acars_user = NULL;
    d->acars2_cb = NULL;
    d->acars2_user = NULL;
    d->cassign_cb = NULL;
    d->cassign_user = NULL;
    d->decoded_cb = NULL;
    d->decoded_user = NULL;
    d->sigstat_good = false;
    d->demod = new MskDemodulator();

    /* AeroL in CONTINUOUS (P-channel) mode */
    d->aerol = new AeroL();
    d->aerol->setSettings(symbol_rate, false);  /* burstmode=false */

    MskDemodulator::Settings s;
    s.Fs = sample_rate;
    s.fb = symbol_rate;
    /* Our feedIQ wrapper mixes baseband IQ → int16 audio at freq_center Hz
     * (same formula as our ZMQ out → real JAERO, known to decode).
     * Demod's mixer_center brings audio back to baseband. */
    s.freq_center = 8000.0;
    s.lockingbw = (symbol_rate <= 600) ? 900.0 : 4800.0;  /* PocketAERO: 900/4800 */
    s.coarsefreqest_fft_power = 15;
    s.symbolspercycle = (symbol_rate <= 600) ? 8 : 16;
    s.signalthreshold = 0.45;

    d->demod->setSettings(s);
    d->demod->setSoftBitsCallback(pmsk_bits_adapter, d);
    d->demod->setSignalStatusCallback(pmsk_sigstat_adapter, d);
    d->demod->setAFC(true);  /* on by default in JAERO GUI */
    d->afc_on = true;
    d->lockingbw = s.lockingbw;

    return d;
}

void jaero_pmsk_feed_iq(jaero_pmsk_demod_t *d,
                         const double *iq_interleaved, int num_samples)
{
    if (!d || !d->demod) return;
    d->demod->feedIQ(iq_interleaved, num_samples);
}

void jaero_pmsk_feed_audio(jaero_pmsk_demod_t *d,
                            const int16_t *audio, int num_samples)
{
    if (!d || !d->demod) return;
    d->demod->feedAudio(audio, num_samples, 0);
}

void jaero_pmsk_destroy(jaero_pmsk_demod_t *d)
{
    if (!d) return;
    delete d->aerol;
    delete d->demod;
    delete d;
}

void jaero_pmsk_set_acars_callback(jaero_pmsk_demod_t *d,
                                     jaero_acars_cb cb, void *user)
{
    if (!d) return;
    d->acars_cb = cb;
    d->acars_user = user;
    if (d->aerol)
        d->aerol->setACARSCallback(pmsk_acars_adapter, d);
}

void jaero_pmsk_set_decoded_callback(jaero_pmsk_demod_t *d,
                                       jaero_decoded_cb cb, void *user)
{
    if (!d) return;
    d->decoded_cb = cb;
    d->decoded_user = user;
    if (d->aerol)
        d->aerol->setDecodedCallback(pmsk_decoded_adapter, d);
}

void jaero_pmsk_set_acars2_callback(jaero_pmsk_demod_t *d,
                                      jaero_acars2_cb cb, void *user)
{
    if (!d) return;
    d->acars2_cb = cb;
    d->acars2_user = user;
    if (d->aerol)
        d->aerol->setACARSCallback(pmsk_acars_adapter, d);
}

static void pmsk_cassign_adapter(CChannelAssignmentItem &item, void *ctx)
{
    jaero_pmsk_demod_t *d = (jaero_pmsk_demod_t *)ctx;
    if (d->cassign_cb)
        d->cassign_cb(d->channel_id, item.type, item.AESID, item.GESID,
                      item.receive_freq, item.transmit_freq, d->cassign_user);
}

void jaero_pmsk_set_cassign_callback(jaero_pmsk_demod_t *d,
                                       jaero_cassign_cb cb, void *user)
{
    if (!d) return;
    d->cassign_cb = cb;
    d->cassign_user = user;
    if (d->aerol)
        d->aerol->setCChannelAssignmentCallback(pmsk_cassign_adapter, d);
}

double jaero_pmsk_get_mse(jaero_pmsk_demod_t *d) {
    if (!d || !d->demod) return 1.0;
    return d->demod->getMSE();
}

double jaero_pmsk_get_ebno(jaero_pmsk_demod_t *d) {
    if (!d || !d->demod) return 0;
    return d->demod->getEbNo();
}

int jaero_pmsk_is_locked(jaero_pmsk_demod_t *d) {
    return (d && d->sigstat_good) ? 1 : 0;
}

int jaero_pmsk_get_audio(jaero_pmsk_demod_t *d, double *out, int max_samples)
{
    if (!d || !d->demod || !out || max_samples <= 0) return 0;
    return d->demod->get_audio_snapshot(out, max_samples);
}

void jaero_pmsk_get_tune_info(jaero_pmsk_demod_t *d, double *mc, double *fc, double *fs)
{
    if (!d || !d->demod) { if (mc) *mc = 0; if (fc) *fc = 0; if (fs) *fs = 0; return; }
    if (mc) *mc = d->demod->getMixerCenterHz();
    if (fc) *fc = d->demod->getFreqCenterHz();
    if (fs) *fs = d->demod->getFs();
}

void jaero_pmsk_set_manual_tune(jaero_pmsk_demod_t *d, double audio_hz)
{
    if (d && d->demod) d->demod->setManualTune(audio_hz);
}

void jaero_pmsk_set_afc(jaero_pmsk_demod_t *d, int on)
{
    if (!d || !d->demod) return;
    d->demod->setAFC(on ? true : false);
    d->afc_on = on ? true : false;
}

int jaero_pmsk_is_afc(jaero_pmsk_demod_t *d)
{
    return (d && d->afc_on) ? 1 : 0;
}

double jaero_pmsk_get_lockingbw(jaero_pmsk_demod_t *d)
{
    return d ? d->lockingbw : 0;
}

int jaero_pmsk_get_spectrum(jaero_pmsk_demod_t *d, float *mags_db, int n_bins)
{
    if (!d || !d->demod || !mags_db || n_bins < 2) return 0;
    std::vector<double> audio(4096);
    int n = d->demod->get_audio_snapshot(audio.data(), 4096);
    return compute_spectrum(audio.data(), n, mags_db, n_bins);
}

int jaero_pmsk_get_constellation(jaero_pmsk_demod_t *d, double *iq_out, int max_pairs)
{
    if (!d || !d->demod) return 0;
    return d->demod->get_constellation_snapshot(iq_out, max_pairs);
}

/* ============================================================
 * Continuous OQPSK demodulator (Aero H/H+/L, 10500 baud forward link)
 * Uses OqpskDemodulator (not BurstOqpskDemodulator).
 * AeroL in continuous (non-burst) mode — same as P-channel MSK.
 * ============================================================ */

struct jaero_oqpsk_cont_demod {
    OqpskDemodulator *demod;
    AeroL *aerol;
    int channel_id;
    jaero_soft_bits_cb cb;
    void *user;
    jaero_acars_cb acars_cb;
    void *acars_user;
    jaero_acars2_cb acars2_cb;
    void *acars2_user;
    jaero_cassign_cb cassign_cb;
    void *cassign_user;
    jaero_decoded_cb decoded_cb;
    void *decoded_user;
    jaero_voice_cb voice_cb;
    void *voice_user;
    bool sigstat_good;
    bool afc_on;          /* shadow of OqpskDemodulator::afc for UI readout */
    double lockingbw;     /* shadow of Settings.lockingbw so UI can pick a zoom window */
};

static void oqpsk_cont_voice_adapter(const uint8_t *frame, int len, void *ctx)
{
    jaero_oqpsk_cont_demod_t *d = (jaero_oqpsk_cont_demod_t *)ctx;
    if (d->voice_cb)
        d->voice_cb(frame, len, d->channel_id, d->voice_user);
}

static void oqpsk_cont_decoded_adapter(const uint8_t *data, int len, void *ctx)
{
    jaero_oqpsk_cont_demod_t *d = (jaero_oqpsk_cont_demod_t *)ctx;
    if (d->decoded_cb)
        d->decoded_cb(data, len, d->channel_id, d->decoded_user);
}

static void oqpsk_cont_aerol_acars_adapter(ACARSItem &acarsitem, void *ctx)
{
    jaero_oqpsk_cont_demod_t *d = (jaero_oqpsk_cont_demod_t *)ctx;
    if (!acarsitem.valid)
        return;
    if (d->acars_cb) {
        const uint8_t *data = acarsitem.isuitem.userdata.data();
        int len = (int)acarsitem.isuitem.userdata.size();
        d->acars_cb(data, len, d->channel_id,
                    acarsitem.isuitem.AESID, acarsitem.isuitem.GESID,
                    acarsitem.isuitem.QNO, acarsitem.isuitem.REFNO,
                    acarsitem.downlink ? 1 : 0,
                    d->acars_user);
    }
    if (d->acars2_cb) {
        jaero_acars_msg m;
        fill_acars_msg(m, acarsitem);
        d->acars2_cb(d->channel_id, &m, d->acars2_user);
    }
}

static void oqpsk_cont_bits_adapter(const short *bits, int num_bits, void *ctx)
{
    jaero_oqpsk_cont_demod_t *d = (jaero_oqpsk_cont_demod_t *)ctx;

    if (d->aerol) {
        std::vector<short> sv(bits, bits + num_bits);
        d->aerol->processDemodulatedSoftBits(sv);
    }

    if (d->cb) {
        std::vector<unsigned char> buf(num_bits);
        for (int i = 0; i < num_bits; i++)
            buf[i] = (unsigned char)(bits[i] & 0xFF);
        d->cb(buf.data(), num_bits, d->channel_id, d->user);
    }
}

/* Signal status callback: reset AeroL when OQPSK demod loses lock.
 * Matches JAERO's SignalStatus→LostSignal connection. Without this,
 * AeroL accumulates noise bits during signal dropouts and gets stuck
 * searching for frame sync in garbage. */
static void oqpsk_cont_sigstat_adapter(bool signal_good, void *ctx)
{
    jaero_oqpsk_cont_demod_t *d = (jaero_oqpsk_cont_demod_t *)ctx;
    d->sigstat_good = signal_good;
    if (!signal_good && d->aerol)
        d->aerol->LostSignal();
}

jaero_oqpsk_cont_demod_t *jaero_oqpsk_cont_create(double sample_rate, double symbol_rate,
                                                    int channel_id,
                                                    jaero_soft_bits_cb cb, void *user)
{
    jaero_oqpsk_cont_demod_t *d = new jaero_oqpsk_cont_demod_t;
    d->channel_id  = channel_id;
    d->cb          = cb;
    d->user        = user;
    d->acars_cb    = NULL;
    d->acars_user  = NULL;
    d->acars2_cb   = NULL;
    d->acars2_user = NULL;
    d->cassign_cb  = NULL;
    d->cassign_user = NULL;
    d->decoded_cb  = NULL;
    d->decoded_user = NULL;
    d->voice_cb    = NULL;
    d->voice_user  = NULL;
    d->sigstat_good = false;
    d->demod       = new OqpskDemodulator();

    /* AeroL: continuous (non-burst) mode — 10500 baud forward link is
     * continuous, not burst. Same as P-channel MSK. */
    d->aerol = new AeroL();
    d->aerol->setSettings(symbol_rate, false);  /* burstmode=false */

    OqpskDemodulator::Settings s;
    s.Fs          = sample_rate;
    s.fb          = symbol_rate;
    /* JAERO desktop defaults for 10500 baud: 8 kHz audio center,
     * 10.5 kHz locking bandwidth. feedAudio path expects signal at freq_center.
     * feedIQ wrapper mixes IQ → audio at freq_center (same as ZMQ output). */
    s.freq_center              = 8000.0;
    /* OQPSK AFC lockingbw. 10500 default catches on-nominal carriers but
     * misses drifted ones; 20-30 kHz catches drifted but can wander onto
     * nearby spurs. 8400 voice C-channels need a wider capture to pull onto
     * slightly-off assigned carriers, so default it to ±6 kHz. Override via
     * --oqpsk-lockingbw when troubleshooting. */
    extern double oqpsk_lockingbw;
    double default_bw = (symbol_rate <= 8400) ? 12000.0 : 10500.0;
    s.lockingbw                = (oqpsk_lockingbw > 0) ? oqpsk_lockingbw : default_bw;
    s.coarsefreqest_fft_power  = 14;
    s.signalthreshold          = 0.8; /* raised from JAERO default 0.65 — accepts noisier constellations */

    d->demod->setSettings(s);
    d->demod->setSoftBitsCallback(oqpsk_cont_bits_adapter, d);
    d->demod->setSignalStatusCallback(oqpsk_cont_sigstat_adapter, d);
    d->demod->setAFC(true);  /* on by default in JAERO GUI */
    d->afc_on = true;
    d->lockingbw = s.lockingbw;

    return d;
}

void jaero_oqpsk_cont_feed_audio(jaero_oqpsk_cont_demod_t *d,
                                   const int16_t *audio, int num_samples)
{
    if (!d || !d->demod) return;
    d->demod->feedAudio(audio, num_samples, 0);
}

void jaero_oqpsk_cont_feed_iq(jaero_oqpsk_cont_demod_t *d,
                                const double *iq_interleaved, int num_samples)
{
    if (!d || !d->demod) return;
    d->demod->feedIQ(iq_interleaved, num_samples);
}

void jaero_oqpsk_cont_destroy(jaero_oqpsk_cont_demod_t *d)
{
    if (!d) return;
    delete d->aerol;
    delete d->demod;
    delete d;
}

void jaero_oqpsk_cont_set_acars_callback(jaero_oqpsk_cont_demod_t *d,
                                          jaero_acars_cb cb, void *user)
{
    if (!d) return;
    d->acars_cb   = cb;
    d->acars_user = user;
    if (d->aerol)
        d->aerol->setACARSCallback(oqpsk_cont_aerol_acars_adapter, d);
}

void jaero_oqpsk_cont_set_decoded_callback(jaero_oqpsk_cont_demod_t *d,
                                             jaero_decoded_cb cb, void *user)
{
    if (!d) return;
    d->decoded_cb = cb;
    d->decoded_user = user;
    if (d->aerol)
        d->aerol->setDecodedCallback(oqpsk_cont_decoded_adapter, d);
}

void jaero_oqpsk_cont_set_acars2_callback(jaero_oqpsk_cont_demod_t *d,
                                            jaero_acars2_cb cb, void *user)
{
    if (!d) return;
    d->acars2_cb = cb;
    d->acars2_user = user;
    if (d->aerol)
        d->aerol->setACARSCallback(oqpsk_cont_aerol_acars_adapter, d);
}

void jaero_oqpsk_cont_set_voice_callback(jaero_oqpsk_cont_demod_t *d,
                                           jaero_voice_cb cb, void *user)
{
    if (!d) return;
    d->voice_cb = cb;
    d->voice_user = user;
    if (d->aerol)
        d->aerol->setVoiceCallback(oqpsk_cont_voice_adapter, d);
}

static void oqpsk_cont_cassign_adapter(CChannelAssignmentItem &item, void *ctx)
{
    jaero_oqpsk_cont_demod_t *d = (jaero_oqpsk_cont_demod_t *)ctx;
    if (d->cassign_cb)
        d->cassign_cb(d->channel_id, item.type, item.AESID, item.GESID,
                      item.receive_freq, item.transmit_freq, d->cassign_user);
}

void jaero_oqpsk_cont_set_cassign_callback(jaero_oqpsk_cont_demod_t *d,
                                              jaero_cassign_cb cb, void *user)
{
    if (!d) return;
    d->cassign_cb = cb;
    d->cassign_user = user;
    if (d->aerol)
        d->aerol->setCChannelAssignmentCallback(oqpsk_cont_cassign_adapter, d);
}

double jaero_oqpsk_cont_get_mse(jaero_oqpsk_cont_demod_t *d) {
    if (!d || !d->demod) return 1.0;
    return d->demod->getMSE();
}

double jaero_oqpsk_cont_get_ebno(jaero_oqpsk_cont_demod_t *d) {
    if (!d || !d->demod) return 0;
    return d->demod->getEbNo();
}

int jaero_oqpsk_cont_is_locked(jaero_oqpsk_cont_demod_t *d) {
    return (d && d->sigstat_good) ? 1 : 0;
}

int jaero_oqpsk_cont_get_audio(jaero_oqpsk_cont_demod_t *d, double *out, int max_samples)
{
    if (!d || !d->demod || !out || max_samples <= 0) return 0;
    return d->demod->get_audio_snapshot(out, max_samples);
}

void jaero_oqpsk_cont_get_tune_info(jaero_oqpsk_cont_demod_t *d, double *mc, double *fc, double *fs)
{
    if (!d || !d->demod) { if (mc) *mc = 0; if (fc) *fc = 0; if (fs) *fs = 0; return; }
    if (mc) *mc = d->demod->getMixerCenterHz();
    if (fc) *fc = d->demod->getFreqCenterHz();
    if (fs) *fs = d->demod->getFs();
}

void jaero_oqpsk_cont_set_manual_tune(jaero_oqpsk_cont_demod_t *d, double audio_hz)
{
    if (d && d->demod) d->demod->setManualTune(audio_hz);
}

void jaero_oqpsk_cont_set_afc(jaero_oqpsk_cont_demod_t *d, int on)
{
    if (!d || !d->demod) return;
    d->demod->setAFC(on ? true : false);
    d->afc_on = on ? true : false;
}

int jaero_oqpsk_cont_is_afc(jaero_oqpsk_cont_demod_t *d)
{
    return (d && d->afc_on) ? 1 : 0;
}

double jaero_oqpsk_cont_get_lockingbw(jaero_oqpsk_cont_demod_t *d)
{
    return d ? d->lockingbw : 0;
}

int jaero_oqpsk_cont_get_spectrum(jaero_oqpsk_cont_demod_t *d, float *mags_db, int n_bins)
{
    if (!d || !d->demod || !mags_db || n_bins < 2) return 0;
    std::vector<double> audio(4096);
    int n = d->demod->get_audio_snapshot(audio.data(), 4096);
    return compute_spectrum(audio.data(), n, mags_db, n_bins);
}

int jaero_oqpsk_cont_get_constellation(jaero_oqpsk_cont_demod_t *d, double *iq_out, int max_pairs)
{
    if (!d || !d->demod) return 0;
    return d->demod->get_constellation_snapshot(iq_out, max_pairs);
}

} /* extern "C" */
