/*
 * MskDemodulator -- JAERO's continuous MSK demodulator for P-channel.
 * Ported from github.com/jontio/JAERO, Qt-stripped to pure C++ with C callback.
 * SPDX-License-Identifier: MIT (JAERO original)
 *
 * This port uses the ORIGINAL JAERO processAudio() monolithic loop exactly
 * as it appears in JAERO desktop — the path that is known to decode when
 * fed real-valued audio centered at freq_center Hz. feedIQ is a thin
 * wrapper: mix IQ to int16 audio at freq_center Hz, then feedAudio.
 */

#include "mskdemodulator.h"
#include "coarsefreqestimate.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

static inline int qRoundI(double v) { return (int)(v + (v >= 0 ? 0.5 : -0.5)); }

MskDemodulator::MskDemodulator()
{
    soft_bits_cb = NULL;
    soft_bits_user = NULL;
    sigstat_cb = NULL;
    sigstat_user = NULL;
    sigstat_last = true;

    afc = false;
    sql = false;
    cpuReduce = false;

    Fs = 48000;
    freq_center = 1000.0;
    lockingbw = 900;
    fb = 600;

    signalthreshold = 0.5;

    SamplesPerSymbol = Fs / fb;

    scatterpointtype = SPT_constellation;

    bc.SetInNumberOfBits(1);
    bc.SetOutNumberOfBits(8);

    matchedfilter_re = new FIR(2 * SamplesPerSymbol);
    matchedfilter_im = new FIR(2 * SamplesPerSymbol);
    for (int i = 0; i < 2 * SamplesPerSymbol; i++) {
        matchedfilter_re->FIRSetPoint(i, sin(M_PI * i / (2.0 * SamplesPerSymbol)) / (2.0 * SamplesPerSymbol));
        matchedfilter_im->FIRSetPoint(i, sin(M_PI * i / (2.0 * SamplesPerSymbol)) / (2.0 * SamplesPerSymbol));
    }

    agc = new AGC(1, Fs);
    ebnomeasure = new MSKEbNoMeasure(0.5 * Fs);
    pointmean = new MovingAverage(100);

    mixer_center.SetFreq(freq_center, Fs);
    mixer2.SetFreq(freq_center, Fs);
    st_osc.SetFreq(fb / 2, Fs);

    bbnfft = pow(2, 14);
    bbcycbuff.resize(bbnfft);
    bbcycbuff_ptr = 0;
    bbtmpbuff.resize(bbnfft);

    spectrumnfft = pow(2, 13);
    spectrumcycbuff.resize(spectrumnfft);
    spectrumcycbuff_ptr = 0;
    spectrumtmpbuff.resize(spectrumnfft);

    pointbuff.resize(100);
    pointbuff_ptr = 0;
    mse = 10.0;
    msema = new MovingAverage(600);

    lastindex = 0;
    singlepointphasevector.resize(1);

    marg = new MovingAverage(80);
    dt.setLength(40);

    RxDataBits.reserve(100);

    coarsefreqestimate = new CoarseFreqEstimate();
    coarsefreqestimate->setSettings(14, lockingbw, fb, Fs);

    dcd = false;
    correctionfactor = 1.0;
    coarseCounter = 0;
    feediq_phase = 0.0;
    freqest_countdown = 4;
}

MskDemodulator::~MskDemodulator()
{
    delete matchedfilter_re;
    delete matchedfilter_im;
    delete agc;
    delete msema;
    delete ebnomeasure;
    delete pointmean;
    delete marg;
    delete coarsefreqestimate;
}

