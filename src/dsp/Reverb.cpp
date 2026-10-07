/*
 * OkumuLab 1 — the room (see Reverb.h)
 */
#include "Reverb.h"
#include "Simd.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>

namespace okl
{

namespace
{
constexpr double kPi = 3.14159265358979323846;
}

/* ------------------------------------------------------------------ FFT */
void RealFFT::init (int size)
{
    n = size;
    m = n / 2;
    cosT.resize ((size_t) m / 2 + 1);
    sinT.resize ((size_t) m / 2 + 1);
    for (int k = 0; k <= m / 2; ++k)
    {
        cosT[(size_t) k] = (float) std::cos (2.0 * kPi * k / m);
        sinT[(size_t) k] = (float) std::sin (2.0 * kPi * k / m);
    }
    wr.resize ((size_t) m + 1);
    wi.resize ((size_t) m + 1);
    for (int k = 0; k <= m; ++k)
    {
        wr[(size_t) k] = (float) std::cos (2.0 * kPi * k / n);
        wi[(size_t) k] = (float) -std::sin (2.0 * kPi * k / n);
    }
    rev.resize ((size_t) m);
    int bits = 0;
    while ((1 << bits) < m) ++bits;
    for (int i = 0; i < m; ++i)
    {
        int r = 0;
        for (int b = 0; b < bits; ++b) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
        rev[(size_t) i] = r;
    }
    zr.assign ((size_t) m, 0.0f);
    zi.assign ((size_t) m, 0.0f);
}

/* in-place radix-2 complex FFT of size m (forward: e^{-i}, no scaling) */
void RealFFT::cfft (float* r, float* im, bool inv)
{
    for (int i = 0; i < m; ++i)
    {
        const int j = rev[(size_t) i];
        if (j > i) { std::swap (r[i], r[j]); std::swap (im[i], im[j]); }
    }
    for (int len = 2; len <= m; len <<= 1)
    {
        const int half = len >> 1, stp = m / len;
        for (int i = 0; i < m; i += len)
        {
            for (int k = 0; k < half; ++k)
            {
                // twiddle e^{-2 pi i k / len} = table at k * stp (m-point); the second quarter from symmetry
                const int idx = k * stp;
                const float c = cosT[(size_t) idx];          // idx < m / 2 always (k < len / 2)
                const float s = inv ? sinT[(size_t) idx] : -sinT[(size_t) idx];
                const int a = i + k, b = a + half;
                const float xr = r[b] * c - im[b] * s, xi = r[b] * s + im[b] * c;
                r[b] = r[a] - xr; im[b] = im[a] - xi;
                r[a] += xr; im[a] += xi;
            }
        }
    }
}

void RealFFT::forward (const float* in, float* re, float* im)
{
    for (int k = 0; k < m; ++k) { zr[(size_t) k] = in[2 * k]; zi[(size_t) k] = in[2 * k + 1]; }
    cfft (zr.data(), zi.data(), false);
    for (int k = 0; k <= m; ++k)
    {
        const int a = k == m ? 0 : k, b = (m - k) % m;
        // E = (Z[k] + conj Z[m-k]) / 2, O = (Z[k] - conj Z[m-k]) / 2i
        const float er = 0.5f * (zr[(size_t) a] + zr[(size_t) b]), ei = 0.5f * (zi[(size_t) a] - zi[(size_t) b]);
        const float orr = 0.5f * (zi[(size_t) a] + zi[(size_t) b]), oi = -0.5f * (zr[(size_t) a] - zr[(size_t) b]);
        const float c = wr[(size_t) k], s = wi[(size_t) k];
        re[k] = er + (orr * c - oi * s);
        im[k] = ei + (orr * s + oi * c);
    }
}

void RealFFT::inverse (const float* re, const float* im, float* out)
{
    for (int k = 0; k < m; ++k)
    {
        const int b = m - k;
        // E = (X[k] + conj X[m-k]) / 2, O = (X[k] - conj X[m-k]) / 2 * W^{-k}; Z = E + i O
        const float er = 0.5f * (re[k] + re[b]), ei = 0.5f * (im[k] - im[b]);
        const float dr = 0.5f * (re[k] - re[b]), di = 0.5f * (im[k] + im[b]);
        const float c = wr[(size_t) k], s = -wi[(size_t) k];          // W^{-k}
        const float orr = dr * c - di * s, oi = dr * s + di * c;
        zr[(size_t) k] = er - oi;
        zi[(size_t) k] = ei + orr;
    }
    cfft (zr.data(), zi.data(), true);
    const float sc = 1.0f / (float) m;
    for (int k = 0; k < m; ++k) { out[2 * k] = zr[(size_t) k] * sc; out[2 * k + 1] = zi[(size_t) k] * sc; }
}

/* ------------------------------------------------------------------ air (ISO 9613-1) */
double airAbsorptionDbPerM (double f, double tempC, double hr, double paKPa)
{
    const double T = tempC + 273.15, T0 = 293.15, T01 = 273.16, pr = 101.325, pa = paKPa;
    const double C = -6.8346 * std::pow (T01 / T, 1.261) + 4.6151;
    const double h = hr * std::pow (10.0, C) * (pr / pa);          // molar concentration of water vapour [%]
    const double frO = (pa / pr) * (24.0 + 4.04e4 * h * (0.02 + h) / (0.391 + h));
    const double frN = (pa / pr) * std::pow (T / T0, -0.5) * (9.0 + 280.0 * h * std::exp (-4.170 * (std::pow (T / T0, -1.0 / 3.0) - 1.0)));
    return 8.686 * f * f * (1.84e-11 * (pr / pa) * std::sqrt (T / T0)
                            + std::pow (T / T0, -2.5) * (0.01275 * std::exp (-2239.1 / T) / (frO + f * f / frO)
                                                         + 0.1068 * std::exp (-3352.0 / T) / (frN + f * f / frN)));
}

/* ------------------------------------------------------------------ the nave */
RoomSpec defaultRoom()
{
    RoomSpec r;
    // absorption coefficients, octave bands 63 Hz .. 8 kHz (usual table values)
    static const double masonry[8] { 0.02, 0.02, 0.02, 0.03, 0.04, 0.05, 0.05, 0.05 };       // plastered stone
    static const double glass[8] { 0.35, 0.35, 0.25, 0.18, 0.12, 0.07, 0.04, 0.03 };         // tall leaded windows
    static const double wood[8] { 0.30, 0.28, 0.22, 0.17, 0.09, 0.10, 0.11, 0.11 };          // organ case, panelling
    static const double stone[8] { 0.01, 0.01, 0.01, 0.015, 0.02, 0.02, 0.02, 0.02 };        // stone floor
    static const double pews[8] { 0.25, 0.35, 0.55, 0.72, 0.78, 0.80, 0.76, 0.72 };          // pews with the congregation
    for (int b = 0; b < 8; ++b)
    {
        r.alpha[0][b] = 0.6 * masonry[b] + 0.4 * wood[b];        // west wall: the organ gallery
        r.alpha[1][b] = masonry[b];                              // east wall: the apse
        r.alpha[2][b] = r.alpha[3][b] = 0.8 * masonry[b] + 0.2 * glass[b];   // side walls with windows
        r.alpha[4][b] = 0.35 * stone[b] + 0.65 * pews[b];        // floor: aisles and occupied pews
        r.alpha[5][b] = masonry[b];                              // vault
    }
    return r;
}

namespace
{
/* surface areas in the order of RoomSpec::alpha */
void areas (const RoomSpec& r, double* A)
{
    A[0] = A[1] = r.Ly * r.Lz;
    A[2] = A[3] = r.Lx * r.Lz;
    A[4] = A[5] = r.Lx * r.Ly;
}

/* octave-band value at f (log-frequency interpolation, flat beyond the ends) */
double bandAt (const double* v, double f)
{
    if (f <= kBandHz[0]) return v[0];
    if (f >= kBandHz[7]) return v[7];
    const double x = std::log2 (f / kBandHz[0]);
    const int b = std::min (6, (int) x);
    const double t = x - b;
    return v[b] * (1.0 - t) + v[b + 1] * t;
}

/* Eyring reverberation time at f: surfaces and air */
double eyringT60 (const RoomSpec& r, double f, double c)
{
    double A[6], S = 0, aS = 0;
    areas (r, A);
    for (int w = 0; w < 6; ++w) { S += A[w]; aS += A[w] * bandAt (r.alpha[w], f); }
    const double V = r.Lx * r.Ly * r.Lz;
    const double m = airAbsorptionDbPerM (f, r.tempC, r.humidity) / (10.0 * std::log10 (std::exp (1.0)));     // energy, 1/m
    return 24.0 * std::log (10.0) * V / (c * (-S * std::log (1.0 - aS / S) + 4.0 * m * V));
}

struct Rng
{
    uint64_t s;
    explicit Rng (uint64_t seed) : s (seed ? seed : 0x9e3779b97f4a7c15ull) {}
    double uni() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return ((s >> 11) + 0.5) * (1.0 / 9007199254740992.0); }
    double gauss() { const double u = uni(), v = uni(); return std::sqrt (-2.0 * std::log (u)) * std::cos (2.0 * kPi * v); }
};
} // namespace

