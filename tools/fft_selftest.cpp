// Offline self-test for the InmarScope spectrum pipeline.
// Verifies the window + FFT + fftshift + power-dB path produces no spurious
// null/spike at the center bin for clean (DC-free) input, and that the DC
// patch removes a synthetic DC spike. Runs headless; prints stats.

#include "dsp/jfft.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void patchDcBins(std::vector<float>& a, int N, int w)
{
    int c = N / 2;
    int span = 2 * w + 1;
    int src = c - w - span;
    if (src < 0 || c + w >= N)
        return;
    for (int k = 0; k < span; ++k)
        a[c - w + k] = a[src + k];
}

static float percentile(std::vector<float> v, double q)
{
    int idx = (int)(q * v.size());
    std::nth_element(v.begin(), v.begin() + idx, v.end());
    return v[idx];
}

static void run(const char* label, bool addDc, bool patch)
{
    const int N = 4096;
    std::vector<double> win(N);
    for (int i = 0; i < N; ++i)
    {
        double x = 2.0 * M_PI * i / (N - 1);
        win[i] = 0.42 - 0.5 * std::cos(x) + 0.08 * std::cos(2.0 * x);
    }

    std::mt19937 rng(12345);
    std::normal_distribution<double> nd(0.0, 0.02);

    std::vector<std::complex<double>> iq(N);
    int toneBin = 200; // +200 bins from DC
    double w0 = 2.0 * M_PI * toneBin / N;
    for (int i = 0; i < N; ++i)
    {
        std::complex<double> s(nd(rng), nd(rng));
        s += 0.3 * std::complex<double>(std::cos(w0 * i), std::sin(w0 * i)); // tone
        if (addDc)
            s += std::complex<double>(0.5, -0.3); // DC offset (DC blocker off)
        iq[i] = s * win[i];
    }

    JFFT fft;
    int nf = N;
    fft.init(nf);
    fft.fft(iq.data(), N, JFFT::FORWARD);

    std::vector<float> db(N);
    const double invN = 1.0 / N;
    for (int i = 0; i < N; ++i)
    {
        int src = (i + N / 2) % N;
        double p = std::norm(iq[src]) * invN;
        db[i] = (float)(10.0 * std::log10(p + 1e-20));
    }

    if (patch)
        patchDcBins(db, N, 4);

    int c = N / 2;
    int toneIdx = c + toneBin; // tone should land here after fftshift
    float floorDb = percentile(db, 0.30);
    float peakDb = *std::max_element(db.begin(), db.end());

    std::printf("[%s] floor(p30)=%.1f  peak=%.1f  tone@%d=%.1f\n",
                label, floorDb, peakDb, toneIdx, db[toneIdx]);
    std::printf("    center bins: ");
    for (int k = -3; k <= 3; ++k)
        std::printf("%.1f ", db[c + k]);
    std::printf("\n    center vs floor: %.1f dB (%s)\n",
                db[c] - floorDb,
                std::fabs(db[c] - floorDb) < 6.0f ? "OK - no artifact"
                                                  : "ARTIFACT at center");
}

int main()
{
    run("clean, DC-free, no patch ", false, false);
    run("DC offset (blocker off)   ", true, false);
    run("DC offset + patch         ", true, true);
    return 0;
}