void MskDemodulator::setSettings(Settings s)
{
    last_applied_settings = s;
    Fs = s.Fs;
    lockingbw = s.lockingbw;
    fb = s.fb;
    freq_center = s.freq_center;
    if (freq_center > (Fs / 2.0 - lockingbw / 2.0))
        freq_center = Fs / 2.0 - lockingbw / 2.0;
    signalthreshold = s.signalthreshold;

    SamplesPerSymbol = (int)(Fs / fb);
    bbnfft = pow(2, s.coarsefreqest_fft_power);
    bbcycbuff.assign(bbnfft, cpx_type(0, 0));
    bbcycbuff_ptr = 0;
    bbtmpbuff.resize(bbnfft);
    coarsefreqestimate->setSettings(s.coarsefreqest_fft_power, lockingbw, fb, Fs);

    mixer_center.SetFreq(freq_center, Fs);
    mixer2.SetFreq(freq_center, Fs);
    st_osc.SetFreq(fb / 2, Fs);

    delete matchedfilter_re;
    delete matchedfilter_im;
    matchedfilter_re = new FIR(2 * SamplesPerSymbol);
    matchedfilter_im = new FIR(2 * SamplesPerSymbol);
    for (int i = 0; i < 2 * SamplesPerSymbol; i++) {
        matchedfilter_re->FIRSetPoint(i, sin(M_PI * i / (2.0 * SamplesPerSymbol)) / (2.0 * SamplesPerSymbol));
        matchedfilter_im->FIRSetPoint(i, sin(M_PI * i / (2.0 * SamplesPerSymbol)) / (2.0 * SamplesPerSymbol));
    }

    delete agc;
    agc = new AGC(1, Fs);

    delete ebnomeasure;
    ebnomeasure = new MSKEbNoMeasure(0.5 * Fs);

    pointbuff.assign(100, cpx_type(0, 0));
    pointbuff_ptr = 0;
    mse = 10.0;

    lastindex = 0;

    st_iir_resonator.a.resize(3);
    st_iir_resonator.b.resize(3);

    if (fb >= 1200) {
        correctionfactor = 0.6;
        if (Fs == 48000) {
            st_iir_resonator.a[0] = 1;
            st_iir_resonator.a[1] = -1.993312819378528;
            st_iir_resonator.a[2] = 0.999476538254407;
            st_iir_resonator.b[0] = 2.617308727964618e-04;
            st_iir_resonator.b[1] = 0;
            st_iir_resonator.b[2] = -2.617308727964618e-04;
            ee = 0.025;
        } else {
            st_iir_resonator.a[0] = 1;
            st_iir_resonator.a[1] = -1.974342917561558;
            st_iir_resonator.a[2] = 0.998953350377616;
            st_iir_resonator.b[0] = 5.233248111921052e-04;
            st_iir_resonator.b[1] = 0;
            st_iir_resonator.b[2] = -5.233248111921052e-04;
            ee = 0.05;
        }
    } else {
        correctionfactor = 1.0;
        if (Fs == 48000) {
            st_iir_resonator.a[0] = 1;
            st_iir_resonator.a[1] = -1.998196509168551;
            st_iir_resonator.a[2] = 0.999738234875681;
            st_iir_resonator.b[0] = 1.308825621597620e-04;
            st_iir_resonator.b[1] = 0;
            st_iir_resonator.b[2] = -1.308825621597620e-04;
            ee = 0.025;
        } else {
            st_iir_resonator.a[0] = 1;
            st_iir_resonator.a[1] = -1.974342917561558;
            st_iir_resonator.a[2] = 0.998953350377616;
            st_iir_resonator.b[0] = 5.233248111921052e-04;
            st_iir_resonator.b[1] = 0;
            st_iir_resonator.b[2] = -5.233248111921052e-04;
            ee = 0.0125;
        }
    }
    st_iir_resonator.init();

    delete marg;
    marg = new MovingAverage(SamplesPerSymbol);
    dt.setLength(SamplesPerSymbol / 2);
    delayedsmpl.setLength(SamplesPerSymbol);
    delayt8.setdelay(SamplesPerSymbol / 2.0);
    coarseCounter = 0;
}

void MskDemodulator::invalidatesettings() { Fs = -1; fb = -1; }
void MskDemodulator::setAFC(bool s) { afc = s; }
void MskDemodulator::setSQL(bool s) { sql = s; }
void MskDemodulator::setCPUReduce(bool s) { cpuReduce = s; }
void MskDemodulator::setScatterPointType(ScatterPointType t) { scatterpointtype = (int)t; }
double MskDemodulator::getCurrentFreq() { return mixer_center.GetFreqHz(); }

int MskDemodulator::get_audio_snapshot(double *out, int max_samples)
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

int MskDemodulator::get_constellation_snapshot(double *out, int max_pairs)
{
    if (!out || max_pairs <= 0) return 0;
    int cap = (int)pointbuff.size();
    if (cap <= 0) return 0;
    int n = cap;
    if (n > max_pairs) n = max_pairs;
    /* pointbuff is written by the DSP thread; we do an unlocked read
     * since a half-updated point renders as a misplaced dot at worst. */
    for (int i = 0; i < n; i++) {
        cpx_type p = pointbuff[i];
        out[i * 2]     = p.real();
        out[i * 2 + 1] = p.imag();
    }
    return n;
}