void roomImpulseResponse (const RoomSpec& r, double fs, std::vector<float>& L, std::vector<float>& R, RoomInfo* info)
{
    const double c = 331.3 * std::sqrt (1.0 + r.tempC / 273.15);
    const double V = r.Lx * r.Ly * r.Lz;
    double A[6], S = 0;
    areas (r, A);
    for (double a : A) S += a;
    const double lmfp = 4.0 * V / S;
    // the two microphones (left = +y half spacing) and the direct path to the centre of the pair
    const double mics[2][3] { { r.mic[0], r.mic[1] + 0.5 * r.micSpacing, r.mic[2] }, { r.mic[0], r.mic[1] - 0.5 * r.micSpacing, r.mic[2] } };
    const double dDir = std::sqrt (std::pow (r.mic[0] - r.src[0], 2) + std::pow (r.mic[1] - r.src[1], 2) + std::pow (r.mic[2] - r.src[2], 2));
    double tMax = 0;
    for (int b = 0; b < RoomSpec::kBands; ++b) tMax = std::max (tMax, eyringT60 (r, kBandHz[b], c));
    const double dur = std::min (6.0, 70.0 / 60.0 * tMax);
    const int N = (int) std::ceil (dur * fs);
    std::vector<double> out[2] { std::vector<double> ((size_t) N, 0.0), std::vector<double> ((size_t) N, 0.0) };
    const double tFade = 0.02;

    // ---- early reflections: image sources, each a 64-tap linear-phase filter at its exact arrival time
    int images = 0;
    double firstRefl = 1e9;
    constexpr int kF = 64;
    for (int ch = 0; ch < 2; ++ch)
    {
        const double* rc = mics[ch];
        const double rMax = dDir + c * (r.mixingTime + tFade);
        const int nx = (int) std::ceil (rMax / (2 * r.Lx)) + 1, ny = (int) std::ceil (rMax / (2 * r.Ly)) + 1, nz = (int) std::ceil (rMax / (2 * r.Lz)) + 1;
        for (int ix = -nx; ix <= nx; ++ix)
            for (int qx = 0; qx < 2; ++qx)
                for (int iy = -ny; iy <= ny; ++iy)
                    for (int qy = 0; qy < 2; ++qy)
                        for (int iz = -nz; iz <= nz; ++iz)
                            for (int qz = 0; qz < 2; ++qz)
                            {
                                if (ix == 0 && iy == 0 && iz == 0 && qx == 0 && qy == 0 && qz == 0) continue;     // the direct sound
                                const double px = (1 - 2 * qx) * r.src[0] + 2 * ix * r.Lx;
                                const double py = (1 - 2 * qy) * r.src[1] + 2 * iy * r.Ly;
                                const double pz = (1 - 2 * qz) * r.src[2] + 2 * iz * r.Lz;
                                const double d = std::sqrt ((px - rc[0]) * (px - rc[0]) + (py - rc[1]) * (py - rc[1]) + (pz - rc[2]) * (pz - rc[2]));
                                const double tau = (d - dDir) / c;
                                if (tau > r.mixingTime + tFade || tau < 0) continue;
                                const int cnt[6] { std::abs (ix - qx), std::abs (ix), std::abs (iy - qy), std::abs (iy), std::abs (iz - qz), std::abs (iz) };
                                const int order = cnt[0] + cnt[1] + cnt[2] + cnt[3] + cnt[4] + cnt[5];
                                // what reaches the microphone specularly, by frequency (amplitude)
                                double g[kF / 2 + 1];
                                for (int k = 0; k <= kF / 2; ++k)
                                {
                                    const double f = std::max (20.0, k * fs / kF);
                                    double a = 1.0 / d;
                                    for (int w = 0; w < 6; ++w) if (cnt[w]) a *= std::pow (1.0 - bandAt (r.alpha[w], f), 0.5 * cnt[w]);
                                    a *= std::pow (1.0 - r.scattering, 0.5 * order);
                                    a *= std::pow (10.0, -airAbsorptionDbPerM (f, r.tempC, r.humidity) * d / 20.0);
                                    g[k] = a;
                                }
                                const double wImg = tau < r.mixingTime ? 1.0 : 0.5 * (1.0 + std::cos (kPi * (tau - r.mixingTime) / tFade));
                                const double pos = tau * fs;
                                const int i0 = (int) std::floor (pos);
                                const double frac = pos - i0;
                                for (int nTap = 0; nTap < kF; ++nTap)
                                {
                                    const double x = nTap - kF / 2 - frac;                // time from the arrival [samples]
                                    double hsum = g[0];
                                    for (int k = 1; k < kF / 2; ++k) hsum += 2.0 * g[k] * std::cos (2.0 * kPi * k * x / kF);
                                    hsum += g[kF / 2] * std::cos (kPi * x);
                                    const double win = std::abs (x) < kF / 2 ? 0.5 * (1.0 + std::cos (2.0 * kPi * x / kF)) : 0.0;
                                    const int at = i0 + nTap - kF / 2;
                                    if (at >= 0 && at < N) out[ch][(size_t) at] += wImg * win * hsum / kF;
                                }
                                ++images;
                                firstRefl = std::min (firstRefl, tau);
                            }
    }

    // ---- diffuse tail: the reverberant energy density 4 pi c / V (direct sound at 1 m = 1), decaying per
    // frequency with the Eyring time; it grows in as reflections scatter and takes over after the mixing time
    {
        int NF = 1024;
        while (NF < fs / 48000.0 * 1024.0) NF <<= 1;
        const int hop = NF / 2, nb = NF / 2;
        RealFFT fft;
        fft.init (NF);
        std::vector<double> t60 ((size_t) nb + 1), rho ((size_t) nb + 1);
        for (int k = 1; k < nb; ++k)
        {
            const double f = k * fs / NF;
            t60[(size_t) k] = eyringT60 (r, f, c);
            const double kd = 2.0 * kPi * f * r.micSpacing / c;
            rho[(size_t) k] = std::sin (kd) / kd;                // diffuse-field coherence of two omnis
        }
        std::vector<float> re ((size_t) nb + 1), im ((size_t) nb + 1), reR ((size_t) nb + 1), imR ((size_t) nb + 1), fr ((size_t) NF), frR ((size_t) NF);
        std::vector<double> win ((size_t) NF);
        for (int i = 0; i < NF; ++i) win[(size_t) i] = std::sin (kPi * (i + 0.5) / NF);        // sqrt-Hann, 50 % overlap
        Rng rng (0x5eed1234abcdull);
        const double e0 = 4.0 * kPi * c / V / fs;                 // energy per sample per unit bandwidth share
        const double tStart = std::max (0.0, firstRefl - 0.0005);
        for (int j = 0; j * hop < N + hop; ++j)
        {
            const double tc = j * (double) hop / fs;              // frame centre, after the direct sound
            const double tAbs = tc + dDir / c;
            // the share of the energy that is diffuse: scattered at least once (order ~ path / mean free path)
            const double order = c * tAbs / lmfp;
            double wd = 1.0 - std::pow (1.0 - r.scattering, order);
            if (tc > r.mixingTime) wd = tc > r.mixingTime + tFade ? 1.0 : wd + (1.0 - wd) * 0.5 * (1.0 - std::cos (kPi * (tc - r.mixingTime) / tFade));
            re[0] = im[0] = reR[0] = imR[0] = 0.0f;
            re[(size_t) nb] = im[(size_t) nb] = reR[(size_t) nb] = imR[(size_t) nb] = 0.0f;
            for (int k = 1; k < nb; ++k)
            {
                const double E = e0 * std::exp (-6.0 * std::log (10.0) * tAbs / t60[(size_t) k]) * wd;
                const double a = std::sqrt (NF * E * 0.5);           // complex Gaussian: each part variance 1/2
                const double gr = rng.gauss(), gi = rng.gauss(), hr = rng.gauss(), hi = rng.gauss();
                const double p = rho[(size_t) k], q = std::sqrt (std::max (0.0, 1.0 - p * p));
                re[(size_t) k] = (float) (a * gr); im[(size_t) k] = (float) (a * gi);
                reR[(size_t) k] = (float) (a * (p * gr + q * hr)); imR[(size_t) k] = (float) (a * (p * gi + q * hi));
            }
            fft.inverse (re.data(), im.data(), fr.data());
            fft.inverse (reR.data(), imR.data(), frR.data());
            const int s0 = j * hop - NF / 2;
            for (int i = 0; i < NF; ++i)
            {
                const int at = s0 + i;
                if (at < 0 || at >= N) continue;
                // nothing diffuse before the first reflection (gated per sample: the frames are 20 ms wide)
                const double ta = at / fs;
                if (ta < tStart) continue;
                const double on = std::min (1.0, (ta - tStart) / 0.003);
                out[0][(size_t) at] += on * win[(size_t) i] * fr[(size_t) i];     // (bins of variance NF E -> samples of variance E)
                out[1][(size_t) at] += on * win[(size_t) i] * frR[(size_t) i];
            }
        }
    }

    // ---- report, then unit energy (the mix law of the reverb knob is the same as before), fade the end
    double eRev = 0;
    for (int i = 0; i < N; ++i) eRev += 0.5 * (out[0][(size_t) i] * out[0][(size_t) i] + out[1][(size_t) i] * out[1][(size_t) i]);
    if (info)
    {
        info->V = V; info->S = S; info->c = c; info->meanFreePath = lmfp; info->directDist = dDir;
        info->firstReflection = firstRefl;
        info->images = images;
        for (int b = 0; b < RoomSpec::kBands; ++b)
        {
            info->t60[b] = eyringT60 (r, kBandHz[b], c);
            info->airDbPerKm[b] = 1000.0 * airAbsorptionDbPerM (kBandHz[b], r.tempC, r.humidity);
        }
        const double eDir = 1.0 / (dDir * dDir);
        info->drRatioDb = 10.0 * std::log10 (std::max (1e-30, eRev) / eDir);
        info->critDist = 0.057 * std::sqrt (V / std::max (0.1, eyringT60 (r, 1000.0, c)));
    }
    const double g = 1.0 / std::sqrt (std::max (1e-30, eRev));
    const int fadeN = (int) (0.1 * fs);
    L.assign ((size_t) N, 0.0f);
    R.assign ((size_t) N, 0.0f);
    for (int i = 0; i < N; ++i)
    {
        const double w = i >= N - fadeN ? 0.5 * (1.0 + std::cos (kPi * (i - (N - fadeN)) / fadeN)) : 1.0;
        L[(size_t) i] = (float) (out[0][(size_t) i] * g * w);
        R[(size_t) i] = (float) (out[1][(size_t) i] * g * w);
    }
    // the convolution needs nothing in the first 64 samples (it starts the first stage one block late)
    for (int i = 0; i < std::min (64, N); ++i) L[(size_t) i] = R[(size_t) i] = 0.0f;
}

