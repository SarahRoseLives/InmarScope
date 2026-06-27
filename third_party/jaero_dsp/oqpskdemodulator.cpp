/*
 * OqpskDemodulator -- JAERO's continuous OQPSK demodulator for Aero H/H+/L.
 * Ported from github.com/jontio/JAERO, Qt-stripped to pure C++ with C callback.
 * SPDX-License-Identifier: MIT (JAERO original)
 *
 * DSP math is identical to JAERO's OqpskDemodulator::writeData().
 * Qt removed: QObject/QIODevice → plain class, QVector → std::vector,
 *             signals/slots → direct calls, qDebug → fprintf,
 *             QElapsedTimer → (timer GUI logic dropped — not needed for decode),
 *             qRound → local qRoundI helper.
 */

#include "oqpskdemodulator.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#ifndef SPECTRUM_FFT_POWER
#define SPECTRUM_FFT_POWER 13
#endif

static inline int qRoundI(double v)
{
    return (int)(v + (v >= 0.0 ? 0.5 : -0.5));
}

OqpskDemodulator::OqpskDemodulator()
{
    soft_bits_cb   = NULL;
    soft_bits_user = NULL;
    sigstat_cb     = NULL;
    sigstat_user   = NULL;
    sigstat_last   = true;

    afc  = false;
    sql  = false;
    dcd  = false;
    cpuReduce = false;
    scatterpointtype = SPT_constellation;

    mse  = 100.0;

    Fs           = 48000;
    lockingbw    = 10500;
    freq_center  = 8000;
    fb           = 10500;
    signalthreshold = 0.5;
    SamplesPerSymbol = 2.0 * Fs / fb;

    mixer_center.SetFreq(freq_center, Fs);
    mixer2.SetFreq(freq_center, Fs);

    spectrumnfft = (int)pow(2.0, SPECTRUM_FFT_POWER);
    spectrumcycbuff.resize(spectrumnfft, 0.0);
    spectrumcycbuff_ptr = 0;

    bbnfft = (int)pow(2.0, 14);
    bbcycbuff.resize(bbnfft, cpx_type(0, 0));
    bbcycbuff_ptr = 0;
    bbtmpbuff.resize(bbnfft, cpx_type(0, 0));

    agc = new AGC(1, Fs);

    ebnomeasure = new OQPSKEbNoMeasure((int)(0.5 * Fs), Fs, fb);

    marg = new MovingAverage(800);
    dt.setLength(400);

    phasepointbuff.resize(2, cpx_type(0, 0));
    phasepointbuff_ptr = 0;

    pointbuff.resize(300, cpx_type(0, 0));
    pointbuff_ptr = 0;

    msecalc = new MSEcalc(400);

    coarsefreqestimate = new CoarseFreqEstimate();
    coarsefreqestimate->setSettings(14, lockingbw, fb, Fs);

    RootRaisedCosine rrc;
    rrc.design(1, 55, Fs, 10500.0 / 2.0);
    fir_re = new FIR((int)rrc.Points.size());
    fir_im = new FIR((int)rrc.Points.size());
    for (int i = 0; i < (int)rrc.Points.size(); i++) {
        fir_re->FIRSetPoint(i, rrc.Points[i]);
        fir_im->FIRSetPoint(i, rrc.Points[i]);
    }

    /* Symbol timing delays */
    double T = Fs / 5250.0;
    delays.setdelay(1);
    delayt41.setdelay(T / 4.0);
    delayt42.setdelay(T / 4.0);
    delayt8.setdelay(T / 8.0);

    /* 10500 Hz resonator at 48000 sps */
    st_iir_resonator.a.resize(3);
    st_iir_resonator.b.resize(3);
    st_iir_resonator.b[0] =  0.00032714218939589035;
    st_iir_resonator.b[1] =  0.0;
    st_iir_resonator.b[2] =  0.00032714218939589035;
    st_iir_resonator.a[0] =  1.0;
    st_iir_resonator.a[1] = -0.39005299948210803;
    st_iir_resonator.a[2] =  0.99934571562120822;
    st_iir_resonator.init();

    ee = 0.4;

    st_osc.SetFreq(10500, Fs);
    st_osc_ref.SetFreq(10500, Fs);

    /* Carrier tracking LPF */
    ct_iir_loopfilter.a.resize(3);
    ct_iir_loopfilter.b.resize(3);
    ct_iir_loopfilter.b[0] =  0.0010275610653672064;
    ct_iir_loopfilter.b[1] =  0.0020551221307344128;
    ct_iir_loopfilter.b[2] =  0.0010275610653672064;
    ct_iir_loopfilter.a[0] =  1.0;
    ct_iir_loopfilter.a[1] = -1.9207386815577139;
    ct_iir_loopfilter.a[2] =  0.92509247310306331;
    ct_iir_loopfilter.init();

    bc.SetInNumberOfBits(1);
    bc.SetOutNumberOfBits(8);

    RxDataBits.reserve(64);

    /* Pre-filter for 8400 bps path */
    RootRaisedCosine rrc_pre_imp;
    rrc_pre_imp.design(1, 1024, Fs, 10500.0 / 2.0);
    fir_pre.SetKernel(rrc_pre_imp.Points);
    mixer_fir_pre.SetFreq(freq_center, Fs);

    coarseCounter = 0;
    feediq_phase  = 0.0;
    sig2_last     = cpx_type(0, 0);
    yui           = 0;
    pt_d          = cpx_type(0, 0);
    freqest_countdown  = 4;
    freqest_countdown2 = 5;
}

