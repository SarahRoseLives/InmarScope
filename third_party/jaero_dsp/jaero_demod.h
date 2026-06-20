/*
 * jaero_demod.h -- C API for JAERO burst demodulators
 *
 * DSP code originally from JAERO by Jonathan Olds, MIT license.
 * Stripped of Qt dependencies and wrapped for use as a plain C/C++ library.
 *
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2015 Jonathan Olds (original JAERO DSP)
 */
#ifndef JAERO_DEMOD_H
#define JAERO_DEMOD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*jaero_soft_bits_cb)(const unsigned char *bits, int num_bits, int channel_id, void *user);
typedef void (*jaero_acars_cb)(const uint8_t *data, int len, int channel_id,
                               uint32_t aes_id, uint8_t ges_id,
                               uint8_t qno, uint8_t refno, int downlink,
                               void *user);

/* C-channel assignment (voice/data session setup). Ground station tells
 * aircraft to use a specific RX/TX frequency for a call. */
typedef void (*jaero_cassign_cb)(int channel_id, uint8_t type,
                                 uint32_t aes_id, uint8_t ges_id,
                                 double rx_mhz, double tx_mhz,
                                 void *user);

typedef struct jaero_msk_demod jaero_msk_demod_t;
typedef struct jaero_oqpsk_demod jaero_oqpsk_demod_t;
typedef struct jaero_pmsk_demod jaero_pmsk_demod_t;  /* continuous MSK for P-channel */
typedef struct jaero_oqpsk_cont_demod jaero_oqpsk_cont_demod_t; /* continuous OQPSK for H/H+/L */

/* --- Burst MSK demodulator (R/T channel bursts, 600/1200 baud) --- */
jaero_msk_demod_t *jaero_msk_create(double sample_rate, double symbol_rate, int channel_id, jaero_soft_bits_cb cb, void *user);
void jaero_msk_feed(jaero_msk_demod_t *d, const int16_t *audio, int num_samples);
void jaero_msk_feed_iq(jaero_msk_demod_t *d, const double *iq_interleaved, int num_samples);
void jaero_msk_feed_soft_bits(jaero_msk_demod_t *d, const short *bits, int num_bits);
void jaero_msk_destroy(jaero_msk_demod_t *d);
void jaero_msk_set_acars_callback(jaero_msk_demod_t *d, jaero_acars_cb cb, void *user);

/* --- Continuous MSK demodulator (P-channel, 600/1200 baud) --- */
jaero_pmsk_demod_t *jaero_pmsk_create(double sample_rate, double symbol_rate, int channel_id, jaero_soft_bits_cb cb, void *user);
void jaero_pmsk_feed_iq(jaero_pmsk_demod_t *d, const double *iq_interleaved, int num_samples);
void jaero_pmsk_feed_audio(jaero_pmsk_demod_t *d, const int16_t *audio, int num_samples);
void jaero_pmsk_destroy(jaero_pmsk_demod_t *d);
void jaero_pmsk_set_acars_callback(jaero_pmsk_demod_t *d, jaero_acars_cb cb, void *user);
void jaero_pmsk_set_cassign_callback(jaero_pmsk_demod_t *d, jaero_cassign_cb cb, void *user);
/* Decoded callback: fires for every decoded info field (all signal units,
 * not just valid ACARS). data/len are the raw decoded frame bytes. */
typedef void (*jaero_decoded_cb)(const uint8_t *data, int len, int channel_id, void *user);
void jaero_pmsk_set_decoded_callback(jaero_pmsk_demod_t *d, jaero_decoded_cb cb, void *user);

/* Parsed ACARS message (JAERO-style clean fields). */
typedef struct jaero_acars_msg {
    uint32_t aes_id;
    uint8_t  ges_id;
    int      downlink;     /* 1 = downlink (from aircraft), 0 = uplink */
    int      nonacars;     /* 1 = non-ACARS (e.g. media advisory) */
    char     mode;         /* ACARS mode char */
    char     bi;           /* block id char */
    char     reg[16];      /* aircraft registration, null-terminated */
    char     label[4];     /* 2-char ACARS label, null-terminated */
    const uint8_t *text;   /* decoded message text body */
    int      text_len;
} jaero_acars_msg;
typedef void (*jaero_acars2_cb)(int channel_id, const jaero_acars_msg *msg, void *user);
void jaero_pmsk_set_acars2_callback(jaero_pmsk_demod_t *d, jaero_acars2_cb cb, void *user);
double jaero_pmsk_get_mse(jaero_pmsk_demod_t *d);
double jaero_pmsk_get_ebno(jaero_pmsk_demod_t *d);
int    jaero_pmsk_is_locked(jaero_pmsk_demod_t *d);
/* Spectrum / tune API for the web UI. get_baseband() copies up to
 * max_samples complex floats (interleaved re,im) to `out`; returns count
 * actually written. get_tune_info() fills current audio-Hz values. */
int    jaero_pmsk_get_audio(jaero_pmsk_demod_t *d, double *out, int max_samples);
void   jaero_pmsk_get_tune_info(jaero_pmsk_demod_t *d, double *mixer_center_hz, double *freq_center_hz, double *fs_hz);
void   jaero_pmsk_set_manual_tune(jaero_pmsk_demod_t *d, double audio_hz);
/* AFC control. Web UI disables AFC when the operator clicks-to-tune
 * so the manual frequency sticks; the 'Auto' button re-enables it. */