/* ------------------------------------------------------------------ convolver */
void Convolver::prepare (const std::vector<float>& irL, const std::vector<float>& irR)
{
    st.clear();
    const int len = (int) std::min (irL.size(), irR.size());
    // (block size, start): the first stage starts one block in (latency-free), later ones at >= 2 blocks
    const int plan[4][2] { { 64, 64 }, { 256, 1024 }, { 2048, 8192 }, { 4096, 40960 } };
    const int ends[4] { 1024, 8192, 40960, 1 << 30 };
    int maxB = 64, maxEnd = 0;
    for (int s = 0; s < 4; ++s)
    {
        const int B = plan[s][0], off = plan[s][1];
        if (off >= len) break;
        const int end = std::min (ends[s], len);
        Stage S;
        S.B = B; S.offset = off; S.P = (end - off + B - 1) / B;
        S.fft.init (2 * B);
        const size_t nb = (size_t) B + 1, tot = (size_t) S.P * nb;
        S.HLr.assign (tot, 0); S.HLi.assign (tot, 0); S.HRr.assign (tot, 0); S.HRi.assign (tot, 0);
        S.Xr.assign (tot, 0); S.Xi.assign (tot, 0);
        S.aLr.assign (nb, 0); S.aLi.assign (nb, 0); S.aRr.assign (nb, 0); S.aRi.assign (nb, 0);
        S.frame.assign ((size_t) 2 * B, 0); S.outT.assign ((size_t) 2 * B, 0);
        std::vector<float> buf ((size_t) 2 * B);
        for (int p = 0; p < S.P; ++p)
            for (int ch = 0; ch < 2; ++ch)
            {
                const std::vector<float>& ir = ch == 0 ? irL : irR;
                std::fill (buf.begin(), buf.end(), 0.0f);
                for (int k = 0; k < B; ++k)
                {
                    const int at = off + p * B + k;
                    if (at < end) buf[(size_t) k] = ir[(size_t) at];
                }
                float* hr = (ch == 0 ? S.HLr : S.HRr).data() + (size_t) p * nb;
                float* hi = (ch == 0 ? S.HLi : S.HRi).data() + (size_t) p * nb;
                S.fft.forward (buf.data(), hr, hi);
            }
        maxB = std::max (maxB, B);
        maxEnd = std::max (maxEnd, off + S.P * B);
        st.push_back (std::move (S));
    }
    int h = 1;
    while (h < 2 * maxB + 64) h <<= 1;
    hist.assign ((size_t) h, 0.0f);
    hmask = h - 1;
    int o = 1;
    while (o < maxEnd + 2 * maxB + 64) o <<= 1;
    ringL.assign ((size_t) o, 0.0f);
    ringR.assign ((size_t) o, 0.0f);
    omask = o - 1;
    reset();
}