OqpskDemodulator::~OqpskDemodulator()
{
    delete agc;
    delete fir_re;
    delete fir_im;
    delete msecalc;
    delete marg;
    delete ebnomeasure;
    delete coarsefreqestimate;
}

void OqpskDemodulator::setAFC(bool state)       { afc = state; }
void OqpskDemodulator::setSQL(bool state)       { sql = state; }
void OqpskDemodulator::setCPUReduce(bool state) { cpuReduce = state; }
void OqpskDemodulator::setScatterPointType(ScatterPointType type) { scatterpointtype = (int)type; }
double OqpskDemodulator::getCurrentFreq()       { return mixer_center.GetFreqHz(); }
void OqpskDemodulator::invalidatesettings()     { Fs = -1; fb = -1; }

int OqpskDemodulator::get_audio_snapshot(double *out, int max_samples)
{
    if (!out || max_samples <= 0) return 0;
    int n = (int)spectrumcycbuff.size();
    if (n > max_samples) n = max_samples;
    int start = spectrumcycbuff_ptr % (int)spectrumcycbuff.size();
    for (int i = 0; i < n; i++) {
        int idx = (start + i) % (int)spectrumcycbuff.size();
        out[i] = spectrumcycbuff[idx];
    }
    return n;
}

int OqpskDemodulator::get_constellation_snapshot(double *out, int max_pairs)
{
    if (!out || max_pairs <= 0) return 0;
    int cap = (int)pointbuff.size();
    if (cap <= 0) return 0;
    int n = cap;
    if (n > max_pairs) n = max_pairs;
    for (int i = 0; i < n; i++) {
        cpx_type p = pointbuff[i];
        out[i * 2]     = p.real();
        out[i * 2 + 1] = p.imag();
    }
    return n;
}

void OqpskDemodulator::setManualTune(double audio_hz)
{
    /* Clamp to the same safe audio-band limits the AFC clamp uses. */
    if (audio_hz < 500.0) audio_hz = 500.0;
    if (audio_hz > Fs / 2.0 - 500.0) audio_hz = Fs / 2.0 - 500.0;
    mixer_center.SetFreq(audio_hz, Fs);
    mixer2.SetFreq(audio_hz, Fs);
    coarsefreqestimate->bigchange();
    for (size_t j = 0; j < bbcycbuff.size(); j++)
        bbcycbuff[j] = cpx_type(0, 0);
}