void   jaero_pmsk_set_afc(jaero_pmsk_demod_t *d, int on);
int    jaero_pmsk_is_afc(jaero_pmsk_demod_t *d);
/* AFC/locking bandwidth from demod settings — UI uses this to pick a
 * reasonable zoom window on the spectrum (show ~2x lockingbw around
 * mixer, not the full 0..Fs/2). */
double jaero_pmsk_get_lockingbw(jaero_pmsk_demod_t *d);
/* Compute a magnitude spectrum of the channel's audio for the web UI.
 * mags_db[] is filled with n_bins log-magnitude values covering 0..Fs/2.
 * Values are normalised so peak = 0 dB, floor clamped around -80 dB.
 * Returns 1 on success, 0 if no data yet. n_bins should be <= 1024. */
int    jaero_pmsk_get_spectrum(jaero_pmsk_demod_t *d, float *mags_db, int n_bins);
/* Constellation points for the web UI scatter plot. `iq_out` is written
 * as interleaved (I, Q) doubles — needs capacity >= 2*max_pairs.
 * Returns number of I/Q pairs actually written. */
int    jaero_pmsk_get_constellation(jaero_pmsk_demod_t *d, double *iq_out, int max_pairs);

/* Lightweight AeroL-only decoder (no MSK demod, just frame decode).
 * Feed soft bits from an external demod via jaero_msk_feed_soft_bits(). */
jaero_msk_demod_t *jaero_aerol_create(double symbol_rate, int channel_id,
                                       jaero_acars_cb acars_cb, void *user);

jaero_oqpsk_demod_t *jaero_oqpsk_create(double sample_rate, double symbol_rate, int channel_id, jaero_soft_bits_cb cb, void *user);
void jaero_oqpsk_feed(jaero_oqpsk_demod_t *d, const int16_t *audio, int num_samples);
void jaero_oqpsk_destroy(jaero_oqpsk_demod_t *d);
void jaero_oqpsk_set_acars_callback(jaero_oqpsk_demod_t *d, jaero_acars_cb cb, void *user);

/* --- Continuous OQPSK demodulator (Aero H/H+/L, 10500 baud forward link) --- */
jaero_oqpsk_cont_demod_t *jaero_oqpsk_cont_create(double sample_rate, double symbol_rate, int channel_id, jaero_soft_bits_cb cb, void *user);
void jaero_oqpsk_cont_feed_audio(jaero_oqpsk_cont_demod_t *d, const int16_t *audio, int num_samples);
void jaero_oqpsk_cont_feed_iq(jaero_oqpsk_cont_demod_t *d, const double *iq_interleaved, int num_samples);
void jaero_oqpsk_cont_destroy(jaero_oqpsk_cont_demod_t *d);
void jaero_oqpsk_cont_set_acars_callback(jaero_oqpsk_cont_demod_t *d, jaero_acars_cb cb, void *user);
void jaero_oqpsk_cont_set_cassign_callback(jaero_oqpsk_cont_demod_t *d, jaero_cassign_cb cb, void *user);
void jaero_oqpsk_cont_set_decoded_callback(jaero_oqpsk_cont_demod_t *d, jaero_decoded_cb cb, void *user);
void jaero_oqpsk_cont_set_acars2_callback(jaero_oqpsk_cont_demod_t *d, jaero_acars2_cb cb, void *user);
/* C-channel voice: one 12-byte AMBE frame at a time. */
typedef void (*jaero_voice_cb)(const uint8_t *frame, int len, int channel_id, void *user);
void jaero_oqpsk_cont_set_voice_callback(jaero_oqpsk_cont_demod_t *d, jaero_voice_cb cb, void *user);
double jaero_oqpsk_cont_get_mse(jaero_oqpsk_cont_demod_t *d);
double jaero_oqpsk_cont_get_ebno(jaero_oqpsk_cont_demod_t *d);
int    jaero_oqpsk_cont_is_locked(jaero_oqpsk_cont_demod_t *d);
int    jaero_oqpsk_cont_get_audio(jaero_oqpsk_cont_demod_t *d, double *out, int max_samples);
void   jaero_oqpsk_cont_get_tune_info(jaero_oqpsk_cont_demod_t *d, double *mixer_center_hz, double *freq_center_hz, double *fs_hz);
void   jaero_oqpsk_cont_set_manual_tune(jaero_oqpsk_cont_demod_t *d, double audio_hz);
void   jaero_oqpsk_cont_set_afc(jaero_oqpsk_cont_demod_t *d, int on);
int    jaero_oqpsk_cont_is_afc(jaero_oqpsk_cont_demod_t *d);
double jaero_oqpsk_cont_get_lockingbw(jaero_oqpsk_cont_demod_t *d);
int    jaero_oqpsk_cont_get_spectrum(jaero_oqpsk_cont_demod_t *d, float *mags_db, int n_bins);
int    jaero_oqpsk_cont_get_constellation(jaero_oqpsk_cont_demod_t *d, double *iq_out, int max_pairs);

#ifdef __cplusplus
}
#endif

#endif /* JAERO_DEMOD_H */