void Convolver::reset()
{
    std::fill (hist.begin(), hist.end(), 0.0f);
    std::fill (ringL.begin(), ringL.end(), 0.0f);
    std::fill (ringR.begin(), ringR.end(), 0.0f);
    for (auto& s : st)
    {
        std::fill (s.Xr.begin(), s.Xr.end(), 0.0f);
        std::fill (s.Xi.begin(), s.Xi.end(), 0.0f);
        s.busy = false; s.step = 0; s.credit = 0;
    }
    t = 0;
}

void Convolver::startJob (Stage& s, long long j)
{
    while (s.busy) doStep (s);          // (never needed in time: the previous block's work is spread to finish early)
    s.busy = true;
    s.step = 0;
    s.block = j;
    s.credit = 0;
}

/* one unit of a stage's work: 0 = input spectrum, 1..P = one partition, P+1 / P+2 = the two outputs */
void Convolver::doStep (Stage& s)
{
    const int B = s.B, P = s.P;
    const size_t nb = (size_t) B + 1;
    const long long j = s.block;
    if (s.step == 0)
    {
        const long long first = (j - 1) * B;                     // frame: the previous and this block
        for (int k = 0; k < 2 * B; ++k)
        {
            const long long at = first + k;
            s.frame[(size_t) k] = at >= 0 ? hist[(size_t) (at & hmask)] : 0.0f;
        }
        const size_t slot = (size_t) (j % P) * nb;
        s.fft.forward (s.frame.data(), s.Xr.data() + slot, s.Xi.data() + slot);
        std::fill (s.aLr.begin(), s.aLr.end(), 0.0f); std::fill (s.aLi.begin(), s.aLi.end(), 0.0f);
        std::fill (s.aRr.begin(), s.aRr.end(), 0.0f); std::fill (s.aRi.begin(), s.aRi.end(), 0.0f);
    }
    else if (s.step <= P)
    {
        const int p = s.step - 1;
        const long long jj = j - p;
        if (jj >= 0)
        {
            const size_t xs = (size_t) (jj % P) * nb, hs = (size_t) p * nb;
            // P5: SSE2, or AVX2 + FMA where the CPU has it (Simd.h)
            cmac ((int) nb, s.Xr.data() + xs, s.Xi.data() + xs, s.HLr.data() + hs, s.HLi.data() + hs, s.HRr.data() + hs, s.HRi.data() + hs,
                  s.aLr.data(), s.aLi.data(), s.aRr.data(), s.aRi.data());
        }
    }
    else
    {
        const bool left = s.step == P + 1;
        s.fft.inverse (left ? s.aLr.data() : s.aRr.data(), left ? s.aLi.data() : s.aRi.data(), s.outT.data());
        std::vector<float>& ring = left ? ringL : ringR;
        const long long at0 = j * B + s.offset;                  // the valid half: output times [jB + offset, +B)
        for (int k = 0; k < B; ++k) ring[(size_t) ((at0 + k) & omask)] += s.outT[(size_t) (B + k)];
    }
    if (++s.step > P + 2) s.busy = false;
}