void OqpskDemodulator::setSoftBitsCallback(oqpsk_soft_bits_cb cb, void *user)
{
    soft_bits_cb   = cb;
    soft_bits_user = user;
}

void OqpskDemodulator::setSettings(Settings s)
{
    Fs           = s.Fs;
    lockingbw    = s.lockingbw;
    fb           = s.fb;
    freq_center  = s.freq_center;
    if (freq_center > ((Fs / 2.0) - (lockingbw / 2.0)))
        freq_center = (Fs / 2.0) - (lockingbw / 2.0);
    signalthreshold = s.signalthreshold;
    SamplesPerSymbol = 2.0 * Fs / fb;

    bbnfft = (int)pow(2.0, s.coarsefreqest_fft_power);
    bbcycbuff.assign(bbnfft, cpx_type(0, 0));
    bbcycbuff_ptr = 0;
    bbtmpbuff.resize(bbnfft);
    coarsefreqestimate->setSettings(s.coarsefreqest_fft_power,
                                    2.0 * lockingbw / 2.0, fb, Fs);

    mixer_center.SetFreq(freq_center, Fs);
    mixer2.SetFreq(freq_center, Fs);

    delete agc;
    agc = new AGC(1, Fs);

    pointbuff.assign(300, cpx_type(0, 0));
    pointbuff_ptr = 0;

    if (fir_re) delete fir_re;
    if (fir_im) delete fir_im;

    RootRaisedCosine rrc;
    if (fb == 8400.0)
        rrc.design(0.6, 55, Fs, fb / 2.0);
    else
        rrc.design(1.0, 55, Fs, fb / 2.0);
    fir_re = new FIR((int)rrc.Points.size());
    fir_im = new FIR((int)rrc.Points.size());
    for (int i = 0; i < (int)rrc.Points.size(); i++) {
        fir_re->FIRSetPoint(i, rrc.Points[i]);
        fir_im->FIRSetPoint(i, rrc.Points[i]);
    }

    /* Symbol timing delays */
    double T = Fs / (fb / 2.0);
    delays.setdelay(1);
    delayt41.setdelay(T / 4.0);
    delayt42.setdelay(T / 4.0);
    delayt8.setdelay(T / 8.0);

    st_iir_resonator.a.resize(3);
    st_iir_resonator.b.resize(3);

    if (fb == 8400.0) {
        /* 10 Hz bw resonator for 8400 bps */
        st_iir_resonator.b[0] =  0.0012845857864470789;
        st_iir_resonator.b[1] =  0.0;
        st_iir_resonator.b[2] = -0.0012845857864470789;
        st_iir_resonator.a[0] =  1.0;
        st_iir_resonator.a[1] = -0.90681461999279889;
        st_iir_resonator.a[2] =  0.99743082842710584;
        ee = 0.65;
    } else {
        st_iir_resonator.b[0] =  0.00032714218939589035;
        st_iir_resonator.b[1] =  0.0;
        st_iir_resonator.b[2] =  0.00032714218939589035;
        st_iir_resonator.a[0] =  1.0;
        st_iir_resonator.a[1] = -0.39005299948210803;
        st_iir_resonator.a[2] =  0.99934571562120822;
        ee = 0.4;
    }
    st_iir_resonator.init();

    st_osc.SetFreq(fb, Fs);
    st_osc_ref.SetFreq(fb, Fs);

    ebnomeasure->setup_update(Fs, fb);

    RootRaisedCosine rrc_pre_imp;
    if (fb == 8400.0)
        rrc_pre_imp.design(0.6, 2048, Fs, fb / 2.0);
    else
        rrc_pre_imp.design(1.0, 2048, Fs, fb / 2.0);
    fir_pre.SetKernel(rrc_pre_imp.Points, 4096);

    coarseCounter = 0;
}

/* Direct equivalent of JAERO's CenterFreqChangedSlot */
void OqpskDemodulator::DCDstatSlot(bool _dcd)
{
    dcd = _dcd;
}