void MskDemodulator::setManualTune(double audio_hz)
{
    if (audio_hz < 500.0) audio_hz = 500.0;
    if (audio_hz > Fs / 2.0 - 500.0) audio_hz = Fs / 2.0 - 500.0;
    mixer_center.SetFreq(audio_hz, Fs);
    mixer2.SetFreq(audio_hz, Fs);
    if (coarsefreqestimate) coarsefreqestimate->bigchange();
    for (size_t j = 0; j < bbcycbuff.size(); j++)
        bbcycbuff[j] = cpx_type(0, 0);
}

void MskDemodulator::setSoftBitsCallback(msk_soft_bits_cb cb, void *user)
{
    soft_bits_cb = cb;
    soft_bits_user = user;
}

void MskDemodulator::CenterFreqChangedSlot(double f)
{
    if (f < 0.75 * fb) f = 0.75 * fb;
    if (f > (Fs / 2.0 - 0.75 * fb)) f = Fs / 2.0 - 0.75 * fb;
    mixer_center.SetFreq(f, Fs);
    if (afc) mixer2.SetFreq(mixer_center.GetFreqHz());
    if ((mixer2.GetFreqHz() - mixer_center.GetFreqHz()) > (lockingbw / 2.0))
        mixer2.SetFreq(mixer_center.GetFreqHz() + (lockingbw / 2.0));
    if ((mixer2.GetFreqHz() - mixer_center.GetFreqHz()) < (-lockingbw / 2.0))
        mixer2.SetFreq(mixer_center.GetFreqHz() - (lockingbw / 2.0));
    for (size_t j = 0; j < bbcycbuff.size(); j++)
        bbcycbuff[j] = cpx_type(0, 0);
}

void MskDemodulator::FreqOffsetEstimateSlot(double freq_offset_est)
{
    /* Was static in JAERO (single-instance). Must be per-instance. */
    int &countdown = this->freqest_countdown;
    if ((mse > signalthreshold) &&
        (fabs(mixer2.GetFreqHz() - (mixer_center.GetFreqHz() + freq_offset_est)) > 0.0)) {
        mixer2.SetFreq(mixer_center.GetFreqHz() + freq_offset_est);
    }
    if (afc && dcd && fabs(mixer2.GetFreqHz() - mixer_center.GetFreqHz()) > 2.0) {
        if (countdown > 0) countdown--;
        else {
            mixer_center.SetFreq(mixer2.GetFreqHz());
            if (mixer_center.GetFreqHz() < lockingbw / 2.0)
                mixer_center.SetFreq(lockingbw / 2.0);
            if (mixer_center.GetFreqHz() > (Fs / 2.0 - lockingbw / 2.0))
                mixer_center.SetFreq(Fs / 2.0 - lockingbw / 2.0);
            coarsefreqestimate->bigchange();
            for (size_t j = 0; j < bbcycbuff.size(); j++)
                bbcycbuff[j] = cpx_type(0, 0);
        }
    } else countdown = 4;

    /* Signal status edge callback — matches JAERO's SignalStatus signal */
    if (sigstat_cb) {
        bool good = (mse < signalthreshold);
        if (good != sigstat_last) {
            sigstat_cb(good, sigstat_user);
            sigstat_last = good;
        }
    }
}

/* ORIGINAL JAERO processAudio — monolithic, unmodified. Operates on int16
 * mono audio at Fs with signal centered at freq_center Hz. */