void Convolver::process (const float* in, float* outL, float* outR, int n)
{
    if (st.empty()) { std::fill (outL, outL + n, 0.0f); std::fill (outR, outR + n, 0.0f); return; }
    const int B1 = st[0].B;
    for (int i = 0; i < n;)
    {
        const int pos = (int) (t % B1);
        const int m = std::min (n - i, B1 - pos);
        for (int k = 0; k < m; ++k)
        {
            const long long tt = t + k;
            hist[(size_t) (tt & hmask)] = in[i + k];
            outL[i + k] = ringL[(size_t) (tt & omask)]; ringL[(size_t) (tt & omask)] = 0.0f;
            outR[i + k] = ringR[(size_t) (tt & omask)]; ringR[(size_t) (tt & omask)] = 0.0f;
        }
        t += m;
        i += m;
        if (t % B1 == 0)
        {
            for (auto& s : st)
                if (t % s.B == 0) startJob (s, t / s.B - 1);
            // the first stage at once (its output starts with the next sample), the others spread over their block
            while (st[0].busy) doStep (st[0]);
            for (size_t k = 1; k < st.size(); ++k)
            {
                Stage& s = st[k];
                if (! s.busy) continue;
                s.credit += 1.25 * (s.P + 3) * (double) B1 / s.B;
                while (s.busy && s.credit >= 1.0) { doStep (s); s.credit -= 1.0; }
            }
        }
    }
}