/* Direct equivalent of JAERO's FreqOffsetEstimateSlot */
void OqpskDemodulator::FreqOffsetEstimateSlot(double freq_offset_est)
{
    /* Update pre-filter until we have DCD and signal */
    if ((mse > signalthreshold) || (!dcd)) {
        mixer_fir_pre.SetFreq(mixer_center.GetFreqHz() + freq_offset_est, Fs);
    }

    /* Prevent bad stable states (per-instance, not static) */
    if ((mse < signalthreshold) && (!dcd)) {
        if (freqest_countdown2 > 0) freqest_countdown2--;
        else {
            mixer2.SetFreq(mixer_center.GetFreqHz() + freq_offset_est);
        }
    } else freqest_countdown2 = 5;

    if ((mse > signalthreshold) &&
        (fabs(mixer2.GetFreqHz() - (mixer_center.GetFreqHz() + freq_offset_est)) > 3.0)) {
        mixer2.SetFreq(mixer_center.GetFreqHz() + freq_offset_est);
    }
    if (afc && (mse < signalthreshold) &&
        (fabs(mixer2.GetFreqHz() - mixer_center.GetFreqHz()) > 3.0)) {
        if (freqest_countdown > 0) freqest_countdown--;
        else {
            mixer_center.SetFreq(mixer2.GetFreqHz());
            /* Clamp mixer_center to ±lockingbw/2 around freq_center (the
             * intended audio position of the carrier) AND to a safe range
             * in [500, Fs/2-500] Hz where the Hilbert USB has clean
             * response. JAERO's original clamp used absolute Hz and broke
             * when lockingbw >= Fs/2 — the two bounds crossed and pulled
             * mixer_center to the wrong side of a real carrier. */
            double lo = freq_center - lockingbw / 2.0;
            double hi = freq_center + lockingbw / 2.0;
            if (lo < 500.0) lo = 500.0;
            if (hi > Fs / 2.0 - 500.0) hi = Fs / 2.0 - 500.0;
            if (mixer_center.GetFreqHz() < lo) mixer_center.SetFreq(lo);
            if (mixer_center.GetFreqHz() > hi) mixer_center.SetFreq(hi);
            coarsefreqestimate->bigchange();
            for (size_t j = 0; j < bbcycbuff.size(); j++)
                bbcycbuff[j] = cpx_type(0, 0);
        }
    } else freqest_countdown = 4;

    /* Signal status callback — matches JAERO's SignalStatus signal.
     * Only fires on edge transitions (good->bad or bad->good) to avoid
     * spamming AeroL with redundant resets. */
    if (sigstat_cb) {
        bool good = (mse < signalthreshold);
        if (good != sigstat_last) {
            sigstat_cb(good, sigstat_user);
            sigstat_last = good;
        }
    }
}

/* ORIGINAL JAERO OqpskDemodulator::writeData() DSP path — identical math,
 * Qt types replaced with std:: equivalents, signals replaced with direct
 * calls, GUI-only branches (scatter points, spectrum display) removed. */