void MskDemodulator::processAudio(const short *ptr, int numofsamples)
{
    for (int i = 0; i < numofsamples; i++) {
        double dval = (double)(*ptr) / 32768.0;

        spectrumcycbuff[spectrumcycbuff_ptr] = dval;
        spectrumcycbuff_ptr++;
        spectrumcycbuff_ptr %= spectrumnfft;

        if (coarseCounter >= Fs || !cpuReduce) {
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

        cpx_type cval = mixer2.WTCISValue() * dval;
        cpx_type sig2 = cpx_type(
            matchedfilter_re->FIRUpdateAndProcess(cval.real()),
            matchedfilter_im->FIRUpdateAndProcess(cval.imag()));

        double dabval = std::sqrt(sig2.real() * sig2.real() + sig2.imag() * sig2.imag());

        ebnomeasure->Update(dabval);
        sig2 *= agc->Update(dabval);

        double abval = std::sqrt(sig2.real() * sig2.real() + sig2.imag() * sig2.imag());
        if (abval > 2.84) sig2 = (2.84 / abval) * sig2;

        cpx_type _pt_d = delayedsmpl.update_dont_touch(sig2);
        cpx_type pt_msk = cpx_type(sig2.real(), _pt_d.imag());

        double st_eta = st_iir_resonator.update(std::abs(pt_msk));
        cpx_type st_m1 = cpx_type(st_eta, -delayt8.update(st_eta));
        cpx_type st_out = st_osc.WTCISValue() * st_m1;

        double st_angle_error = std::arg(st_out);
        double weighting = fabs(tanh(st_angle_error));

        if (!dcd)
            st_osc.AdvanceFractionOfWave(-(1.0 - weighting) * st_angle_error * (0.05 / 360.0));
        else
            st_osc.AdvanceFractionOfWave(-(1.0 - weighting) * st_angle_error * (0.003 / 360.0));

        if (st_osc.IfHavePassedPoint(ee)) {
            double ct_xt = tanh(sig2.imag()) * sig2.real();
            double ct_xt_d = tanh(_pt_d.real()) * _pt_d.imag();
            double ct_ec = ct_xt_d - ct_xt;

            if (ct_ec > M_PI) ct_ec = M_PI;
            if (ct_ec < -M_PI) ct_ec = -M_PI;
            if (ct_ec > M_PI_2) ct_ec = M_PI_2;
            if (ct_ec < -M_PI_2) ct_ec = -M_PI_2;

            double carrier_aggression = 12.0 * correctionfactor;
            if (dcd) carrier_aggression = 8.0 * correctionfactor;
            mixer2.IncresePhaseDeg(carrier_aggression * 1.0 * ct_ec);
            mixer2.IncreseFreqHz(carrier_aggression * 0.01 * ct_ec);

            marg->UpdateSigned(ct_ec / 2.0);
            dt.update(pt_msk);
            pt_msk *= cpx_type(cos(marg->Val), sin(marg->Val));

            /* Ring-buffer the per-symbol constellation point for the web UI
             * scatter plot. Always on — cheap, used only when someone opens
             * the Spectrum tab. */
            if (!pointbuff.empty()) {
                pointbuff[pointbuff_ptr] = pt_msk;
                pointbuff_ptr = (pointbuff_ptr + 1) % (int)pointbuff.size();
            }

            double tda = (fabs(pt_msk.real() * 0.75) - 1.0);
            double tdb = (fabs(pt_msk.imag() * 0.75) - 1.0);
            mse = msema->Update((tda * tda) + (tdb * tdb));

            double imagin = diffdecode.UpdateSoft(pt_msk.imag());
            int ibit = qRoundI(imagin * 127.0 + 128.0);
            if (ibit > 255) ibit = 255;
            if (ibit < 0) ibit = 0;
            RxDataBits.push_back((short)ibit);

            double real = diffdecode.UpdateSoft(pt_msk.real());
            real = -real;
            ibit = qRoundI(real * 127.0 + 128.0);
            if (ibit > 255) ibit = 255;
            if (ibit < 0) ibit = 0;
            RxDataBits.push_back((short)ibit);

            if ((int)RxDataBits.size() >= 12) {
                if (soft_bits_cb)
                    soft_bits_cb(RxDataBits.data(), (int)RxDataBits.size(), soft_bits_user);
                RxDataBits.clear();
            }
        }

        mixer2.WTnextFrame();
        mixer_center.WTnextFrame();
        st_osc.WTnextFrame();
        ptr++;
    }
}

void MskDemodulator::feedAudio(const int16_t *samples, int num_samples, int sample_rate)
{
    (void)sample_rate;
    processAudio(samples, num_samples);
}

/* feedIQ: mix baseband IQ to int16 audio at freq_center via 125-tap
 * Hilbert USB demod (same as SDRReceiver/ZMQ path), then processAudio.
 * Measured ~1.5 dB better Eb/No than plain `re*cos - im*sin` on ch12. */
void MskDemodulator::feedIQ(const double *iq_interleaved, int num_samples)
{
    std::vector<int16_t> pcm(num_samples);
    feediq_usb.process(iq_interleaved, num_samples, Fs, freq_center, 5.0, pcm.data());
    processAudio(pcm.data(), num_samples);
}