/* ------------------------------------------------------------------ the engine's room */
void RoomReverb::prepare (double fs)
{
    // the room depends only on the sample rate: computed once per rate and shared (prepare is not real-time)
    struct Cached { std::vector<float> L, R; RoomInfo info; };
    static std::mutex mtx;
    static std::map<long, std::shared_ptr<const Cached>> cache;
    std::shared_ptr<const Cached> ir;
    {
        std::lock_guard<std::mutex> lock (mtx);
        auto& slot = cache[(long) std::lround (fs)];
        if (! slot)
        {
            auto c = std::make_shared<Cached>();
            roomImpulseResponse (defaultRoom(), fs, c->L, c->R, &c->info);
            slot = c;
        }
        ir = slot;
    }
    roomInfo = ir->info;
    conv.prepare (ir->L, ir->R);
    in.assign (256, 0.0f); oL.assign (256, 0.0f); oR.assign (256, 0.0f);
}

void RoomReverb::clear() { conv.reset(); }

void RoomReverb::process (const double* inL, const double* inR, double* outL, double* outR, int n)
{
    if ((int) in.size() < n) { in.resize ((size_t) n); oL.resize ((size_t) n); oR.resize ((size_t) n); }
    for (int i = 0; i < n; ++i) in[(size_t) i] = (float) (0.5 * (inL[i] + inR[i]));
    conv.process (in.data(), oL.data(), oR.data(), n);
    for (int i = 0; i < n; ++i) { outL[i] = oL[(size_t) i]; outR[i] = oR[(size_t) i]; }
}

} // namespace okl