void OqpskDemodulator::processAudio(const short *data, int num_samples)
{
    double lastmse = mse;
    int    len     = num_samples;  /* naming matches original for clarity */

    /* ---- Pre-filter (8400 bps path) ---- */
    std::vector<cpx_type> cval_prefiltered;
    if (fb == 8400.0) {
        cval_prefiltered.resize(len);
        const short *ptr2 = data;
        double savedphase = mixer_fir_pre.GetPhaseDeg();
        for (int i = 0; i < len; i++) {
            double dval = ((double)(*ptr2)) / 32768.0;
            cval_prefiltered[i] = mixer_fir_pre.WTCISValue() * dval;
            mixer_fir_pre.WTnextFrame();
            ptr2++;
        }
        /* filter in place */
        fir_pre.update(cval_prefiltered);
        /* up-convert back */
        mixer_fir_pre.SetPhaseDeg(savedphase);
        for (int i = 0; i < len; i++) {
            cval_prefiltered[i] *= mixer_fir_pre.WTCISValue_conj();
            mixer_fir_pre.WTnextFrame();
        }
    }

    double mixer2_freq_sum = 0.0;
    int i = 0;
    const short *ptr = data;

    for (i = 0; i < len; i++) {
        double dval = ((double)(*ptr)) / 32768.0;

        spectrumcycbuff[spectrumcycbuff_ptr] = dval;
        spectrumcycbuff_ptr++;
        spectrumcycbuff_ptr %= spectrumnfft;

        /* Coarse frequency estimator */
        if ((coarseCounter >= (int)Fs) || !cpuReduce) {
            bbcycbuff[bbcycbuff_ptr] = mixer_center.WTCISValue() * dval;
            bbcycbuff_ptr++;
            bbcycbuff_ptr %= bbnfft;
            if (bbcycbuff_ptr % (cpuReduce ? bbnfft : bbnfft / 4) == 0) {
                for (int j = 0; j < bbnfft; j++) {
                    bbtmpbuff[j] = bbcycbuff[bbcycbuff_ptr];
                    bbcycbuff_ptr++;
                    bbcycbuff_ptr %= bbnfft;
                }
                coarsefreqestimate->ProcessBasebandData(bbtmpbuff);
                FreqOffsetEstimateSlot(coarsefreqestimate->getFreqOffsetEst());
                coarseCounter = 0;
            }
        }
        coarseCounter++;

        /* ---- Mix and RRC filter ---- */
        cpx_type cval, sig2;
        if (fb == 8400.0) {
            sig2 = mixer2.WTCISValue() * cval_prefiltered[i];
            mixer2_freq_sum += mixer2.GetFreqHz();
        } else {
            cval = mixer2.WTCISValue() * dval;
            sig2 = cpx_type(fir_re->FIRUpdateAndProcess(cval.real()),
                            fir_im->FIRUpdateAndProcess(cval.imag()));
        }

        /* ---- EbNo and AGC ---- */
        double dabval = std::sqrt(sig2.real() * sig2.real() + sig2.imag() * sig2.imag());
        ebnomeasure->Update(dabval);
        sig2 *= agc->Update(dabval);

        /* Clipping */
        double abval = std::abs(sig2);
        if (abval > 2.84) sig2 = (2.84 / abval) * sig2;

        /* ---- Symbol timing ---- */
        double st_diff  = delays.update(abval * abval) - (abval * abval);
        double st_d1out = delayt41.update(st_diff);
        double st_d2out = delayt42.update(st_d1out);
        double st_eta   = (st_d2out - st_diff) * st_d1out;
        st_eta = st_iir_resonator.update(st_eta);
        cpx_type st_m1  = cpx_type(st_eta, -delayt8.update(st_eta));
        cpx_type st_out = st_osc.WTCISValue() * st_m1;
        double st_angle_error = std::arg(st_out);
        st_osc.IncreseFreqHz(-st_angle_error * 0.00000001);
        st_osc.AdvanceFractionOfWave(-st_angle_error * 0.01 / 360.0);
        if (st_osc.GetFreqHz() < (st_osc_ref.GetFreqHz() - 0.1))
            st_osc.SetFreq(st_osc_ref.GetFreqHz() - 0.1);
        if (st_osc.GetFreqHz() > (st_osc_ref.GetFreqHz() + 0.1))
            st_osc.SetFreq(st_osc_ref.GetFreqHz() + 0.1);

        /* ---- Symbol decision ---- */
        /* NOTE: these were 'static' in JAERO (single-instance). Must be
         * per-instance for our multi-channel architecture. Moved to
         * member variables sig2_last, yui, pt_d in the header. */
        if (st_osc.IfHavePassedPoint(ee)) {
            /* Interpolate */
            double pt_last = st_osc.FractionOfSampleItPassesBy;
            double pt_this = 1.0 - pt_last;
            cpx_type pt   = pt_this * sig2 + pt_last * sig2_last;

            yui++; yui %= 2;
            if (!yui) {
                this->pt_d = pt;
            } else {
                cpx_type pt_qpsk = cpx_type(pt.real(), this->pt_d.imag());

                /* Carrier tracking: BPSK 2x method */
                double ct_xt   = tanh(pt.imag()) * pt.real();
                double ct_xt_d = tanh(pt_d.real()) * pt_d.imag();
                double ct_ec   = ct_xt_d - ct_xt;
                if (ct_ec >  M_PI) ct_ec =  M_PI;
                if (ct_ec < -M_PI) ct_ec = -M_PI;

                if (fb > 8400.0) {
                    ct_ec = ct_iir_loopfilter.update(ct_ec);
                    if (ct_ec >  M_PI_2) ct_ec =  M_PI_2;
                    if (ct_ec < -M_PI_2) ct_ec = -M_PI_2;
                    mixer2.IncresePhaseDeg(1.0 * ct_ec);
                    mixer2.IncreseFreqHz(0.01 * ct_ec);
                } else {
                    mixer2.IncresePhaseDeg(1.0 * ct_ec);
                    mixer2.IncreseFreqHz(0.5 * 0.01 * ct_iir_loopfilter.update(ct_ec));
                }

                /* Rotate to remove any remaining bias */
                marg->UpdateSigned(ct_ec);
                dt.update(pt_qpsk);
                pt_qpsk *= cpx_type(cos(marg->Val), sin(marg->Val));

                /* Ring-buffer the per-symbol constellation point for the
                 * web UI scatter plot. Unconditional — inexpensive, and
                 * only read when the Spectrum tab is open. */
                if (!pointbuff.empty()) {
                    pointbuff[pointbuff_ptr] = pt_qpsk;
                    pointbuff_ptr = (pointbuff_ptr + 1) % (int)pointbuff.size();
                }

                /* MSE */
                mse = msecalc->Update(pt_qpsk);

                if (mse < signalthreshold) {
                    /* Soft bits: scale to [0,255], 128 = decision boundary */
                    int ibit = qRoundI(0.75 * pt_qpsk.imag() * 127.0 + 128.0);
                    if (ibit > 255) ibit = 255;
                    if (ibit <   0) ibit = 0;
                    RxDataBits.push_back((short)(unsigned char)ibit);

                    ibit = qRoundI(0.75 * pt_qpsk.real() * 127.0 + 128.0);
                    if (ibit > 255) ibit = 255;
                    if (ibit <   0) ibit = 0;
                    RxDataBits.push_back((short)(unsigned char)ibit);

                    if ((int)RxDataBits.size() >= 32) {
                        if (!sql || mse < signalthreshold || lastmse < signalthreshold) {
                            if (soft_bits_cb)
                                soft_bits_cb(RxDataBits.data(),
                                             (int)RxDataBits.size(),
                                             soft_bits_user);
                        }
                        RxDataBits.clear();
                    }
                }
            }
        }
        sig2_last = sig2;

        mixer2.WTnextFrame();
        mixer_center.WTnextFrame();
        st_osc.WTnextFrame();
        st_osc_ref.WTnextFrame();
        ptr++;
    }

    /* Update 8400 bps pre-filter carrier estimate */
    if (fb == 8400.0 && i > 0)
        mixer_fir_pre.SetFreq(mixer2_freq_sum / (double)i);
}

void OqpskDemodulator::feedAudio(const int16_t *samples, int num_samples, int sample_rate)
{
    (void)sample_rate;
    processAudio(samples, num_samples);
}

/* feedIQ: mix baseband complex IQ → int16 audio via Hilbert USB demod at
 * freq_center Hz (same 125-tap path as ZMQ output), then processAudio.
 * Measured ~1.5 dB better Eb/No than the prior real-mixer on ch12. */
void OqpskDemodulator::feedIQ(const double *iq_interleaved, int num_samples)
{
    std::vector<int16_t> pcm(num_samples);
    feediq_usb.process(iq_interleaved, num_samples, Fs, freq_center, 5.0, pcm.data());
    processAudio(pcm.data(), num_samples);
}
