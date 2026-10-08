/*
 * OkumuLab 1 — offline checks of the C++ DSP core (no JUCE)
 *
 *   1. port      : C++ voice vs the Phase 1 C core (compiled in) and the Phase 1
 *                  reference numbers (pitch, regime, level, harmonics)
 *   2. pitchlock : pitch lock 100 % stays within ±2 cents of the key, with and
 *                  without the P3 servo, steady states and slow sweeps
 *   3. cpu       : real-time load of the engine with 1..32 sounding voices
 *   4. stress    : random parameters (incl. extremes): finite, bounded output
 *   5. midi      : channel-10 pad split, CC map, MIDI learn, pitch bend
 *
 *   labium_check [port|derive|midi|pitchlock|wind|tone|cpu|stress|mallet|room|lab|simd|all]
 */
#include "Engine.h"
#include "LabiumCore.h"
#include "MidiRouter.h"
#include "Params.h"
#include "Reverb.h"
#include "Presets.h"
#include "Simd.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include "Bessel.h"
#include "FpEnv.h"



extern "C" void labium_debug_derive (const double* p, double fs, double* out);
extern "C" int labium_npar (void);
extern "C" int labium_render (int n, double fs, int ctrl, int nc, const double* params, unsigned int seed,
                              double* out_mouth, double* out_top, double* out_eta, double* out_vm,
                              double* out_pf, double* out_uj, double* out_ps,
                              int nsnap, const int* snap_idx, int nx, double* snap_p, double* snap_u, double* snap_info);

namespace
{
#include "RefData.inc"

using namespace okl;

constexpr double FS = 96000.0;
constexpr int kMaxParamsCheck = 128;
int stressVariant = 0;           // (diagnostic: labium_check stress <bits> [trial])
int stressOnly = -1;
int g_fail = 0;

void check (bool ok, const char* what)
{
    if (! ok) { ++g_fail; std::printf ("  !! FAIL: %s\n", what); }
}

/* ------------------------------------------------------------ analysis */
void fft (std::vector<std::complex<double>>& a)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i)
    {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap (a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1)
    {
        const double ang = -2.0 * kPi / (double) len;
        const std::complex<double> wl (std::cos (ang), std::sin (ang));
        for (size_t i = 0; i < n; i += len)
        {
            std::complex<double> w (1.0, 0.0);
            for (size_t j = 0; j < len / 2; ++j)
            {
                const auto u = a[i + j], v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

size_t nextPow2 (size_t n) { size_t p = 1; while (p < n) p <<= 1; return p; }

/* labium.py est_pitch: autocorrelation peak near f_hint * [1/hi .. 1/lo], parabolic interpolation */
double estPitch (const double* x, size_t n, double fs, double fHint, double lo = 0.4, double hi = 4.5, double* clarity = nullptr)
{
    double m = 0;
    for (size_t i = 0; i < n; ++i) m += x[i];
    m /= (double) n;
    const size_t N = nextPow2 (2 * n);
    std::vector<std::complex<double>> a (N);
    for (size_t i = 0; i < n; ++i) a[i] = x[i] - m;
    fft (a);
    for (auto& v : a) v = std::norm (v);
    fft (a);                                 // real & even: forward = N x inverse
    std::vector<double> ac (n);
    if (a[0].real() <= 0) return std::numeric_limits<double>::quiet_NaN();
    for (size_t i = 0; i < n; ++i) ac[i] = a[i].real() / a[0].real();
    const size_t lagMin = (size_t) (fs / (fHint * hi));
    const size_t lagMax = std::min ((size_t) (fs / (fHint * lo)), n - 2);
    double best = -1e30;
    for (size_t i = lagMin; i < lagMax; ++i) best = std::max (best, ac[i]);
    size_t cand = 0;
    for (size_t j = lagMin + 1; j + 1 < lagMax; ++j)
        if (ac[j] >= ac[j - 1] && ac[j] >= ac[j + 1] && ac[j] > 0.9 * best) { cand = j; break; }
    if (! cand) for (size_t j = lagMin; j < lagMax; ++j) if (ac[j] == best) { cand = j; break; }
    if (cand < 1) return std::numeric_limits<double>::quiet_NaN();
    const double A = ac[cand - 1], B = ac[cand], C = ac[cand + 1], den = A - 2 * B + C;
    if (clarity) *clarity = B * (double) n / (double) (n - cand);     // unbiased normalised autocorrelation at the period
    return fs / ((double) cand + (den != 0 ? 0.5 * (A - C) / den : 0.0));
}

void harmonics (const double* x, size_t n, double fs, double f0, double* out, int nh)
{
    double m = 0;
    for (size_t i = 0; i < n; ++i) m += x[i];
    m /= (double) n;
    const size_t N = nextPow2 (n * 4);
    std::vector<std::complex<double>> a (N);
    for (size_t i = 0; i < n; ++i)
    {
        const double w = 0.42 - 0.5 * std::cos (2 * kPi * i / (n - 1)) + 0.08 * std::cos (4 * kPi * i / (n - 1));
        a[i] = (x[i] - m) * w;
    }
    fft (a);
    double ref = 0;
    for (int hh = 1; hh <= nh; ++hh)
    {
        const double f = hh * f0;
        if (f > fs / 2 * 0.95) { out[hh - 1] = std::numeric_limits<double>::quiet_NaN(); continue; }
        const size_t k0 = (size_t) std::ceil ((f - 0.25 * f0) * N / fs), k1 = (size_t) std::floor ((f + 0.25 * f0) * N / fs);
        double mx = 0;
        for (size_t k = k0; k <= k1; ++k) mx = std::max (mx, std::abs (a[k]));
        if (hh == 1) ref = mx;
        out[hh - 1] = 20 * std::log10 (mx / ref + 1e-12);
    }
}

/* strongest spectral peaks (frequency, dB re strongest) of a segment */
void spectralPeaks (const double* x, size_t n, double fs, int np, double* fr, double* db)
{
    const size_t N = nextPow2 (n * 2);
    std::vector<std::complex<double>> a (N);
    for (size_t i = 0; i < n; ++i)
        a[i] = x[i] * (0.42 - 0.5 * std::cos (2 * kPi * i / (n - 1)) + 0.08 * std::cos (4 * kPi * i / (n - 1)));
    fft (a);
    std::vector<double> m (N / 2);
    for (size_t k = 0; k < N / 2; ++k) m[k] = std::abs (a[k]);
    std::vector<std::pair<double, size_t>> pk;
    for (size_t k = 2; k + 2 < N / 2; ++k)
        if (m[k] > m[k - 1] && m[k] >= m[k + 1] && m[k] > m[k - 2] && m[k] >= m[k + 2]) pk.push_back ({ m[k], k });
    std::sort (pk.begin(), pk.end(), [] (auto& p, auto& q) { return p.first > q.first; });
    for (int i = 0; i < np; ++i)
    {
        if (i >= (int) pk.size()) { fr[i] = 0; db[i] = -999; continue; }
        const size_t k = pk[(size_t) i].second;
        const double A = std::log (m[k - 1]), B = std::log (m[k]), C = std::log (m[k + 1]);
        const double off = 0.5 * (A - C) / (A - 2 * B + C);
        fr[i] = (k + off) * fs / (double) N;
        db[i] = 20 * std::log10 (m[k] / pk[0].first);
    }
}

struct Metrics { double f = 0, regime = 0, db = 0; double hm[12] {}; int resets = 0; };

Metrics analyze (const std::vector<double>& mouth, double fs, double f0, double steadyFrom)
{
    Metrics r;
    const size_t i0 = (size_t) (steadyFrom * fs);
    const double* x = mouth.data() + i0;
    const size_t n = mouth.size() - i0;
    r.f = estPitch (x, n, fs, f0);
    double s = 0;
    for (size_t i = 0; i < n; ++i) s += x[i] * x[i];
    r.regime = r.f / f0;
    r.db = 20 * std::log10 (std::sqrt (s / (double) n) / 2e-5);
    harmonics (x, n, fs, r.f, r.hm, 12);
    return r;
}

double cents (double f, double ref) { return 1200.0 * std::log2 (f / ref); }

/*
 * pitch error of a sustained segment against the key: the sounding pitch is the
 * autocorrelation pitch when the tone is clearly periodic, otherwise (breathy,
 * noisy tones where the autocorrelation picks a wrong lag) the strongest spectral
 * partial. Error = cents from the nearest k/2 multiple of the key frequency.
 */
int g_noisy = 0;
double pitchError (const double* x, size_t n, double fs, double fKey, double* candOut, double* fOut = nullptr)
{
    double clar = 0;
    double f = estPitch (x, n, fs, fKey, 0.4, 8.5, &clar);
    if (! (clar >= 0.9) || ! std::isfinite (f))
    {
        double pf[1], pdb[1];
        spectralPeaks (x, n, fs, 1, pf, pdb);
        f = pf[0];
        ++g_noisy;
    }
    if (fOut) *fOut = f;
    if (! (f > 0)) { *candOut = 0; return std::numeric_limits<double>::quiet_NaN(); }
    const double r = f / fKey;
    const double cand = 0.5 * std::clamp ((int) std::lround (2.0 * r), 1, 24);
    *candOut = cand;
    return cents (r, cand);
}

/* ------------------------------------------------------------- renders */
ParamVec basePipe (int midi)
{
    // Phase 1 base_params(design_prinzipal(midi), k_cal) through the plugin's own path (Phase 1's pipe: voiced for 75 mmWS)
    const PipeDesign pd = designPrinzipal (midi, 75.0 * MMWS);
    ParamVec v;
    Globals G;
    G.wind = 75.0;               // Phase 1: 75 mmWS in the windchest (the plugin's reference is 500 Pa since 0.6.1)
    voiceParams (midi, G, VoiceOverride {}, pd, v);
    // undo the knob defaults that differ from Phase 1 base_params: voicing follow 0, pallet 12 ms
    v[P_VFOLLOW] = 0.0;
    v[P_PALLET_TAU] = 0.012;
    return v;
}

/* the jet of a bare voice as the plugin's engine runs it (the jitter of P5, the v1.0 jet; Engine::setupVoice) */
void engineJet (Voice& v)
{
    static const Engine e;
    e.setupVoice (v);
}

/* jitter: P5 jet convection jitter (0 = Phase 1's jet, for the port checks; > 0: the plugin's engine's jet, engineJet) */
std::vector<double> renderCpp (const ParamVec& p, double dur, double fs, bool servo, double gateOff = -1, int* resets = nullptr, double servoStart = 0.0,
                               double jitter = 0.0)
{
    const int n = (int) std::lround (dur * fs);
    Voice v;
    if (jitter > 0.0) engineJet (v);
    v.jetJitter = jitter;
    v.servoEnabled = servo;
    v.prepare (fs, 3);
    v.start (p, servoStart);
    std::vector<double> mouth ((size_t) n), L ((size_t) n), R ((size_t) n);
    // the gate closes on the first control frame at or after gateOff (as the C core's frames do)
    const int gOff = gateOff >= 0 ? (int) std::ceil (gateOff * fs / CTRL) * CTRL : n;
    const int blk = 4096;
    for (int i = 0; i < n;)
    {
        int m = std::min (blk, n - i);
        if (i < gOff && i + m > gOff) m = gOff - i;
        if (i == gOff)
        {
            ParamVec t = p;
            t[P_GATE] = 0;
            v.setTarget (t);
        }
        v.render (m, L.data() + i, R.data() + i, mouth.data() + i);
        i += m;
    }
    if (resets) *resets = v.resets;
    return mouth;
}

std::vector<double> renderC (const ParamVec& p, double dur, double gateOff = -1, int* resets = nullptr, unsigned seed = 1)
{
    const int n = (int) std::lround (dur * FS);
    const int nc = n / CTRL + 2;
    // the C core's frames have its own (Phase 1) length: the C++ parameters appended since (mallet, P5) are left out
    const int np = labium_npar();
    std::vector<double> P ((size_t) nc * (size_t) np);
    for (int i = 0; i < nc; ++i)
    {
        std::copy (p.begin(), p.begin() + np, P.begin() + (size_t) i * np);
        if (gateOff >= 0 && i * CTRL / FS >= gateOff) P[(size_t) i * np + P_GATE] = 0.0;
    }
    std::vector<double> mouth ((size_t) n);
    const int r = labium_render (n, FS, CTRL, nc, P.data(), seed, mouth.data(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                                 0, nullptr, 0, nullptr, nullptr, nullptr);
    if (resets) *resets = r;
    return mouth;
}

/* ================================================================ 1. port */
void testPort()
{
    std::printf ("\n== 1. port: C++ voice vs Phase 1 C core and Phase 1 reference (96 kHz, servo off) ==\n");
    std::printf ("%-22s %10s %10s %10s %8s %8s %7s %7s %7s %6s %6s\n", "state", "ref f", "C f", "C++ f", "C++-ref", "C++-C", "ref dB", "C++ dB", "d dB", "hm2-6", "reset");
    double worstC = 0, worstRef = 0, worstDb = 0, worstHm = 0, seedVar = 0, cBase = 0;
    auto row = [&] (const char* name, int midi, const ParamVec& p, double dur, double steady, double gateOff,
                    double rf, double rreg, double rdb, const double* rhm)
    {
        const double f0 = midiToHz (midi);
        int rc = 0, rcpp = 0;
        const Metrics mc = analyze (renderC (p, dur, gateOff, &rc), FS, f0, steady);
        const Metrics mp = analyze (renderCpp (p, dur, FS, false, gateOff, &rcpp), FS, f0, steady);
        const Metrics ms = analyze (renderC (p, dur, gateOff, nullptr, 7), FS, f0, steady);   // C core, other noise seed
        double hmax = 0;
        if (rhm)
            for (int i = 1; i < 6; ++i)
                if (std::isfinite (rhm[i]) && std::isfinite (mp.hm[i])) hmax = std::max (hmax, std::abs (mp.hm[i] - rhm[i]));
        double hmaxC = 0;
        if (rhm)
            for (int i = 1; i < 6; ++i)
                if (std::isfinite (rhm[i]) && std::isfinite (mc.hm[i])) hmaxC = std::max (hmaxC, std::abs (mc.hm[i] - rhm[i]));
        const double dRef = cents (mp.f, rf), dC = cents (mp.f, mc.f), dDb = mp.db - rdb;
        std::printf ("%-22s %10.3f %10.3f %10.3f %8.2f %8.2f %7.1f %7.1f %7.2f %6.1f %3d/%d   C core: %+.2f dB, hm %.1f; seed 7: %+.2f cent, hm %.1f\n", name, rf, mc.f, mp.f, dRef, dC, rdb, mp.db, dDb,
                     rhm ? hmax : 0.0, rcpp, rc, mc.db - rdb, hmaxC, cents (ms.f, rf), [&] { double x = 0; if (rhm) for (int i = 1; i < 6; ++i) if (std::isfinite (rhm[i]) && std::isfinite (ms.hm[i])) x = std::max (x, std::abs (ms.hm[i] - rhm[i])); return x; }());
        // 8' C needs more than 1.4 s to settle: that row is compared with the noise-seed spread
        if (std::string (name) == "C_base") { seedVar = std::abs (cents (ms.f, rf)); cBase = dRef; }
        else
        {
            worstC = std::max (worstC, std::abs (dC));
            worstRef = std::max (worstRef, std::abs (dRef));
        }
        worstDb = std::max (worstDb, std::abs (dDb));
        worstHm = std::max (worstHm, hmax);
        (void) rreg;
    };

    for (const auto& s : kRefStates)
    {
        ParamVec p = basePipe (s.midi);
        const std::string n = s.name;
        if (n == "c1_overblown") p[P_PCHEST] = 700 * MMWS;
        else if (n == "c1_stopped") { p[P_MORPH] = 1; p[P_VFOLLOW] = 1; p[P_PLOCK] = 0; }
        else if (n == "c1_wide" || n == "c1_narrow")
        {
            const double d = 0.1555 * std::pow (2.0, -(24.0 - (n == "c1_wide" ? 10.0 : -14.0)) / 16.0);
            p[P_D] = d; p[P_MOUTHW] = 0.25 * kPi * d;
        }
        else if (n == "c1_jet_centred") p[P_Y0] = 0;
        else if (n == "c1_helium")
        {
            const GasProps g = gasProps (0, 1, 0, 20);
            p[P_C] = g.c; p[P_RHO] = g.rho; p[P_GAMMA] = g.gamma; p[P_MU] = g.mu; p[P_PLOCK] = 0;
        }
        row (s.name, s.midi, p, 1.4, 1.0, -1, s.f, s.regime, s.db, s.hm);
    }
    for (const auto& w : kRefWind)
    {
        ParamVec p = basePipe (60);
        p[P_PCHEST] = w.pMMWS * MMWS;
        char nm[40];
        std::snprintf (nm, sizeof nm, "c1 wind %.0f mmWS", w.pMMWS);
        row (nm, 60, p, 1.6, 0.7, 1.2, w.f, w.regime, w.db, nullptr);
    }
    for (const auto& r : kRefRange)
    {
        if (r.midi % 12) continue;
        char nm[40];
        std::snprintf (nm, sizeof nm, "range MIDI %d", r.midi);
        row (nm, r.midi, basePipe (r.midi), 2.5, 2.0, -1, r.f, r.f / r.f0, r.db, r.hm);
    }
    std::printf ("worst (settled states): C++ vs C %.2f cent, C++ vs Phase 1 %.2f cent; level %.2f dB; harmonics 2-6 %.1f dB\n", worstC, worstRef, worstDb, worstHm);
    std::printf ("C_base at 1.0-1.4 s (attack not settled): C++ %+.2f cent; the C core itself moves %+.2f cent with another noise seed\n", cBase, seedVar);
    check (worstRef < 1.0, "settled states within 1 cent of Phase 1");
    check (worstDb < 1.0, "level within 1 dB of Phase 1");

    // the turbulence noise uses another random sequence than the C core; without noise the
    // two cores must agree closely (what remains is the per-frame jet delay of the RT engine)
    std::printf ("-- without turbulence noise (deterministic): C++ vs C core --\n");
    std::printf ("%-22s %10s %10s %8s %8s %8s %8s\n", "state", "C f", "C++ f", "cents", "C dB", "d dB", "hm2-6");
    double worstNz = 0;
    for (const auto& s : kRefStates)
    {
        ParamVec p = basePipe (s.midi);
        const std::string n = s.name;
        if (n == "c1_overblown") p[P_PCHEST] = 700 * MMWS;
        else if (n == "c1_jet_centred") p[P_Y0] = 0;
        else if (n != "C_base" && n != "c1_base" && n != "c3_base") continue;
        p[P_NOISE] = 0.0;
        const double f0 = midiToHz (s.midi), dur = s.midi < 48 ? 2.5 : 1.4, st = dur - 0.4;
        const Metrics mc = analyze (renderC (p, dur), FS, f0, st);
        const Metrics mp = analyze (renderCpp (p, dur, FS, false), FS, f0, st);
        double hmax = 0;
        for (int i = 1; i < 6; ++i) if (std::isfinite (mc.hm[i]) && mc.hm[i] > -60) hmax = std::max (hmax, std::abs (mp.hm[i] - mc.hm[i]));
        std::printf ("%-22s %10.3f %10.3f %8.2f %8.1f %8.2f %8.2f\n", s.name, mc.f, mp.f, cents (mp.f, mc.f), mc.db, mp.db - mc.db, hmax);
        worstNz = std::max (worstNz, std::abs (cents (mp.f, mc.f)));
    }
    check (worstNz < 0.05, "noise-free C++ within 0.05 cent of the C core");
}

/* =========================================================== 2. pitch lock */
struct Mod { const char* name; void (*apply) (Globals&); };

const Mod kMods[] = {
    { "default", [] (Globals&) {} },
    { "wind 40 mmWS", [] (Globals& g) { g.wind = 40; } },
    { "wind 150 mmWS", [] (Globals& g) { g.wind = 150; } },
    { "wind 300 mmWS", [] (Globals& g) { g.wind = 300; } },
    { "cut-up x0.6", [] (Globals& g) { g.cutup = 0.6; } },
    { "cut-up x1.6", [] (Globals& g) { g.cutup = 1.6; } },
    { "labium -1.5b", [] (Globals& g) { g.y0b = -1.5; } },
    { "labium +1.5b", [] (Globals& g) { g.y0b = 1.5; } },
    { "scale +8 HT", [] (Globals& g) { g.scaleHT = 8; } },
    { "scale -14 HT", [] (Globals& g) { g.scaleHT = -14; } },
    { "mouth 1/7", [] (Globals& g) { g.mouthFrac = 1.0 / 7.0; } },
    { "toe x0.6", [] (Globals& g) { g.toe = 0.6; } },
    { "helium 60%", [] (Globals& g) { g.gas = 0.6; } },
    { "CO2 60%", [] (Globals& g) { g.gas = -0.6; } },
    { "-20 C", [] (Globals& g) { g.tempC = -20; } },
    { "60 C", [] (Globals& g) { g.tempC = 60; } },
    { "c only x1.5", [] (Globals& g) { g.cMult = 1.5; } },
    { "rho only x0.4", [] (Globals& g) { g.rhoMult = 0.4; } },
    { "glide +5 st", [] (Globals& g) { g.glide = 5; } },
    { "glide -7 st", [] (Globals& g) { g.glide = -7; } },
    { "stopped", [] (Globals& g) { g.morph = 1; } },
    { "jet feedback x1.5", [] (Globals& g) { g.jetGain = 1.5; } },
    { "wall loss x2", [] (Globals& g) { g.loss = 5.0; } },
    { "wind 200 + He 60%", [] (Globals& g) { g.wind = 200; g.gas = 0.6; } },
};

/* steady pitch of one voice (Engine path: velocity, pallet, learned tuning off) */
double steadyCents (int midi, const Globals& G, double fs, bool servo, double dur, double* ratioOut)
{
    const PipeDesign pd = designPrinzipal (midi);
    ParamVec p;
    voiceParams (midi, G, VoiceOverride {}, pd, p);
    const auto m = renderCpp (p, dur, fs, servo, -1, nullptr, 1200.0 * std::log2 (kcalFor (midi)), Engine::kJetJitter);
    // sustained pitch over the last 0.6 s: the strongest spectral partial (precise over long windows,
    // immune to the lag errors the autocorrelation makes on breathy low pipes)
    const size_t i0 = (size_t) ((dur - 0.6) * fs);
    double pf[1], pdb[1];
    spectralPeaks (m.data() + i0, m.size() - i0, fs, 1, pf, pdb);
    const double r = pf[0] / p[P_FTARGET];
    if (! (r > 0)) { *ratioOut = 0; return std::numeric_limits<double>::quiet_NaN(); }
    const double cand = 0.5 * std::clamp ((int) std::lround (2.0 * r), 1, 24);
    *ratioOut = cand;
    return cents (r, cand);
}

void testPitchLock()
{
    std::printf ("\n== 2. pitch lock 100 %%: steady pitch vs the key (cents) ==\n");
    for (double fs : { 96000.0, 88200.0 })
    {
        std::printf ("-- internal %.1f kHz, default voicing, keys C..c4 --\n", fs / 1000);
        double wOff = 0, wOn = 0;
        std::printf ("  key   servo off  servo on\n");
        for (int m = 36; m <= 96; m += 3)
        {
            double r1, r2;
            Globals G;
            const double a = steadyCents (m, G, fs, false, 2.0, &r1);
            const double b = steadyCents (m, G, fs, true, 2.0, &r2);
            char nm[8];
            organName (m, nm);
            std::printf ("  %-5s %9.2f %9.2f\n", nm, a, b);
            wOff = std::max (wOff, std::abs (a));
            wOn = std::max (wOn, std::abs (b));
        }
        std::printf ("  worst |cents|: servo off %.2f, servo on %.2f\n", wOff, wOn);
        check (wOn <= 2.0, "default voicing within 2 cents (servo on)");
    }

    std::printf ("-- modulated states (internal 96 kHz), worst of keys C, c, c1, c2, c3 --\n");
    std::printf ("  %-20s %10s %10s %8s\n", "state", "servo off", "servo on", "regime");
    const int keys[] { 36, 48, 60, 72, 84 };
    double worstOn = 0;
    int nOver = 0;
    for (const auto& md : kMods)
    {
        double wOff = 0, wOn = 0, reg = 0;
        for (int k : keys)
        {
            Globals G;               // (the voicing pressure: the reference, 500 Pa)
            md.apply (G);
            double r1, r2;
            const double a = steadyCents (k, G, FS, false, 2.6, &r1);
            const double b = steadyCents (k, G, FS, true, 2.6, &r2);
            if (std::isfinite (a)) wOff = std::max (wOff, std::abs (a));
            if (std::isfinite (b)) wOn = std::max (wOn, std::abs (b)); else wOn = 999;
            reg = std::max (reg, r2);
        }
        std::printf ("  %-20s %10.2f %10.2f %8.1f\n", md.name, wOff, wOn, reg);
        worstOn = std::max (worstOn, wOn);
        if (wOn > 2.0) ++nOver;
    }
    std::printf ("  worst |cents| with servo: %.2f  (%d states over 2 cents)\n", worstOn, nOver);
    check (nOver == 0, "modulated states within 2 cents (servo on)");

    // slow sweeps: the knob moves while the note sounds
    std::printf ("-- sweeps over 3 s at c1 (internal 96 kHz, servo on), pitch in 60 ms windows --\n");
    struct Sweep { const char* name; void (*at) (Globals&, double); };
    const Sweep sweeps[] = {
        { "wind x1 -> x4", [] (Globals& g, double x) { g.wind = Globals {}.wind * std::pow (4.0, x); } },
        { "cut-up x1 -> x1.6", [] (Globals& g, double x) { g.cutup = 1 + 0.6 * x; } },
        { "labium 0.5 -> -1.5 b", [] (Globals& g, double x) { g.y0b = 0.5 - 2.0 * x; } },
        { "scale -2 -> +8", [] (Globals& g, double x) { g.scaleHT = -2 + 10 * x; } },
        { "helium 0 -> 60%", [] (Globals& g, double x) { g.gas = 0.6 * x; } },
        { "temp 20 -> 60 C", [] (Globals& g, double x) { g.tempC = 20 + 40 * x; } },
        { "glide 0 -> +5 st", [] (Globals& g, double x) { g.glide = 5 * x; } },
        { "tremulant 50%", [] (Globals& g, double) { g.trem = 0.5; } },
    };
    for (bool servo : { false, true })
    {
        double worstAll = 0;
        for (const auto& sw : sweeps)
        {
            const double dur = 4.2, fs = FS;
            const int n = (int) (dur * fs);
            const int midi = 60;
            const PipeDesign pd = designPrinzipal (midi);
            Voice v;
            v.servoEnabled = servo;
            engineJet (v);
            v.prepare (fs, 3);
            Globals G;               // (the voicing pressure, as the states above)
            ParamVec p;
            voiceParams (midi, G, VoiceOverride {}, pd, p);
            v.start (p, 1200.0 * std::log2 (kcalFor (midi)));
            std::vector<double> mouth ((size_t) n), L ((size_t) n), R ((size_t) n);
            double tremPh = 0;
            for (int i = 0; i < n; i += 64)
            {
                const double t = i / fs;
                const double x = std::clamp ((t - 0.8) / 3.0, 0.0, 1.0);
                Globals g;
                sw.at (g, x);
                voiceParams (midi, g, VoiceOverride {}, pd, p);
                v.setTarget (p);
                tremPh += kTwoPi * g.tremRate * 64 / fs;
                v.trem = 1.0 + 0.15 * g.trem * std::sin (tremPh);
                v.render (std::min (64, n - i), L.data() + i, R.data() + i, mouth.data() + i);
            }
            // pitch track from 0.8 s (sweep start) to the end
            const int win = (int) (0.06 * fs), hop = (int) (0.03 * fs);
            double worst = 0, sq = 0;
            int cnt = 0;
            for (int i = (int) (0.8 * fs); i + win < n; i += hop)
            {
                const double t = (i + win / 2) / fs;
                const double x = std::clamp ((t - 0.8) / 3.0, 0.0, 1.0);
                Globals g;
                sw.at (g, x);
                const double ft = midiToHz (midi) * std::pow (2.0, g.glide / 12.0);
                double cand;
                const double c = pitchError (mouth.data() + i, (size_t) win, fs, ft, &cand);
                if (servo && std::getenv ("OKL_SWEEP_TRACE") && std::string (sw.name).find (std::getenv ("OKL_SWEEP_TRACE")) == 0)
                    std::printf ("      t %.2f  x %.2f  err %+6.2f\n", t, x, c);
                worst = std::max (worst, std::abs (c));
                sq += c * c;
                ++cnt;
            }
            std::printf ("  servo %-3s %-22s max %6.2f  rms %5.2f cents\n", servo ? "on" : "off", sw.name, worst, std::sqrt (sq / std::max (1, cnt)));
            if (std::string (sw.name) != "tremulant 50%") worstAll = std::max (worstAll, worst);
        }
        std::printf ("  servo %s: worst during sweeps (tremulant excluded) %.2f cents\n", servo ? "on" : "off", worstAll);
    }
    std::printf ("  (%d measurements were breathy / not clearly periodic and used the strongest partial)\n", g_noisy);
}

/* ================================================================== 3. cpu */
void testCpu()
{
    std::printf ("\n== 3. CPU: engine at 48 kHz host (96 kHz internal), 512-sample blocks, one core ==\n");
    const int nv[] { 1, 8, 16, 32 };
    for (int voicesN : nv)
    {
        for (bool moving : { false, true })
        {
            Engine e;
            e.prepare (48000, 512);
            Globals G;
            e.setGlobals (G);
            for (int i = 0; i < voicesN; ++i) e.noteOn (i, 36 + (i * 7) % 60, 100);
            std::vector<float> L (512), R (512);
            // settle
            for (int b = 0; b < 100; ++b) e.process (L.data(), R.data(), 512);
            const int blocks = (int) (8.0 * 48000 / 512);
            const auto t0 = std::chrono::steady_clock::now();
            for (int b = 0; b < blocks; ++b)
            {
                if (moving)
                {
                    // a knob moving continuously (cut-up wobble) keeps derive() busy in every voice
                    G.cutup = 1.0 + 0.2 * std::sin (b * 0.05);
                    e.setGlobals (G);
                }
                e.process (L.data(), R.data(), 512);
            }
            const double sec = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
            const double load = sec / (blocks * 512 / 48000.0) * 100.0;
            std::printf ("  %2d voices %-14s %6.1f %% of one core  (%.2f %% per voice)\n", voicesN, moving ? "knob moving" : "steady", load, load / voicesN);
        }
    }
}

/* =============================================================== 4. stress */
void testStress()
{
    std::printf ("\n== 4. stress: random parameters incl. extremes (48 kHz host) ==\n");
    std::mt19937 rng (20261004);
    std::uniform_real_distribution<double> U (0.0, 1.0);
    auto randomGlobals = [&] (Globals& g)
    {
        for (int i = 0; i < kNumParams; ++i)
        {
            const ParamDef& d = kParamDefs[i];
            double n = U (rng);
            const double r = U (rng);
            if (r < 0.15) n = 0.0; else if (r < 0.3) n = 1.0;     // corners
            else if (r < 0.5) n = toNorm (d, d.def);
            g.*(d.field) = fromNorm (d, n);
        }
        g.outDb = 0;
        if (g.excite >= 0.5) g.damper = std::max (g.damper, 0.5);    // (struck pipes: the release check waits for the felt)
        // (diagnostic variants: labium_check stress <bits>: 1 no side hole, 2 no cross drive, 4 no FM, 16 wall loss >= physical, 32 no mallet)
        if (stressVariant & 1) g.sideHole = 0;
        if (stressVariant & 2) g.crossDrive = 0;
        if (stressVariant & 4) g.fmDepth = 0;
        if (stressVariant & 16) g.loss = std::max (g.loss, 2.5);
        if (stressVariant & 32) g.excite = 0;
        if (stressVariant & 64) g.modSrc1 = g.modSrc2 = g.modSrc3 = g.modSrc4 = 0;
        if (stressVariant & 256) g.reverb = 0;
        if (stressVariant & 512) g.edge = 0;
        if (stressVariant & 1024) g.fmDepth = 0;
        if (stressVariant & 2048) g.jetGain = std::min (g.jetGain, 1.0);
        if (stressVariant & 128) { g.modSrc1 = g.modSrc2 = g.modSrc3 = g.modSrc4 = 0; g.sideHole = 0; }
    };
    const int trials = 240;
    int bad = 0, withResets = 0, notFreed = 0, totalResets = 0, longRings = 0, sustained = 0;
    double peak = 0;
    std::vector<float> L (480), R (480);
    for (int t = 0; t < trials; ++t)
    {
        if (stressOnly >= 0 && t != stressOnly)
        {
            // (diagnostic: replay one trial: the same random draws, nothing rendered)
            Globals g;
            randomGlobals (g);
            const int nn = 1 + (int) (U (rng) * 6);
            for (int i = 0; i < nn; ++i) { (void) U (rng); (void) U (rng); }
            randomGlobals (g);
            continue;
        }
        Engine e;
        e.prepare (t % 3 == 0 ? 44100 : (t % 3 == 1 ? 48000 : 96000), 480);
        Globals g;
        randomGlobals (g);
        e.setGlobals (g);
        const int nn = 1 + (int) (U (rng) * 6);
        for (int i = 0; i < nn; ++i) e.noteOn (i, (stressVariant & 8) ? 24 + (int) (U (rng) * 85) : (int) (U (rng) * 128), 1 + (int) (U (rng) * 126));     // P5: the whole MIDI range (8: the old 24..108)
        bool finite = true;
        const double fsr = e.hostFs();
        const int blocks = (int) (0.6 * fsr / 480);
        for (int b = 0; b < 2 * blocks; ++b)
        {
            if (b == blocks) { randomGlobals (g); e.setGlobals (g); }
            e.process (L.data(), R.data(), 480);
            for (int i = 0; i < 480; ++i)
            {
                if (! std::isfinite (L[i]) || ! std::isfinite (R[i]) || std::abs (L[i]) > 1.0f || std::abs (R[i]) > 1.0f) finite = false;
                peak = std::max (peak, (double) std::max (std::abs (L[i]), std::abs (R[i])));
            }
        }
        int res = 0;
        for (int i = 0; i < Engine::kMaxVoices; ++i) res += e.voice (i).resets;
        totalResets += res;
        if (res)
        {
            ++withResets;
            const Globals& gg = e.globals();
            std::printf ("    resets: trial %d (%d) | fm %s depth %.2f ratio %.2f | hole %.2f | cross %.2f | jet %.2f loss %.2f wind %.0f cMult %.2f rhoMult %.2f gas %.2f morph %.2f excite %.0f | matrix %.0f>%.0f %.2f\n",
                         t, res, gg.fmTarget < 0.5 ? "wind" : (gg.fmTarget < 1.5 ? "length" : "lip"), gg.fmDepth, gg.fmRatio, gg.sideHole, gg.crossDrive,
                         gg.jetGain, gg.loss, gg.wind, gg.cMult, gg.rhoMult, gg.gas, gg.morph, gg.excite, gg.modSrc1, gg.modDst1, gg.modAmt1);
        }
        if (! finite) ++bad;
        // release: everything must fall silent and free its voices (a very low wall loss rings for many seconds)
        e.allOff (false);
        std::vector<double> tailBuf;
        double e15 = 0, e25 = 0, relSec = 0;
        for (int b = 0; b < (int) (25.0 * fsr / 480) && e.activeVoices() > 0; ++b)
        {
            e.process (L.data(), R.data(), 480);
            const double tb = b * 480.0 / fsr;
            if (stressOnly >= 0)
                for (int i = 0; i < 480; ++i) relSec += (double) L[i] * L[i];
            if (stressOnly >= 0 && b % (int) (fsr / 480) == 0)
            {
                std::printf ("    t %4.1f s: out %.1f dBFS", tb, 10 * std::log10 (relSec / fsr + 1e-30));
                relSec = 0;
                for (int i = 0; i < Engine::kMaxVoices; ++i)
                    if (e.voice (i).active)
                    {
                        const Voice& vv = e.voice (i);
                        std::printf ("  [midi %d peak %.2e pf %.2e | Leff %.2f m dline %.1f am %.6f]", vv.midi, vv.peakLevel, vv.pf, vv.derived().Leff, vv.derived().dline, vv.derived().aMorph);
                    }
                std::printf ("\n");
            }
            for (int i = 0; i < 480; ++i)
            {
                if (tb >= 14.0 && tb < 15.0) e15 += (double) L[i] * L[i];
                if (tb >= 24.0) { e25 += (double) L[i] * L[i]; tailBuf.push_back (L[i]); }
            }
        }
        // still sounding: acceptable only as a decaying ring (very low wall loss), not as a sustained tone
        if (e.activeVoices() > 0)
        {
            if (e25 < 0.5 * e15) ++longRings;
            else { ++sustained; std::printf ("    sustained: trial %d, energy 24-25 s / 14-15 s = %.2f (%.1f / %.1f dBFS, %d voices)\n", t, e25 / std::max (1e-300, e15), 10 * std::log10 (e25 / fsr + 1e-30), 10 * std::log10 (e15 / fsr + 1e-30), e.activeVoices()); }
        }
        if (e.activeVoices() > 0 && tailBuf.size() > 4096 && notFreed < 6)
        {
            double pf[3], pdb[3];
            spectralPeaks (tailBuf.data(), tailBuf.size(), fsr, 3, pf, pdb);
            double s = 0;
            for (double x : tailBuf) s += x * x;
            std::printf ("    tail: %.1f dBFS, peaks %.1f Hz, %.1f Hz (%.0f dB), %.1f Hz (%.0f dB)\n", 10 * std::log10 (s / tailBuf.size() + 1e-30), pf[0], pf[1], pdb[1], pf[2], pdb[2]);
        }
        if (e.activeVoices() != 0)
        {
            if (notFreed < 14)
            {
                for (int i = 0; i < Engine::kMaxVoices; ++i)
                {
                    const Voice& v = e.voice (i);
                    if (! v.active) continue;
                    const Globals& gg = e.globals();
                    const Derived& dv = v.derived();
                    const double h0 = (dv.lb0 + dv.lb1) / (1.0 + dv.la1), hpi = (dv.lb0 - dv.lb1) / (1.0 - dv.la1);
                    std::printf ("    not freed: trial %d midi %d pg %.3g Pa peak %.3g | loss filter |H| DC %.4f Nyq %.4f (a1 %.4f) | wind %.0f loss %.2f jet %.2f edge %.3f cMult %.2f rhoMult %.2f morph %.2f\n"
                                 "        P5: fm %s depth %.2f ratio %.2f | hole %.2f | cross %.2f | nick %.2f | excite %.0f | matrix %.0f>%.0f %.2f, %.0f>%.0f %.2f, %.0f>%.0f %.2f, %.0f>%.0f %.2f, LFO %.2f / %.2f Hz | resets %d\n",
                                 t, v.midi, v.pg, v.peakLevel, std::abs (h0), std::abs (hpi), dv.la1, gg.wind, gg.loss, gg.jetGain, gg.edge, gg.cMult, gg.rhoMult, gg.morph,
                                 gg.fmTarget < 0.5 ? "wind" : (gg.fmTarget < 1.5 ? "length" : "lip"), gg.fmDepth, gg.fmRatio, gg.sideHole, gg.crossDrive, gg.nicking, gg.excite,
                                 gg.modSrc1, gg.modDst1, gg.modAmt1, gg.modSrc2, gg.modDst2, gg.modAmt2, gg.modSrc3, gg.modDst3, gg.modAmt3, gg.modSrc4, gg.modDst4, gg.modAmt4, gg.lfo1Rate, gg.lfo2Rate, v.resets);
                }
            }
            ++notFreed;
        }
    }
    std::printf ("  %d trials: non-finite / out-of-range output %d, trials with a voice reset %d (%d resets), peak %.3f\n"
                 "  after release: all voices freed in %d trials; still ringing at 25 s but decaying %d (very low wall loss, low stopped pipes); sustained %d\n",
                 trials, bad, withResets, totalResets, peak, trials - notFreed, longRings, sustained);
    check (bad == 0, "stress output finite and within +-1");
    check (sustained == 0, "nothing keeps sounding by itself after release");
}

/* ======================================================= derive() vs C core */
void testDerive()
{
    std::printf ("\n== derive(): C++ (analytic bracket) vs the Phase 1 C core (scan + bisection) ==\n");
    std::mt19937 rng (7);
    std::uniform_real_distribution<double> U (0.0, 1.0);
    const double rates[] { 88200.0, 96000.0, 192000.0, 48000.0 };
    double worstA = 0, worstB = 0, worstD = 0;
    int n = 0, big = 0, clampedN = 0;
    for (int trial = 0; trial < 20000; ++trial)
    {
        Globals g;
        for (int i = 0; i < kNumParams; ++i)
        {
            const ParamDef& d = kParamDefs[i];
            double x = U (rng);
            const double r = U (rng);
            if (r < 0.1) x = 0; else if (r < 0.2) x = 1; else if (r < 0.5) x = toNorm (d, d.def);
            g.*(d.field) = fromNorm (d, x);
        }
        const int midi = 24 + (int) (U (rng) * 85);
        ParamVec p;
        voiceParams (midi, g, VoiceOverride {}, designPrinzipal (midi), p);
        p[P_KCAL] *= std::exp2 ((U (rng) - 0.5) * 300.0 / 1200.0);     // servo corrections
        const double fs = rates[trial % 4];
        Derived dv;
        derive (p.data(), fs, dv);
        double ref[8];
        labium_debug_derive (p.data(), fs, ref);
        const double rg = std::max (std::abs ((ref[0] + ref[1]) / (1.0 + ref[2])), std::abs ((ref[0] - ref[1]) / (1.0 - ref[2])));
        if (rg > 0.9995) { ++clampedN; continue; }       // P3 scales non-passive fits back
        const double dA = std::abs (dv.la1 - ref[2]), dB = std::max (std::abs (dv.lb0 - ref[0]), std::abs (dv.lb1 - ref[1]));
        const double dD = std::abs (dv.dline - ref[3]);
        if (dA > 1e-8 || dB > 1e-8 || dD > 1e-6) ++big;
        worstA = std::max (worstA, dA); worstB = std::max (worstB, dB); worstD = std::max (worstD, dD);
        ++n;
    }
    std::printf ("  %d random parameter sets (4 sample rates): max |d a1| %.2e, max |d b| %.2e, max |d delay| %.2e samples; %d differ by more than 1e-8\n"
                 "  (%d more sets where the Phase 1 fit gains more than 1 were left out: P3 scales those back)\n",
                 n, worstA, worstB, worstD, big, clampedN);
    check (big == 0, "derive() matches the Phase 1 C core");
}

/* ================================================================= 5. midi */
struct TestSink : MidiRouter::Sink
{
    int last = -1; double norm = -1; int count = 0;
    void midiParam (int index, double n) override { last = index; norm = n; ++count; }
};

void testMidi()
{
    std::printf ("\n== 5. MIDI routing ==\n");
    Engine e;
    e.prepare (48000, 256);
    e.setGlobals (Globals {});
    MidiRouter r (e);
    TestSink s;
    r.setSink (&s);
    std::vector<float> L (256), R (256);
    auto send = [&] (int a, int b, int c) { const uint8_t m[3] { (uint8_t) a, (uint8_t) b, (uint8_t) c }; r.handle (m, 3); };

    send (0x90, 60, 100);                          // keyboard c1, channel 1
    e.process (L.data(), R.data(), 256);
    check (e.activeVoices() == 1, "keyboard note starts a voice");
    send (0x99, 36, 90);                           // pad A1 on channel 10: modulation, no pipe
    e.process (L.data(), R.data(), 256);
    check (e.activeVoices() == 1 && r.padA[0].load() > 0.6f, "pad A1 (ch10 note 36) is a modulation pad, not a C pipe");
    send (0xA9, 36, 127);                          // poly pressure
    Globals g;
    r.applyPads (g);
    check (std::abs (g.wind - Globals {}.wind * 5.0) < 1e-6, "pad A1 pressure 127 -> wind x5");
    send (0x89, 36, 0);
    send (0x99, 44, 100);                          // pad B1: wind pipe C (MIDI 36), id 1000
    e.process (L.data(), R.data(), 256);
    check (e.activeVoices() == 2, "pad B1 (ch10 note 44) starts a wind pipe");
    send (0x90, 44, 100);                          // note 44 on channel 1 is a keyboard key
    e.process (L.data(), R.data(), 256);
    check (e.activeVoices() == 3, "note 44 on channel 1 is a keyboard pipe");
    send (0xB0, 74, 127);
    check (s.last == paramIndex ("cutup") && s.norm == 1.0, "CC74 -> cut-up");
    send (0xB0, 93, 0);
    check (s.last == paramIndex ("bellows") && s.norm == 0.0, "CC93 (knob 5) -> wind pressure");
    send (0xB0, 83, 64);
    check (s.last == paramIndex ("trem"), "CC83 (fader 2) -> tremulant");
    send (0xE0, 0, 127);                            // pitch bend up
    check (s.last == paramIndex ("glide") && s.norm > 0.99, "pitch bend -> length glide");
    r.learnParam.store (paramIndex ("noise"));
    send (0xB0, 20, 50);
    check (s.last == paramIndex ("noise") && r.ccMap[20].load() == paramIndex ("noise") && r.learnParam.load() == -1, "MIDI learn: CC20 -> turbulence");
    send (0xB0, 64, 127);                           // sustain
    send (0x80, 60, 0);
    e.process (L.data(), R.data(), 256);
    check (e.activeVoices() == 3, "sustain holds the released key");
    send (0xB0, 120, 0);                            // all sound off
    e.process (L.data(), R.data(), 256);
    check (e.activeVoices() == 0, "CC120 silences everything");
    std::printf ("  %s\n", g_fail ? "(see failures above)" : "all routing checks passed");
}

/* trace of the servo over time for one state and key */
void traceState (const char* modName, int key)
{
    const Mod* md = nullptr;
    for (const auto& m : kMods) if (std::string (m.name) == modName) md = &m;
    if (! md) return;
    Globals G;
    md->apply (G);
    const PipeDesign pd = designPrinzipal (key);
    ParamVec p;
    voiceParams (key, G, VoiceOverride {}, pd, p);
    const double fs = FS, dur = 2.6;
    const int n = (int) (dur * fs), hop = (int) (0.05 * fs);
    Voice v;
    v.prepare (fs, 3);
    v.start (p, 1200.0 * std::log2 (kcalFor (key)));
    std::vector<double> mouth ((size_t) n), L ((size_t) n), R ((size_t) n);
    std::printf ("\n== trace: %s, key %d (target %.2f Hz) ==\n   t     meas Hz  cand  lock  regime scan share    D      fb    | spectral f (50 ms)\n", modName, key, p[P_FTARGET]);
    for (int i = 0; i < n; i += hop)
    {
        const int m = std::min (hop, n - i);
        v.render (m, L.data() + i, R.data() + i, mouth.data() + i);
        if (i < 0.1 * fs) continue;
        double pf[1], pdb[1];
        spectralPeaks (mouth.data() + i, (size_t) m, fs, 1, pf, pdb);
        std::printf ("  %4.2f %9.2f  %4.1f  %d    %4.1f   %d   %5.2f %7.2f %7.2f | %8.1f\n", (i + m) / fs, v.measuredHz, v.measuredRatio, v.measuredLocked ? 1 : 0,
                     v.servoRegime(), v.servoScanning() ? 1 : 0, v.servoShare(), v.servoCents(), p[P_C] / (2.0 * v.derived().Leff), pf[0]);
    }
}

/* diagnostics: one state, per key, what the servo sees vs the reference estimator */
void diagState (const char* modName)
{
    const Mod* md = nullptr;
    for (const auto& m : kMods) if (std::string (m.name) == modName) md = &m;
    if (! md) { std::printf ("unknown state %s\n", modName); return; }
    std::printf ("\n== diag: %s ==\n", modName);
    std::printf ("  key  f target   est f    ratio  cents | servo: meas f  locked   D[c]   fb     est@0.6-1.2s est@1.8-2.6s\n");
    for (int k : { 36, 48, 60, 72, 84 })
    {
        Globals G;
        md->apply (G);
        const PipeDesign pd = designPrinzipal (k);
        ParamVec p;
        voiceParams (k, G, VoiceOverride {}, pd, p);
        const double fs = FS, dur = 2.6;
        const int n = (int) (dur * fs);
        Voice v;
        v.prepare (fs, 3);
        v.start (p, 1200.0 * std::log2 (kcalFor (k)));
        std::vector<double> mouth ((size_t) n), L ((size_t) n), R ((size_t) n);
        int nLocked = 0, nMeas = 0;
        double lastHz = 0;
        for (int i = 0; i < n; i += 512)
        {
            v.render (std::min (512, n - i), L.data() + i, R.data() + i, mouth.data() + i);
            if (i > 1.0 * fs) { nMeas++; if (v.measuredLocked) nLocked++; }
            lastHz = v.measuredHz;
        }
        const double ft = p[P_FTARGET];
        const size_t i0 = (size_t) ((dur - 0.6) * fs);
        double clar = 0;
        const double f = estPitch (mouth.data() + i0, mouth.size() - i0, fs, ft, 0.4, 4.5, &clar);
        double pf[4], pdb[4];
        spectralPeaks (mouth.data() + i0, mouth.size() - i0, fs, 4, pf, pdb);
        const double fA = estPitch (mouth.data() + (size_t) (0.6 * fs), (size_t) (0.6 * fs), fs, ft);
        const double fB = estPitch (mouth.data() + (size_t) (1.8 * fs), (size_t) (0.8 * fs), fs, ft);
        const double r = f / ft;
        const double cand = 0.5 * std::clamp ((int) std::lround (2.0 * r), 1, 16);
        double rc;
        const double err = pitchError (mouth.data() + i0, mouth.size() - i0, fs, ft, &rc);
        std::printf ("  %3d %9.2f err %7.2f (x%.1f) | servo %9.2f x%.1f %3d/%-3d D %7.2f fb %7.2f | est %9.2f clar %.3f  peaks %.1f(%.0f) %.1f(%.0f) %.1f(%.0f) %.1f(%.0f)\n", k, ft, err, rc, lastHz, v.measuredRatio, nLocked, nMeas,
                     v.servoCents(), v.derived().Leff > 0 ? v.frame()[P_C] / (2.0 * v.derived().Leff) : 0.0, f, clar, pf[0], pdb[0], pf[1], pdb[1], pf[2], pdb[2], pf[3], pdb[3]);
        (void) r; (void) cand; (void) fA; (void) fB;
    }
}

} // namespace

namespace okl
{
double malletTestShellDelta (double lam, int n, double nu, double k, int root);
double malletTestShellLowest (double lam, int n, double nu, double k);
double malletTestBeamRoot (int m, double mu, double K);
void malletTestBeamIntegrals (double lam, double& x2, double& x1, double& xEnd);
double malletTestBeamX (double lam, double xi);
}

namespace
{
/* ================================================================ 6. mallet */
StrikeParams pipeOf (int midi, const Globals& G)
{
    ParamVec p;
    voiceParams (midi, G, VoiceOverride {}, designPrinzipal (midi), p);
    StrikeParams s;
    s.L = p[P_LPHYS]; s.d = p[P_D]; s.wall = p[P_WALL];
    s.metal = (int) p[P_METAL]; s.head = (int) p[P_HEAD]; s.headD = p[P_HEADD] * 1e-3;
    s.strike = p[P_STRIKE]; s.damper = p[P_DAMPER]; s.morph = p[P_MORPH];
    s.c0 = p[P_C]; s.rho0 = p[P_RHO]; s.tempK = p[P_TEMPK];
    return s;
}

/* frequency of mode (n, m) of a struck pipe (-1: not among the chosen modes) */
double modeHz (const MalletModel& mdl, int n, int m)
{
    double f = -1;
    for (int i = 0; i < mdl.modeCount(); ++i)
        if (mdl.modeN (i) == n && mdl.modeM (i) == m && (f < 0 || mdl.modeFreq (i) < f)) f = mdl.modeFreq (i);
    return f;
}
double modeT60 (const MalletModel& mdl, int n, int m)
{
    for (int i = 0; i < mdl.modeCount(); ++i) if (mdl.modeN (i) == n && mdl.modeM (i) == m) return mdl.modeT60 (i);
    return -1;
}

void testMallet()
{
    std::printf ("\n== 6. mallet: struck pipe body (shell modes, Hertz contact, radiation) ==\n");
    // --- shell theory against its limits
    {
        const double nu = 0.3, k = 1e-4;
        double worstRing = 0;
        for (int n = 2; n <= 8; ++n)
        {
            const double ring = k * n * n * std::pow (n * n - 1.0, 2) / (n * n + 1.0);     // inextensional ring (Love)
            worstRing = std::max (worstRing, std::abs (malletTestShellDelta (1e-4, n, nu, k, 0) / ring - 1.0));
        }
        double worstBeam = 0;
        double worstFast = 0;
        for (double L : { 0.005, 0.01, 0.02 })
            worstBeam = std::max (worstBeam, std::abs (malletTestShellDelta (L, 1, nu, k, 0) / ((1 - nu * nu) * std::pow (L, 4) / 2.0) - 1.0));
        for (int n = 1; n <= 12; ++n)
            for (double L : { 0.01, 0.1, 0.5, 1.5 })
                worstFast = std::max (worstFast, std::abs (malletTestShellLowest (L, n, nu, k) / malletTestShellDelta (L, n, nu, k, 0) - 1.0));
        const double bar = malletTestShellDelta (0.01, 0, nu, k, 1) / ((1 - nu * nu) * 1e-4) - 1.0;     // (root 0 is torsion)
        const double breathing = malletTestShellDelta (1e-3, 0, nu, k, 2) - 1.0;
        std::printf ("  Flügge limits: ring n=2..8 %.1e, beam (Euler-Bernoulli) %.1e, bar %.1e, breathing %.1e (relative)\n",
                     worstRing, worstBeam, std::abs (bar), std::abs (breathing));
        std::printf ("  fast flexural root vs full 3x3 solution: %.1e\n", worstFast);
        check (worstRing < 2e-3 && worstBeam < 2e-2 && std::abs (bar) < 2e-3 && std::abs (breathing) < 2e-3, "shell modes reproduce ring, beam, bar and breathing limits");
        check (worstFast < 1e-6, "fast flexural root = full solution");
    }
    // --- beam functions
    {
        const double r0 = malletTestBeamRoot (1, 0, 0), rM = malletTestBeamRoot (1, 1.0, 0), rK = malletTestBeamRoot (1, 0, 1e9);
        double x2, x1, xe;
        malletTestBeamIntegrals (malletTestBeamRoot (3, 0, 0), x2, x1, xe);
        // the same integrals numerically
        double s2 = 0, s1 = 0;
        const int N = 4000;
        const double l3 = malletTestBeamRoot (3, 0, 0);
        for (int i = 0; i <= N; ++i)
        {
            const double w = (i == 0 || i == N) ? 1 : (i % 2 ? 4 : 2), x = malletTestBeamX (l3, i / (double) N);
            s2 += w * x * x; s1 += w * x;
        }
        s2 /= 3.0 * N; s1 /= 3.0 * N;
        std::printf ("  beam roots: cantilever %.5f (1.87510), tip mass = beam mass %.5f (1.24792), pinned tip %.5f (3.92660); int X^2 %.6f / %.6f, int X %.6f / %.6f\n",
                     r0, rM, rK, x2, s2, x1, s1);
        check (std::abs (r0 - 1.87510) < 1e-4 && std::abs (rM - 1.24792) < 1e-4 && std::abs (rK - 3.92660) < 1e-3, "beam roots");
        check (std::abs (x2 - s2) < 1e-6 && std::abs (x1 - s1) < 1e-6 && std::abs (x2 - 1.0) < 1e-6, "beam integrals (closed form = numerical; cantilever normalised to 1)");
    }
    // --- contact against Hertz theory: a soft head on a thick zinc wall is nearly a rigid target
    {
        MalletModel m;
        m.prepare (96000);
        Globals G;
        StrikeParams s = pipeOf (60, G);
        s.metal = 3; s.wall = 2.0; s.head = 0; s.headD = 0.025; s.strike = 0.5;
        m.setParams (s);
        m.strike (1.0);
        std::vector<double> sh (9600), q (9600);
        m.render (9600, sh.data(), q.data());
        const double tH = 3.2145 * std::pow (m.malletMass() * m.malletMass() / (m.contactStiffness() * m.contactStiffness() * 1.0), 0.2);
        std::printf ("  contact, rubber on thick zinc at 1 m/s: %.3f ms (Hertz, rigid wall: %.3f ms), peak force %.1f N\n",
                     m.lastContactTime() * 1e3, tH * 1e3, m.lastPeakForce());
        check (m.lastContactTime() > 0.95 * tH && m.lastContactTime() < 1.15 * tH, "contact time follows Hertz (soft head, stiff wall)");
    }
    // --- a c1 pipe: the spectrum of the sound is the model's modes, decays follow the losses
    {
        MalletModel m;
        m.prepare (96000);
        Globals G;
        G.excite = 1;
        const StrikeParams s = pipeOf (60, G);
        m.setParams (s);
        m.strike (Engine::malletSpeed (100));
        const int N = 96000 * 2;
        std::vector<double> sh ((size_t) N), q ((size_t) N);
        for (int i = 0; i < N; i += 128) m.render (128, sh.data() + i, q.data() + i);
        double pk = 0;
        for (double x : sh) pk = std::max (pk, std::abs (x));
        std::printf ("  c1, spotted metal, acrylic 25 mm at %.2f m/s: contact %.3f ms, peak force %.1f N, %d modes (%d ringing after 2 s), peak %.1f dB SPL at 1 m\n",
                     Engine::malletSpeed (100), m.lastContactTime() * 1e3, m.lastPeakForce(), m.modeCount(), m.activeModes(), 20 * std::log10 (pk / 2e-5));
        // strongest peaks of 0.1 .. 1.1 s against the nearest mode
        double fr[8], db[8];
        spectralPeaks (sh.data() + 9600, 96000, 96000, 8, fr, db);
        double worst = 0;
        int matched = 0;
        std::printf ("  spectral peaks vs modes:");
        for (int i = 0; i < 8; ++i)
        {
            if (fr[i] <= 0 || db[i] < -40) continue;
            double best = 1e9; int bi = -1;
            for (int k = 0; k < m.modeCount(); ++k) if (std::abs (m.modeFreq (k) - fr[i]) < best) { best = std::abs (m.modeFreq (k) - fr[i]); bi = k; }
            std::printf (" %.1f (%d,%d %.1f)", fr[i], m.modeN (bi), m.modeM (bi), m.modeFreq (bi));
            worst = std::max (worst, best / fr[i]);
            ++matched;
        }
        std::printf ("\n");
        check (matched >= 4 && worst < 0.003, "spectral peaks are the model's mode frequencies");
        // decay of the strongest low peak between two windows 1 s apart
        auto bandAmp = [&] (int from, double f) {
            std::vector<double> seg (sh.begin() + from, sh.begin() + from + 48000);
            double frs[1], dbs[1];
            double amp = 0;
            const size_t M = 65536;
            std::vector<std::complex<double>> a (M);
            for (size_t i = 0; i < seg.size(); ++i) a[i] = seg[i] * (0.5 - 0.5 * std::cos (kTwoPi * i / (seg.size() - 1)));
            fft (a);
            const size_t kc = (size_t) std::lround (f * M / 96000.0);
            for (size_t k = kc - 3; k <= kc + 3; ++k) amp = std::max (amp, std::abs (a[k]));
            (void) frs; (void) dbs;
            return amp;
        };
        // the strongest peak: its decay between two half-second windows 0.5 s apart vs the mode's losses
        {
            int bi = -1;
            for (int k = 0; k < m.modeCount(); ++k) if (bi < 0 || std::abs (m.modeFreq (k) - fr[0]) < std::abs (m.modeFreq (bi) - fr[0])) bi = k;
            const double f = m.modeFreq (bi), t60 = m.modeT60 (bi);
            const double r = bandAmp (9600, f) / std::max (1e-30, bandAmp (9600 + 48000, f));
            const double t60m = 0.5 * 6.9078 / std::log (std::max (1.0000001, r));
            std::printf ("  decay of mode (%d,%d) %.1f Hz: T60 %.2f s measured, %.2f s model\n", m.modeN (bi), m.modeM (bi), f, t60m, t60);
            check (std::abs (t60m / t60 - 1.0) < 0.15, "a mode decays as its losses say");
        }
    }
    // --- the physics moves the right way
    {
        Globals G;
        G.excite = 1;
        auto design = [&] (StrikeParams s) { MalletModel m; m.prepare (96000); m.setParams (s); m.strike (1.0); return m; };
        const StrikeParams base = pipeOf (60, G);
        const MalletModel m0 = design (base);
        StrikeParams thick = base; thick.wall = 2.0;
        const MalletModel mThick = design (thick);
        StrikeParams zinc = base; zinc.metal = 3;
        StrikeParams lead = base; lead.metal = 0;
        const MalletModel mZn = design (zinc), mPb = design (lead);
        StrikeParams he = base; { const GasProps gp = gasForKnob (1.0, 20.0); he.c0 = gp.c; he.rho0 = gp.rho; }
        const MalletModel mHe = design (he);
        StrikeParams cap = base; cap.morph = 1.0;
        const MalletModel mCap = design (cap);
        const double r2 = modeHz (mThick, 2, 1) / modeHz (m0, 2, 1);
        const MetalProps& zn = metalProps (3); const MetalProps& pb = metalProps (0);
        const double rBeamExp = std::sqrt ((zn.E / zn.rho) / (pb.E / pb.rho)), rBeam = modeHz (mZn, 1, 2) / modeHz (mPb, 1, 2);
        std::printf ("  c1 modes (n,m): (1,1) %.1f  (1,2) %.1f  (2,1) %.1f  (3,1) %.1f  (0,1) %.0f Hz; T60 (1,2) %.2f s, (2,1) %.2f s\n",
                     modeHz (m0, 1, 1), modeHz (m0, 1, 2), modeHz (m0, 2, 1), modeHz (m0, 3, 1), modeHz (m0, 0, 1), modeT60 (m0, 1, 2), modeT60 (m0, 2, 1));
        std::printf ("  wall x2: ovalling (2,1) x%.3f; zinc / common metal: bending (1,2) x%.3f (sqrt(E/rho) x%.3f), T60 (1,2) %.2f / %.2f s;\n"
                     "  helium: (2,1) %+.2f cents (lighter gas loads the wall less); capped top: (2,1) x%.3f, (1,1) x%.3f (cap mass)\n",
                     r2, rBeam, rBeamExp, modeT60 (mZn, 1, 2), modeT60 (mPb, 1, 2),
                     1200 * std::log2 (modeHz (mHe, 2, 1) / modeHz (m0, 2, 1)), modeHz (mCap, 2, 1) / modeHz (m0, 2, 1), modeHz (mCap, 1, 1) / modeHz (m0, 1, 1));
        check (r2 > 1.7 && r2 < 2.1, "ovalling frequency ~ wall thickness");
        check (std::abs (rBeam / rBeamExp - 1.0) < 0.03, "bending frequency ~ sqrt(E / rho) of the metal");
        check (modeT60 (mZn, 1, 2) > 2.0 * modeT60 (mPb, 1, 2), "zinc rings longer than lead-rich metal");
        check (modeHz (mHe, 2, 1) > modeHz (m0, 2, 1), "helium: less added mass, higher ovalling frequency");
        check (modeHz (mCap, 2, 1) > 1.02 * modeHz (m0, 2, 1) && modeHz (mCap, 1, 1) < modeHz (m0, 1, 1), "cap: holds the rim (ovalling up), adds mass (bending down)");
    }
    // --- through the engine: strikes, damper, re-strike, all materials and heads, random extremes
    {
        Engine e;
        e.prepare (48000, 480);
        Globals G;
        G.excite = 1;
        G.reverb = 0;
        e.setGlobals (G);
        std::vector<float> L (480), R (480);
        e.noteOn (60, 60, 100);
        double pk = 0, tail = 0;
        for (int b = 0; b < 100; ++b)
        {
            e.process (L.data(), R.data(), 480);
            for (int i = 0; i < 480; ++i) pk = std::max (pk, (double) std::abs (L[(size_t) i]));
        }
        e.noteOff (60);
        int freed = -1;
        for (int b = 0; b < 1500 && freed < 0; ++b)
        {
            e.process (L.data(), R.data(), 480);
            if (e.activeVoices() == 0) freed = b;
            tail = 0;
            for (int i = 0; i < 480; ++i) tail = std::max (tail, (double) std::abs (L[(size_t) i]));
        }
        std::printf ("  engine c1 (vel 100): peak %.3f FS, key up with the damper: silent and freed after %.2f s\n", pk, freed * 0.01);
        check (pk > 0.01 && pk <= 1.0, "struck pipe sounds, within full scale");
        check (freed >= 0 && freed < 400, "the damper stops it (voice freed within 4 s)");

        std::mt19937 rng (7);
        std::uniform_real_distribution<double> U (0, 1);
        int bad = 0;
        double worstPk = 0;
        for (int t = 0; t < 120; ++t)
        {
            Engine x;
            x.prepare (t % 3 == 0 ? 44100 : (t % 3 == 1 ? 48000 : 96000), 256);
            Globals g;
            g.excite = 1; g.reverb = 0;
            g.metal = (double) (t % 5); g.head = (double) ((t / 5) % 4);
            g.wallMult = 0.5 * std::pow (4.0, U (rng)); g.headD = 10 + 40 * U (rng); g.strikePos = 0.05 + 0.9 * U (rng);
            g.damper = U (rng); g.morph = U (rng) < 0.3 ? 1.0 : (U (rng) < 0.5 ? 0.0 : U (rng));
            g.scaleHT = -20 + 36 * U (rng); g.glide = -12 + 24 * U (rng); g.gas = -1 + 2 * U (rng); g.tempC = -30 + 110 * U (rng);
            g.cMult = 0.5 * std::pow (4.0, U (rng)); g.rhoMult = 0.2 * std::pow (25.0, U (rng));
            x.setGlobals (g);
            std::vector<float> l (256), r (256);
            const int notes[3] { 24 + (int) (84 * U (rng)), 24 + (int) (84 * U (rng)), 24 + (int) (84 * U (rng)) };
            for (int b = 0; b < 400; ++b)
            {
                if (b % 40 == 0) x.noteOn (notes[b / 40 % 3], notes[b / 40 % 3], 1 + (int) (126 * U (rng)));
                if (b == 200) { g.glide = -g.glide; g.wallMult *= 1.3; g.strikePos = 1.0 - g.strikePos; x.setGlobals (g); }
                x.process (l.data(), r.data(), 256);
                for (int i = 0; i < 256; ++i)
                {
                    if (! std::isfinite (l[(size_t) i]) || std::abs (l[(size_t) i]) > 1.0f) ++bad;
                    worstPk = std::max (worstPk, (double) std::abs (l[(size_t) i]));
                }
            }
        }
        std::printf ("  120 random struck pipes (all metals and heads, extremes, re-strikes, knobs moving while ringing): %d bad samples\n", bad);
        check (bad == 0, "struck pipes: finite, within full scale");
    }
    // --- every knob: does it change a struck pipe? (wind and jet must not; geometry, gas, mouth, metal, mallet must)
    {
        auto strike = [] (const Globals& g, bool release) {
            Engine x;
            x.prepare (48000, 480);
            x.setGlobals (g);
            std::vector<float> L (480), R (480), out;
            x.noteOn (60, 60, 100);
            for (int b = 0; b < 120; ++b)
            {
                if (release && b == 30) x.noteOff (60);
                x.process (L.data(), R.data(), 480);
                out.insert (out.end(), L.begin(), L.end());
            }
            return out;
        };
        Globals base;
        base.excite = 1; base.reverb = 0;
        const std::vector<float> ref = strike (base, false), refRel = strike (base, true);
        double pk = 0;
        for (float v : ref) pk = std::max (pk, (double) std::abs (v));
        struct K { const char* id; double v; bool acts; bool release; };
        const K knobs[] = {
            // no wind, no jet: nothing to act on
            { "bellows", 4, false, false }, { "trem", 1, false, false }, { "tremRate", 10, false, false },
            { "velSens", 0, false, false }, { "y0b", -2, false, false }, { "noise", 0.05, false, false }, { "toe", 1.7, false, false },
            { "pitchLock", 0, false, false }, { "fmDepth", 0.6, false, false }, { "fmRatio", 3, false, false }, { "jetGain", 2.5, false, false },
            { "kappa", 2.4, false, false }, { "edge", 0.3, false, false }, { "kick", 0.4, false, false },
            { "nicking", 1, false, false }, { "crossDrive", 0.8, false, false },
            // the pipe body, its metal and gas, the mouth (air column), the mallet
            { "scaleHT", 6, true, false }, { "morph", 1, true, false }, { "glide", 5, true, false }, { "gas", 0.8, true, false },
            { "tempC", 70, true, false }, { "cMult", 1.5, true, false }, { "rhoMult", 3, true, false }, { "cutup", 2, true, false },
            { "mouthFrac", 0.12, true, false }, { "loss", 8, true, false }, { "metal", 3, true, false }, { "wallMult", 1.6, true, false },
            { "head", 3, true, false }, { "headD", 45, true, false }, { "strikePos", 0.7, true, false }, { "damper", 0.0, true, true },
            { "sideHole", 0.5, true, false },
        };
        int wrong = 0;
        std::printf ("  knob -> change of the struck c1 (max |difference| re peak, 2.4 s; damper with the key released after 0.3 s):\n   ");
        int col = 0;
        for (const auto& k : knobs)
        {
            Globals g = base;
            g.*(kParamDefs[paramIndex (k.id)].field) = k.v;
            const std::vector<float> o = strike (g, k.release);
            const std::vector<float>& r0 = k.release ? refRel : ref;
            double d = 0;
            for (size_t i = 0; i < o.size(); ++i) d = std::max (d, (double) std::abs (o[i] - r0[i]));
            const double rel = d / pk;
            const bool acts = rel > 1e-4;
            if (acts != k.acts) ++wrong;
            std::printf (" %s%s %s%.0e", acts != k.acts ? "!!" : "", k.id, acts ? "" : "=", rel);
            if (++col % 6 == 0) std::printf ("\n   ");
        }
        std::printf ("\n");
        check (wrong == 0, "struck pipe: wind and jet knobs change nothing, the physical ones do");
    }
    // --- CPU: 32 pipes struck at once (the worst case: every mode rings)
    {
        Engine e;
        e.prepare (48000, 512);
        Globals G;
        G.excite = 1;
        e.setGlobals (G);
        std::vector<float> L (512), R (512);
        const auto t0 = std::chrono::steady_clock::now();
        for (int k = 0; k < 32; ++k) e.noteOn (36 + k * 2, 36 + k * 2, 100);
        const double tStrike = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
        const int blocks = 94;       // 1 s
        const auto t1 = std::chrono::steady_clock::now();
        for (int b = 0; b < blocks; ++b) e.process (L.data(), R.data(), 512);
        const double s = std::chrono::duration<double> (std::chrono::steady_clock::now() - t1).count();
        std::printf ("  32 pipes struck together: %.2f ms to set up the strokes, then %.1f %% of one core for the first second\n", tStrike * 1e3, s / (blocks * 512 / 48000.0) * 100);
    }
}

/* ============================================================ 2b. wind pressure */
struct WindPoint { double db, regime, cents; };
WindPoint windPoint (int midi, double wind, double lock)
{
    Globals G;
    G.wind = wind;
    G.pitchLock = lock;
    ParamVec p;
    voiceParams (midi, G, VoiceOverride {}, designPrinzipal (midi), p);
    Voice v;
    engineJet (v);
    v.prepare (FS, 3);
    v.start (p, 1200.0 * std::log2 (kcalFor (midi)));
    const int n = (int) (2.5 * FS);
    std::vector<double> mouth ((size_t) n), L ((size_t) n), R ((size_t) n);
    for (int i = 0; i < n; i += 4096) v.render (std::min (4096, n - i), L.data() + i, R.data() + i, mouth.data() + i);
    const Metrics m = analyze (mouth, FS, p[P_FTARGET], 1.5);
    return { m.db, m.regime, cents (m.f, p[P_FTARGET] * std::max (0.5, std::round (2.0 * m.regime) / 2.0)) };
}

void testWind()
{
    std::printf ("\n== 2b. wind pressure: below the voicing pressure the pipe weakens, goes flat, and stops speaking ==\n");
    int bad = 0;
    for (int midi : { 48, 60, 72 })
    {
        const WindPoint w5 = windPoint (midi, 5, 0), w20 = windPoint (midi, 20, 0), w30 = windPoint (midi, 30, 0), w75 = windPoint (midi, 75, 0), w300 = windPoint (midi, 300, 0);
        std::printf ("  MIDI %d (pitch lock 0): 5 mmWS %.1f dB (regime %.2f) | 20: %.1f dB %+.1f c | 30: %.1f dB %+.1f c | 75: %.1f dB %+.1f c | 300: %.1f dB %+.1f c\n",
                     midi, w5.db, w5.regime, w20.db, w20.cents, w30.db, w30.cents, w75.db, w75.cents, w300.db, w300.cents);
        if (! (w5.db < w75.db - 15.0)) ++bad;                                   // no tone at 5 mmWS (breath only, or a faint whistle)
        if (! (w20.db < w30.db && w30.db < w75.db && w75.db < w300.db)) ++bad;  // louder with more wind
        if (std::abs (w20.regime - 1.0) > 0.05 || std::abs (w30.regime - 1.0) > 0.05) ++bad;   // still the fundamental, no loud upper regime
        if (! (w20.cents < w30.cents && w30.cents < w75.cents)) ++bad;          // flat at low pressure
    }
    check (bad == 0, "low wind: quieter and flatter towards the threshold, silent below it; no upper regime at low pressure");
}

/* =============================================================== 8. step 5: Lab */
/* a steady note through the engine (48 kHz, no reverb): left channel, after `from` seconds */
std::vector<float> engineNote (const Globals& g, int midi, double dur, double from = 1.0, int vel = 100)
{
    Engine e;
    e.prepare (48000, 480);
    Globals gg = g;
    gg.reverb = 0;
    e.setGlobals (gg);
    std::vector<float> L (480), R (480), out;
    e.noteOn (midi, midi, vel);
    const int blocks = (int) (dur * 100);
    for (int b = 0; b < blocks; ++b)
    {
        e.process (L.data(), R.data(), 480);
        if (b >= (int) (from * 100)) out.insert (out.end(), L.begin(), L.end());
    }
    return out;
}
double strongestHz (const std::vector<float>& x)
{
    std::vector<double> d (x.begin(), x.end());
    double fr[1], db[1];
    spectralPeaks (d.data(), d.size(), 48000, 1, fr, db);
    return fr[0];
}
/* level of the spectrum near f (dB re the strongest component) */
double levelAt (const std::vector<float>& x, double f)
{
    const size_t N = nextPow2 (x.size());
    std::vector<std::complex<double>> a (N);
    for (size_t i = 0; i < x.size(); ++i) a[i] = x[i] * (0.5 - 0.5 * std::cos (2 * kPi * i / (x.size() - 1)));
    fft (a);
    double mx = 1e-30, at = 1e-30;
    const size_t kf = (size_t) std::lround (f * N / 48000.0);
    for (size_t k = 1; k < N / 2; ++k) mx = std::max (mx, std::abs (a[k]));
    for (size_t k = kf - 3; k <= kf + 3; ++k) at = std::max (at, std::abs (a[k]));
    return 20 * std::log10 (at / mx);
}

/* ===================================================== 2c. low wind, engine */
/* the regression check for "a very weak wind still sounds loud", as the plugin plays it: the engine, pitch lock 100 %
   (the default), velocity 100, no reverb; level of both channels over 1.5 .. 2.5 s */
double engineLevelDb (double wind, int midi, double jitter)
{
    Engine e;
    e.jetJitter = jitter;
    if (jitter == 0.0) { e.jetSpread = e.jetSat = e.jetLipVoice = 0.0; e.topPath = false; }     // Phase 1's jet and mix
    e.prepare (48000, 480);
    Globals g;
    g.reverb = 0; g.wind = wind;
    e.setGlobals (g);
    std::vector<float> L (480), R (480);
    e.noteOn (midi, midi, 100);
    double s = 0;
    long cnt = 0;
    for (int b = 0; b < 250; ++b)
    {
        e.process (L.data(), R.data(), 480);
        if (b >= 150) for (int i = 0; i < 480; ++i) { s += (double) L[i] * L[i] + (double) R[i] * R[i]; cnt += 2; }
    }
    return 10 * std::log10 (s / (double) cnt + 1e-30);
}

void testWindEngine()
{
    std::printf ("\n== 2c. low wind through the engine (the plugin's path: pitch lock 100 %%, velocity 100, no reverb) ==\n");
    const double winds[] { 5, 10, 20, 40, 75, 150, 300 };
    constexpr int NW = 7, i75 = 4;
    int bad = 0, oldBad = 0;
    for (int midi : { 48, 60, 72 })
    {
        double db[NW];
        for (int i = 0; i < NW; ++i) db[i] = engineLevelDb (winds[i], midi, Engine::kJetJitter);
        const double slope = (db[6] - db[2]) / std::log2 (300.0 / 20.0);        // dB per doubling, 20 .. 300 mmWS
        std::printf ("  MIDI %d, dB re 75 mmWS:", midi);
        for (int i = 0; i < NW; ++i) std::printf ("  %g: %+.1f", winds[i], db[i] - db[i75]);
        std::printf ("   | %.2f dB per doubling (20-300)\n", slope);
        // the jet as it was before the fix (sharp convection time): the same check must catch it
        const double old5 = engineLevelDb (5, midi, 0.0) - engineLevelDb (75, midi, 0.0);
        std::printf ("          without the turbulent jitter (before the fix): 5 mmWS %+.1f dB re 75\n", old5);
        if (! (db[0] < db[i75] - 20.0)) ++bad;                    // 5 mmWS: breath, far below the voiced pipe
        for (int i = 1; i + 1 < NW; ++i) if (! (db[i] < db[i + 1])) ++bad;        // louder with more wind from 10 mmWS up
        if (! (slope > 1.5 && slope < 6.0)) ++bad;                // about +3 dB per doubling (Verge 1997)
        if (! (old5 < -20.0)) ++oldBad;
    }
    check (bad == 0, "low wind (engine, pitch lock 100 %): 5 mmWS > 20 dB below 75, louder with more wind, 1.5..6 dB per doubling");
    std::printf ("  the jet before the fix fails the 5 mmWS limit at %d of 3 keys\n", oldBad);
    check (oldBad > 0, "the low-wind check catches the jet before the fix");
}

void testLab()
{
    std::printf ("\n== 8. Lab (step 5): side hole, physical FM of length and lip, cross drive, nicking, range, matrix, presets ==\n");
    // --- side hole: an open hole raises the resonance (more near the middle of the bore than near the top),
    //     by the interval of the acoustic model; pitch lock removes the jet's pull and keeps that interval
    {
        auto run = [] (double lock, double hole, double& holeR) {
            Engine e;
            e.prepare (48000, 480);
            Globals g;
            g.reverb = 0; g.pitchLock = lock; g.sideHole = hole;
            e.setGlobals (g);
            e.noteOn (60, 60, 100);
            std::vector<float> L (480), R (480), out;
            for (int b = 0; b < 300; ++b) { e.process (L.data(), R.data(), 480); if (b >= 150) out.insert (out.end(), L.begin(), L.end()); }
            holeR = 1;
            for (int i = 0; i < Engine::kMaxVoices; ++i) if (e.voice (i).active) holeR = e.voice (i).derived().holeR;
            return strongestHz (out);
        };
        double r0, rHi, rLo, rL;
        const double f0 = run (0, 0, r0), fHi = run (0, 0.85, rHi), fLo = run (0, 0.35, rLo), fLock = run (1, 0.35, rL);
        const double eHi = cents (fHi, f0) - 1200 * std::log2 (rHi), eLo = cents (fLo, f0) - 1200 * std::log2 (rLo);
        std::printf ("  side hole, c1 (lock 0): closed %.2f Hz; open near the top %.2f Hz (%+.0f c, model %+.0f c); near the middle %.2f Hz (%+.0f c, model %+.0f c)\n",
                     f0, fHi, cents (fHi, f0), 1200 * std::log2 (rHi), fLo, cents (fLo, f0), 1200 * std::log2 (rLo));
        std::printf ("  lock 100 %%, hole near the middle: %.2f Hz = key %+.2f c + hole %+.2f c\n", fLock, cents (fLock, 261.6256 * rL), 1200 * std::log2 (rL));
        check (fHi > f0 * 1.002 && fLo > fHi, "open side hole: pitch up, more near the middle of the bore than near the top");
        check (std::abs (eHi) < 15 && std::abs (eLo) < 15, "the waveguide's hole interval = the two-segment model's (within 15 c)");
        check (std::abs (cents (fLock, 261.6256 * rL)) < 2.0, "pitch lock 100 %: the key plus the hole's interval");
    }    // --- physical FM of the length and of the upper lip: sidebands at f +- f_mod
    {
        Globals g;
        g.fmDepth = 0.3; g.fmRatio = 0.5;
        g.fmTarget = 1; const std::vector<float> len = engineNote (g, 60, 2.5);
        g.fmTarget = 2; g.fmDepth = 0.6; const std::vector<float> lip = engineNote (g, 60, 2.5);
        Globals g0; const std::vector<float> dry = engineNote (g0, 60, 2.5);
        const double sbLen = levelAt (len, 1.5 * 261.63), sbLip = levelAt (lip, 1.5 * 261.63), sb0 = levelAt (dry, 1.5 * 261.63);
        std::printf ("  physical FM at half the key frequency, sideband 1.5 f: length %.1f dB, upper lip %.1f dB, none %.1f dB (re strongest)\n", sbLen, sbLip, sb0);
        check (sbLen > sb0 + 15 && sbLip > sb0 + 15, "length and lip FM make sidebands");
    }
    // --- cross drive: two pipes shake each other's jets (stable, and it changes the sound)
    {
        auto chord = [] (double cross) {
            Engine e;
            e.prepare (48000, 480);
            Globals g;
            g.reverb = 0; g.crossDrive = cross;
            e.setGlobals (g);
            std::vector<float> L (480), R (480), out;
            e.noteOn (60, 60, 100); e.noteOn (64, 64, 100);
            for (int b = 0; b < 300; ++b) { e.process (L.data(), R.data(), 480); if (b >= 100) out.insert (out.end(), L.begin(), L.end()); }
            return out;
        };
        const std::vector<float> a = chord (0.0), b = chord (0.8);
        double d = 0, pk = 0, pkB = 0;
        bool fin = true;
        for (size_t i = 0; i < a.size(); ++i) { d = std::max (d, (double) std::abs (a[i] - b[i])); pk = std::max (pk, (double) std::abs (a[i])); pkB = std::max (pkB, (double) std::abs (b[i])); fin = fin && std::isfinite (b[i]); }
        std::printf ("  cross drive 0.8, c1 + e1: max difference %.2f of peak, peak %.3f vs %.3f\n", d / pk, pkB, pk);
        check (fin && pkB <= 1.0 && d / pk > 0.05, "cross drive changes the chord, stays finite");
    }
    // --- nicking: calmer speech (less noise, a rounder tone)
    {
        Globals g;
        g.noise = 0.03;
        const std::vector<float> a = engineNote (g, 60, 2.5);
        g.nicking = 1.0;
        const std::vector<float> b = engineNote (g, 60, 2.5);
        auto hf = [] (const std::vector<float>& x) {     // energy above 6 kHz re total
            const size_t N = nextPow2 (x.size());
            std::vector<std::complex<double>> s (N);
            for (size_t i = 0; i < x.size(); ++i) s[i] = x[i];
            fft (s);
            double hi = 0, all = 0;
            for (size_t k = 1; k < N / 2; ++k) { const double e = std::norm (s[k]); all += e; if (k * 48000.0 / N > 6000) hi += e; }
            return 10 * std::log10 (hi / all);
        };
        std::printf ("  nicking 0 -> 1 (turbulence 3 %%): energy above 6 kHz %.1f -> %.1f dB re total\n", hf (a), hf (b));
        check (hf (b) < hf (a) - 3.0, "nicking: less noise and high partials");
    }
    // --- beyond the compass: MIDI 0 and 127 are stable pipes
    {
        Globals g;
        const std::vector<float> lo = engineNote (g, 0, 3.0, 1.5), hi = engineNote (g, 127, 2.0);
        double pl = 0, ph = 0;
        bool fin = true;
        for (float v : lo) { pl = std::max (pl, (double) std::abs (v)); fin = fin && std::isfinite (v); }
        for (float v : hi) { ph = std::max (ph, (double) std::abs (v)); fin = fin && std::isfinite (v); }
        std::printf ("  MIDI 0 (%.1f Hz key, a %.0f m pipe): peak %.3f, strongest %.1f Hz; MIDI 127: peak %.4f, strongest %.0f Hz\n",
                     midiToHz (0), designPrinzipal (0).lPhys, pl, strongestHz (lo), ph, strongestHz (hi));
        check (fin && pl <= 1.0 && ph <= 1.0 && pl > 1e-3, "pipes beyond the compass: finite, they sound");
    }
    // --- matrix: LFO 1 -> cut-up moves the mouth; overblow -> stopped closes an overblowing pipe
    {
        Engine e;
        e.prepare (48000, 480);
        Globals g;
        g.reverb = 0; g.modSrc1 = MS_LFO1; g.modDst1 = 2; g.modAmt1 = 0.3; g.lfo1Rate = 2.0;     // target 2 = cut-up
        e.setGlobals (g);
        std::vector<float> L (480), R (480);
        e.noteOn (60, 60, 100);
        double wMin = 1e9, wMax = 0;
        for (int b = 0; b < 150; ++b)
        {
            e.process (L.data(), R.data(), 480);
            for (int i = 0; i < Engine::kMaxVoices; ++i) if (e.voice (i).active) { wMin = std::min (wMin, e.voice (i).derived().W); wMax = std::max (wMax, e.voice (i).derived().W); }
        }
        std::printf ("  matrix LFO 1 (2 Hz) -> cut-up +-0.3: mouth W %.2f .. %.2f mm\n", wMin * 1e3, wMax * 1e3);
        check (wMax > 1.3 * wMin, "matrix: an LFO moves the cut-up");

        Engine e2;
        e2.prepare (48000, 480);
        double v[kMaxParamsCheck];
        int pr = -1;
        for (int i = 0; i < (int) factoryPresets().size(); ++i) if (std::string (factoryPresets()[(size_t) i].name).find ("Overblow") == 0) pr = i;
        presetValues (pr, v);
        Globals g2;
        for (int i = 0; i < kNumParams; ++i) g2.*(kParamDefs[i].field) = v[i];
        g2.reverb = 0;
        e2.setGlobals (g2);
        e2.noteOn (48, 48, 100);       // (below c1, where 480 mmWS overblows)
        double mMax = 0, obMax = 0;
        for (int b = 0; b < 300; ++b)
        {
            e2.process (L.data(), R.data(), 480);
            for (int i = 0; i < Engine::kMaxVoices; ++i) if (e2.voice (i).active) { mMax = std::max (mMax, e2.voice (i).frame()[P_MORPH]); obMax = std::max (obMax, e2.voice (i).modOver); }
        }
        std::printf ("  recipe 'Overblow closes the pipe' (c, Wind pressure 5.00, cut-up x0.65): overblow source up to %.2f, top closed up to %.0f %%\n", obMax, mMax * 100);
        check (obMax > 0.5 && mMax > 0.5, "matrix: the pipe's own overblowing closes its top");
    }
    // --- every recipe plays a short chord, finite and within full scale
    {
        int bad = 0;
        double v[kMaxParamsCheck];
        for (int pi = 0; pi < (int) factoryPresets().size(); ++pi)
        {
            presetValues (pi, v);
            Globals g;
            for (int i = 0; i < kNumParams; ++i) g.*(kParamDefs[i].field) = v[i];
            Engine e;
            e.prepare (48000, 480);
            e.setGlobals (g);
            std::vector<float> L (480), R (480);
            e.noteOn (48, 48, 100); e.noteOn (60, 60, 90); e.noteOn (67, 67, 80);
            double pk = 0;
            for (int b = 0; b < 200; ++b)
            {
                if (b == 120) e.allOff (false);
                e.process (L.data(), R.data(), 480);
                for (int i = 0; i < 480; ++i) { if (! std::isfinite (L[i]) || std::abs (L[i]) > 1.0f) ++bad; pk = std::max (pk, (double) std::abs (L[i])); }
            }
            if (pk < 1e-3) ++bad;
        }
        std::printf ("  %d recipes: chord and release, bad samples or silent recipes %d\n", (int) factoryPresets().size(), bad);
        check (bad == 0, "every recipe sounds, finite and within full scale");
    }
}

/* ================================================================ 9. simd */
/* P5: the AVX2 + FMA kernels against the SSE2 ones (same data), and their speed */
void testSimd()
{
    std::printf ("\n== 9. SIMD: the wall's modal ring and the reverb's spectral multiply-accumulate (this CPU: %s) ==\n", simdLevel());
    if (! simd_detail::cpuHasAvx2Fma()) { std::printf ("  no AVX2 + FMA here: only the baseline (%s) runs\n", simdLevel()); return; }
    std::mt19937 rng (5);
    std::uniform_real_distribution<double> U (-1.0, 1.0);
    // 64 decaying modes (|p| < 1), a 1 s ring at 48 kHz
    constexpr int NA = 64, NS = 48000;
    alignas(32) double zr0[NA], zi0[NA], pr[NA], pim[NA], oa[NA], ob[NA];
    for (int k = 0; k < NA; ++k)
    {
        const double w = 0.002 + 0.9 * (k + 0.5) / NA, rr = std::exp (-1e-4 * (1 + k));
        pr[k] = rr * std::cos (w); pim[k] = rr * std::sin (w);
        zr0[k] = U (rng); zi0[k] = U (rng); oa[k] = U (rng); ob[k] = U (rng);
    }
    std::vector<double> a (NS), b (NS);
    alignas(32) double zra[NA], zia[NA], zrb[NA], zib[NA];
    std::copy (zr0, zr0 + NA, zra); std::copy (zi0, zi0 + NA, zia); std::copy (zr0, zr0 + NA, zrb); std::copy (zi0, zi0 + NA, zib);
    simd_detail::modalRingBase (NS, NA, zra, zia, pr, pim, oa, ob, a.data());
    simd_detail::modalRingAvx2 (NS, NA, zrb, zib, pr, pim, oa, ob, b.data());
    double dMax = 0, pk = 0;
    for (int i = 0; i < NS; ++i) { dMax = std::max (dMax, std::abs (a[(size_t) i] - b[(size_t) i])); pk = std::max (pk, std::abs (a[(size_t) i])); }
    auto timeRing = [&] (bool avx) {
        const auto t0 = std::chrono::steady_clock::now();
        for (int r = 0; r < 20; ++r)
            (avx ? simd_detail::modalRingAvx2 : simd_detail::modalRingBase) (NS, NA, zrb, zib, pr, pim, oa, ob, b.data());
        return std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count() / 20.0;
    };
    const double tS = timeRing (false), tA = timeRing (true);
    std::printf ("  modal ring, 64 modes x 1 s: max difference %.1e of the peak %.2f; SSE2 %.2f ms, AVX2+FMA %.2f ms (x%.2f)\n",
                 dMax / pk, pk, tS * 1e3, tA * 1e3, tS / tA);
    check (dMax < 1e-9 * pk, "AVX2 modal ring = SSE2 (rounding only)");
    // spectral MAC: 2049 bins, one partition into two ears
    constexpr int NB = 2049;
    std::vector<float> x[2], h[4], accA[4], accB[4];
    for (auto& v : x) { v.resize (NB); for (auto& e : v) e = (float) U (rng); }
    for (auto& v : h) { v.resize (NB); for (auto& e : v) e = (float) U (rng); }
    for (int i = 0; i < 4; ++i) { accA[i].assign (NB, 0.f); accB[i].assign (NB, 0.f); }
    for (int r = 0; r < 16; ++r)
    {
        simd_detail::cmacBase (NB, x[0].data(), x[1].data(), h[0].data(), h[1].data(), h[2].data(), h[3].data(), accA[0].data(), accA[1].data(), accA[2].data(), accA[3].data());
        simd_detail::cmacAvx2 (NB, x[0].data(), x[1].data(), h[0].data(), h[1].data(), h[2].data(), h[3].data(), accB[0].data(), accB[1].data(), accB[2].data(), accB[3].data());
    }
    double dm = 0, am = 0;
    for (int i = 0; i < 4; ++i) for (int k = 0; k < NB; ++k) { dm = std::max (dm, (double) std::abs (accA[i][(size_t) k] - accB[i][(size_t) k])); am = std::max (am, (double) std::abs (accA[i][(size_t) k])); }
    auto timeMac = [&] (bool avx) {
        const auto t0 = std::chrono::steady_clock::now();
        for (int r = 0; r < 20000; ++r)
            (avx ? simd_detail::cmacAvx2 : simd_detail::cmacBase) (NB, x[0].data(), x[1].data(), h[0].data(), h[1].data(), h[2].data(), h[3].data(), accB[0].data(), accB[1].data(), accB[2].data(), accB[3].data());
        return std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count() / 20000.0;
    };
    const double mS = timeMac (false), mA = timeMac (true);
    std::printf ("  spectral MAC, 2049 bins x 2 ears: max difference %.1e of %.1f; SSE2 %.2f us, AVX2+FMA %.2f us (x%.2f)\n", dm / am, am, mS * 1e6, mA * 1e6, mS / mA);
    check (dm < 1e-5 * am, "AVX2 spectral MAC = SSE2 (rounding only)");
}

/* ================================================================== 7. room */void testRoom()
{
    std::printf ("\n== 7. room: the church nave (image sources, Eyring tail, ISO 9613 air), partitioned convolution ==\n");
    std::mt19937 rng (11);
    std::normal_distribution<float> nd (0.0f, 1.0f);
    // --- real FFT against the DFT, and round trip
    {
        const int n = 256;
        RealFFT f;
        f.init (n);
        std::vector<float> x (n), re (n / 2 + 1), im (n / 2 + 1), y (n);
        for (auto& v : x) v = nd (rng);
        f.forward (x.data(), re.data(), im.data());
        double err = 0;
        for (int k = 0; k <= n / 2; ++k)
        {
            double sr = 0, si = 0;
            for (int i = 0; i < n; ++i) { sr += x[(size_t) i] * std::cos (2 * kPi * k * i / n); si -= x[(size_t) i] * std::sin (2 * kPi * k * i / n); }
            err = std::max (err, std::hypot (sr - re[(size_t) k], si - im[(size_t) k]));
        }
        f.inverse (re.data(), im.data(), y.data());
        double rt = 0;
        for (int i = 0; i < n; ++i) rt = std::max (rt, (double) std::abs (y[(size_t) i] - x[(size_t) i]));
        std::printf ("  real FFT 256: vs DFT %.1e, round trip %.1e\n", err, rt);
        check (err < 1e-3 && rt < 1e-5, "real FFT");
    }
    // --- the convolver against direct convolution (all four stages, odd block sizes)
    {
        const int L = 45000, nIn = 6000, nOut = 52000;
        std::vector<float> irL ((size_t) L), irR ((size_t) L);
        for (int i = 64; i < L; ++i) { const float e = std::exp (-i / 9000.0f); irL[(size_t) i] = nd (rng) * e; irR[(size_t) i] = nd (rng) * e; }
        Convolver cv;
        cv.prepare (irL, irR);
        std::vector<float> x ((size_t) nOut, 0.0f), yL ((size_t) nOut), yR ((size_t) nOut);
        for (int i = 0; i < nIn; ++i) x[(size_t) i] = nd (rng);
        const int blk[] { 37, 64, 13, 128, 1, 200, 64, 7 };
        for (int i = 0, b = 0; i < nOut; ++b)
        {
            const int m = std::min (blk[b % 8], nOut - i);
            cv.process (x.data() + i, yL.data() + i, yR.data() + i, m);
            i += m;
        }
        double err = 0, ref = 0;
        for (int t = 0; t < nOut; t += 7)
        {
            double sL = 0, sR = 0;
            for (int m = std::max (0, t - L + 1); m <= std::min (t, nIn - 1); ++m) { sL += (double) x[(size_t) m] * irL[(size_t) (t - m)]; sR += (double) x[(size_t) m] * irR[(size_t) (t - m)]; }
            err = std::max (err, std::max (std::abs (sL - yL[(size_t) t]), std::abs (sR - yR[(size_t) t])));
            ref = std::max (ref, std::abs (sL));
        }
        std::printf ("  partitioned convolution vs direct (IR 45000, 4 stages, odd blocks): max error %.1e of peak %.1f\n", err, ref);
        check (err < 1e-4 * ref, "partitioned convolution = direct convolution, no latency");
    }
    // --- the room
    {
        const double fs = 48000;
        RoomInfo info;
        std::vector<float> L, R;
        const auto t0 = std::chrono::steady_clock::now();
        roomImpulseResponse (defaultRoom(), fs, L, R, &info);
        const double tGen = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
        std::printf ("  nave %.0f m3, %.0f m2, mean free path %.1f m; organ -> listener %.1f m; %d image sources; first reflection %.1f ms after the direct sound\n",
                     info.V, info.S, info.meanFreePath, info.directDist, info.images, info.firstReflection * 1e3);
        std::printf ("  reverberant / direct at the listener %+.1f dB (critical distance %.1f m); impulse response %.2f s, computed in %.0f ms\n",
                     info.drRatioDb, info.critDist, L.size() / fs, tGen * 1e3);
        std::printf ("  band        63    125    250    500     1k     2k     4k     8k\n  air dB/km");
        for (double a : info.airDbPerKm) std::printf (" %6.2f", a);
        std::printf ("\n  T60 Eyring");
        for (double tt : info.t60) std::printf (" %6.2f", tt);
        // measured: octave-band Schroeder decay of the computed impulse response (T20: -5 .. -25 dB)
        const size_t N = nextPow2 (L.size());
        std::vector<std::complex<double>> spec (N);
        for (size_t i = 0; i < L.size(); ++i) spec[i] = L[i];
        fft (spec);
        std::printf ("\n  T60 IR    ");
        double worst = 0;
        for (int b = 0; b < RoomSpec::kBands; ++b)
        {
            std::vector<std::complex<double>> s2 (N);
            for (size_t k = 0; k < N; ++k)
            {
                const double f = (k <= N / 2 ? k : N - k) * fs / N;
                const double x = std::log2 (std::max (1.0, f) / kBandHz[b]);
                const double w = std::abs (x) < 0.5 ? 1.0 : (std::abs (x) < 0.75 ? 0.5 * (1 + std::cos (kPi * (std::abs (x) - 0.5) / 0.25)) : 0.0);
                s2[k] = std::conj (spec[k] * w);
            }
            fft (s2);                                         // conj-FFT-conj = inverse (unscaled)
            std::vector<double> e (L.size());
            for (size_t i = 0; i < L.size(); ++i) e[i] = std::norm (s2[i]);
            double acc = 0;
            for (size_t i = e.size(); i-- > 0;) { acc += e[i]; e[i] = acc; }
            const double e0 = e[0];
            auto tAt = [&] (double db) { for (size_t i = 0; i < e.size(); ++i) if (10 * std::log10 (e[i] / e0) <= db) return i / fs; return e.size() / fs; };
            const double t60 = 3.0 * (tAt (-25.0) - tAt (-5.0));
            std::printf (" %6.2f", t60);
            if (b >= 1 && b <= 6) worst = std::max (worst, std::abs (t60 / info.t60[b] - 1.0));     // (8 kHz: air absorption changes ~4x across the octave)
        }
        std::printf ("\n");
        check (worst < 0.12, "the impulse response decays with the room's Eyring time in every octave (125 Hz .. 4 kHz)");
        check (info.airDbPerKm[4] > 3.5 && info.airDbPerKm[4] < 6.0 && info.airDbPerKm[6] > 18 && info.airDbPerKm[6] < 40, "air absorption (ISO 9613-1, 20 C, 50 %)");
        // the two microphones in the diffuse tail: coherence at 250 Hz and 2 kHz vs sin(kd)/(kd)
        double cohw[2] = { 0, 0 };
        for (int bi = 0; bi < 2; ++bi)
        {
            const double fc = bi == 0 ? 250.0 : 2000.0;
            const size_t a0 = (size_t) (0.3 * fs), a1 = std::min (L.size(), (size_t) (2.0 * fs));
            const size_t M = 4096;
            std::complex<double> sxy = 0; double sxx = 0, syy = 0;
            for (size_t s0 = a0; s0 + M < a1; s0 += M / 2)
            {
                std::vector<std::complex<double>> a (M), b (M);
                for (size_t i = 0; i < M; ++i) { const double w = 0.5 - 0.5 * std::cos (2 * kPi * i / (M - 1)); a[i] = L[s0 + i] * w; b[i] = R[s0 + i] * w; }
                fft (a); fft (b);
                const size_t kc = (size_t) std::lround (fc * M / fs);
                for (size_t k = kc - 4; k <= kc + 4; ++k) { sxy += a[k] * std::conj (b[k]); sxx += std::norm (a[k]); syy += std::norm (b[k]); }
            }
            cohw[bi] = std::real (sxy) / std::sqrt (sxx * syy);
        }
        const double kd1 = 2 * kPi * 250 * 0.6 / 343.2, kd2 = 2 * kPi * 2000 * 0.6 / 343.2;
        std::printf ("  microphones 0.6 m apart, tail coherence: 250 Hz %.2f (theory %.2f), 2 kHz %.2f (theory %.2f)\n", cohw[0], std::sin (kd1) / kd1, cohw[1], std::sin (kd2) / kd2);
        check (std::abs (cohw[0] - std::sin (kd1) / kd1) < 0.2 && std::abs (cohw[1]) < 0.2, "diffuse-field coherence of the two microphones");
        bool silentBefore = true;
        const int firstN = (int) (info.firstReflection * fs) - 33;
        for (int i = 0; i < firstN; ++i) if (std::abs (L[(size_t) i]) > 1e-6f || std::abs (R[(size_t) i]) > 1e-6f) silentBefore = false;
        check (silentBefore && firstN > 64, "nothing before the first reflection (and the convolution can start one block late)");
    }
    // --- CPU of the room at 48 kHz
    {
        RoomReverb rv;
        rv.prepare (48000);
        std::vector<double> inL (64), inR (64), oL (64), oR (64);
        const int blocks = 48000 * 5 / 64;
        const auto t0 = std::chrono::steady_clock::now();
        double acc = 0;
        for (int b = 0; b < blocks; ++b)
        {
            for (int i = 0; i < 64; ++i) inL[(size_t) i] = inR[(size_t) i] = nd (rng);
            rv.process (inL.data(), inR.data(), oL.data(), oR.data(), 64);
            acc += oL[0];
        }
        const double sec = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
        std::printf ("  room reverb at 48 kHz, 64-sample blocks: %.1f %% of one core   [%g]\n", sec / 5.0 * 100.0, acc * 0);
    }
}

/* ============================================================== 2d. tone (v1.0) */
/* The Principal's tone as heard (the engine at 48 kHz, velocity 100, dry, each channel, steady part 1.5 .. 2.9 s), against
   what measured principals show (Pykett): the octave present (2nd partial at most 26 dB under the fundamental), the partials
   above the 5th falling away (6th .. 12th together at most -24 dB: no flat plateau, the buzz of a jet that switches like a
   valve), the breath well under the tone (partials 32 dB over the noise between them), steady (level sd < 0.1 dB).
   The v0.6.3 pipe (Phase 1's jet, the two ends mixed without their path difference, voiced for 75 mmWS) must fail it. */
struct ToneCheck { double h2 = 0, up = 0, hnr = 0, sd = 0; };
ToneCheck toneCheckOf (const std::vector<float>& x, double fs, double f0)
{
    ToneCheck t;
    const size_t i0 = (size_t) (1.5 * fs), i1 = (size_t) (2.9 * fs), n = i1 - i0, N = nextPow2 (n);
    std::vector<std::complex<double>> a (N);
    for (size_t i = 0; i < n; ++i)
    {
        const double u = 2 * kPi * (double) i / (double) (n - 1);
        a[i] = x[i0 + i] * (0.35875 - 0.48829 * std::cos (u) + 0.14128 * std::cos (2 * u) - 0.01168 * std::cos (3 * u));
    }
    fft (a);
    const double df = fs / (double) N, w = std::max (5.0 * df, std::min (0.1 * f0, 40.0));
    double hp[13] {}, pH = 0, pN = 0;
    for (size_t k = 1; k < N / 2; ++k)
    {
        const double fk = (double) k * df;
        if (fk < 40.0 || fk > 16000.0) continue;
        const int kh = (int) std::lround (fk / f0);
        const double e = std::norm (a[k]);
        if (kh >= 1 && std::abs (fk - kh * f0) <= w) { pH += e; if (kh <= 12) hp[kh] += e; }
        else pN += e;
    }
    t.h2 = 10 * std::log10 (hp[2] / hp[1] + 1e-30);
    double up = 0;
    for (int k = 6; k <= 12; ++k) up += hp[k];
    t.up = 10 * std::log10 (up / hp[1] + 1e-30);
    t.hnr = 10 * std::log10 (pH / (pN + 1e-300));
    // level steadiness over frames of whole periods (>= 40 ms)
    const size_t per = (size_t) std::lround (fs / f0), fl = per * (size_t) std::ceil (0.04 * f0);
    std::vector<double> lv;
    for (size_t s = i0; s + fl <= i1; s += fl)
    {
        double q = 0;
        for (size_t i = s; i < s + fl; ++i) q += (double) x[i] * x[i];
        lv.push_back (10 * std::log10 (q / (double) fl + 1e-30));
    }
    double mu = 0, var = 0;
    for (double v : lv) mu += v;
    mu /= (double) lv.size();
    for (double v : lv) var += (v - mu) * (v - mu);
    t.sd = std::sqrt (var / (double) lv.size());
    return t;
}

void toneNote (int midi, bool v063, std::vector<float>& L, std::vector<float>& R)
{
    Engine e;
    if (v063) { e.jetSpread = e.jetSat = e.jetLipVoice = 0.0; e.topPath = false; e.voicingPa = 75.0 * MMWS; }
    e.prepare (48000, 480);
    Globals g;
    g.reverb = 0;
    e.setGlobals (g);
    std::vector<float> l (480), r (480);
    L.clear(); R.clear();
    e.noteOn (midi, midi, 100);
    for (int b = 0; b < 300; ++b)
    {
        e.process (l.data(), r.data(), 480);
        L.insert (L.end(), l.begin(), l.end());
        R.insert (R.end(), r.begin(), r.end());
    }
}

void testTone()
{
    std::printf ("\n== 2d. tone: the default sound is a Principal's (every 3rd key, both channels, dry) ==\n");
    std::printf ("  key  ch |   2nd  6..12   HNR  lvl sd |  v0.6.3:  2nd  6..12   HNR\n");
    int bad = 0, badOld = 0, keysOld = 0;
    for (int midi = 36; midi <= 96; midi += 3)
    {
        std::vector<float> L, R, oL, oR;
        toneNote (midi, false, L, R);
        toneNote (midi, true, oL, oR);
        bool oldFails = false;
        for (int ch = 0; ch < 2; ++ch)
        {
            const double f0 = midiToHz (midi);
            const ToneCheck t = toneCheckOf (ch ? R : L, 48000, f0), o = toneCheckOf (ch ? oR : oL, 48000, f0);
            auto ok = [] (const ToneCheck& c) { return c.h2 >= -26.0 && c.up <= -24.0 && c.hnr >= 32.0 && c.sd < 0.1; };
            if (! ok (t)) ++bad;
            if (! ok (o)) { ++badOld; oldFails = true; }
            char nm[8];
            organName (midi, nm);
            std::printf ("  %-4s %s | %5.1f %6.1f %5.1f  %5.3f%s | %12.1f %6.1f %5.1f%s\n", nm, ch ? "R" : "L", t.h2, t.up, t.hnr, t.sd, ok (t) ? "  " : " !",
                         o.h2, o.up, o.hnr, ok (o) ? "" : "  (fails)");
        }
        if (oldFails) ++keysOld;
    }
    check (bad == 0, "the default sound is a Principal's at every key: octave present, upper partials falling, breath under the tone, steady");
    std::printf ("  the v0.6.3 pipe fails at %d of 21 keys (%d channel checks)\n", keysOld, badOld);
    check (keysOld >= 15, "the check catches the v0.6.3 pipe (weak octave, flat upper partials, rasp)");
}

/* the host's block size: a control frame (CTRL internal samples) may span two renders. Until v1.0 the jet jitter's low-pass
   and the side hole's junction constants restarted at every render, so blocks that are not a multiple of 16 (SAVIHost's 1764,
   441 ...) sounded rough and off pitch. The sound must not depend on the block size at all. */
void testBlocks()
{
    std::printf ("\n== 2e. host block size: the same note in blocks of any size (default knobs, held 3 s) ==\n");
    for (const double fs : { 44100.0, 48000.0 })
        for (const double hole : { 0.0, 0.5 })
        {
            auto render = [&] (int bs) {
                auto e = std::make_unique<Engine>();
                e->prepare (fs, bs);
                Globals g;
                g.sideHole = hole;
                e->setGlobals (g);
                e->noteOn (60, 60, 100);
                const int n = (int) (3.0 * fs);
                std::vector<float> L ((size_t) n), R ((size_t) n);
                for (int s = 0; s < n; s += bs) e->process (L.data() + s, R.data() + s, std::min (bs, n - s));
                L.insert (L.end(), R.begin(), R.end());
                return L;
            };
            const auto ref = render (16);
            std::printf ("  %.1f kHz, side hole %s:", fs / 1000, hole > 0 ? "open  " : "closed");
            bool same = true;
            for (const int bs : { 37, 100, 441, 480, 1764 })
            {
                const auto x = render (bs);
                double d = 0;
                for (size_t i = 0; i < x.size(); ++i) d = std::max (d, (double) std::abs (x[i] - ref[i]));
                std::printf ("  %d %s", bs, d == 0.0 ? "same" : "DIFFERS");
                if (d != 0.0) same = false;
            }
            std::printf ("\n");
            char nm[96];
            std::snprintf (nm, sizeof nm, "%.1f kHz, side hole %s: blocks of 37 .. 1764 sound exactly as blocks of 16", fs / 1000, hole > 0 ? "open" : "closed");
            check (same, nm);
        }
}
} // namespace

int main (int argc, char** argv)
{
    const std::string what = argc > 1 ? argv[1] : "all";
    std::printf ("OkumuLab 1 — labium_check (%s)\n", what.c_str());
    if (what == "diag")
    {
        for (int i = 2; i < argc; ++i) diagState (argv[i]);
        return 0;
    }
    if (what == "bench")
    {
        {
            const int N = 2000000;
            volatile double sink = 0;
            double x = 0.3, acc = 0;
            auto tm = [&] (const char* nm, auto fn)
            {
                acc = 0; x = 0.3;
                const auto t0 = std::chrono::steady_clock::now();
                for (int i = 0; i < N; ++i) { acc += fn (x); x += 1e-7; }
                sink = acc;
                std::printf ("  %-6s %6.1f ns\n", nm, std::chrono::duration<double, std::nano> (std::chrono::steady_clock::now() - t0).count() / N);
            };
            tm ("exp", [] (double v) { return std::exp (-v); });
            tm ("pow", [] (double v) { return std::pow (v + 1.0, -0.6667); });
            tm ("cos", [] (double v) { return std::cos (v); });
            tm ("atan2", [] (double v) { return std::atan2 (v, 1.0 - v); });
            tm ("sqrt", [] (double v) { return std::sqrt (v); });
            tm ("tan", [] (double v) { return std::tan (v); });
            tm ("log2", [] (double v) { return std::log2 (v + 1.0); });
            tm ("cbrt", [] (double v) { return std::cbrt (v); });
        }
        {
            Globals G0;
            ParamVec p0;
            const PipeDesign pd0 = designPrinzipal (60);
            voiceParams (60, G0, VoiceOverride {}, pd0, p0);
            Derived dv;
            const int N = 200000;
            auto t0 = std::chrono::steady_clock::now();
            double acc = 0;
            for (int i = 0; i < N; ++i) { p0[P_CUTUP] *= (i & 1) ? 1.0000001 : 0.9999999; derive (p0.data(), 96000, dv); acc += dv.dline; }
            const double nsD = std::chrono::duration<double, std::nano> (std::chrono::steady_clock::now() - t0).count() / N;
            t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < N; ++i) { G0.cutup = 1.0 + 1e-6 * (i & 7); voiceParams (60, G0, VoiceOverride {}, pd0, p0); acc += p0[P_CUTUP]; }
            const double nsV = std::chrono::duration<double, std::nano> (std::chrono::steady_clock::now() - t0).count() / N;
            std::printf ("derive(): %.0f ns (%.1f ns per internal sample when it runs every frame); voiceParams(): %.0f ns   [%g]\n", nsD, nsD / CTRL, nsV, acc * 0);
        }
        // one voice, rendered directly (no engine): ns per internal sample
        Voice v;
        v.prepare (96000, 3);
        Globals G;
        ParamVec p;
        voiceParams (60, G, VoiceOverride {}, designPrinzipal (60), p);
        v.start (p, 0.0);
        std::vector<double> L (512), R (512);
        for (int i = 0; i < 200; ++i) v.render (512, L.data(), R.data(), nullptr);
        const int blocks = 4000;
        for (int pass = 0; pass < 3; ++pass)
        {
            if (pass == 1) fpenv::set (fpenv::flushing (fpenv::get()));     // denormals flushed to zero, as the plugin's audio runs
            if (pass == 2) engineJet (v);                         // the jet as the engine runs it (P5 jitter, v1.0)
            const auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < blocks; ++i) v.render (512, L.data(), R.data(), nullptr);
            const double ns = std::chrono::duration<double, std::nano> (std::chrono::steady_clock::now() - t0).count() / (blocks * 512.0);
            std::printf ("bench %s: %.1f ns per internal sample (%.2f %% of a core per voice at 96 kHz)\n",
                         pass == 0 ? "default" : (pass == 1 ? "FTZ/DAZ" : "FTZ/DAZ + jet jitter"), ns, ns * 96000 * 1e-9 * 100);
        }
        return 0;
    }
    if (what == "obdiag")
    {
        // the regime at Wind pressure 5.00 (2500 Pa) for some cut-ups (pitch lock off, 48 kHz engine)
        for (double cut : { 1.0, 0.85, 0.75, 0.65 })
            for (int midi : { 36, 48, 55, 60 })
            {
                Engine e;
                e.prepare (48000, 480);
                Globals g;
                g.reverb = 0; g.bellows = 5.0; g.pitchLock = 0; g.cutup = cut;
                e.setGlobals (g);
                e.noteOn (midi, midi, 100);
                std::vector<float> L (480), R (480);
                for (int b = 0; b < 250; ++b) e.process (L.data(), R.data(), 480);
                for (int i = 0; i < Engine::kMaxVoices; ++i)
                    if (e.voice (i).active)
                        std::printf ("  cut-up x%.2f midi %d: regime %.1f (%.1f Hz)\n", cut, midi, e.voice (i).measuredRatio, e.voice (i).measuredHz);
            }
        return 0;
    }
    if (what == "lockdiag")
    {
        // the stopped state of the pitch-lock check, per key: the strongest partial over the last 0.6 s and 1.6 s
        for (int k : { 36, 48, 60, 72, 84 })
        {
            Globals G;
            G.morph = 1;
            ParamVec p;
            voiceParams (k, G, VoiceOverride {}, designPrinzipal (k), p);
            const auto m = renderCpp (p, 2.6, FS, true, -1, nullptr, 1200.0 * std::log2 (kcalFor (k)), Engine::kJetJitter);
            for (double win : { 0.6, 1.6 })
            {
                const size_t i0 = (size_t) ((2.6 - win) * FS);
                double pf[1], pdb[1];
                spectralPeaks (m.data() + i0, m.size() - i0, FS, 1, pf, pdb);
                const double r = pf[0] / p[P_FTARGET], cand = 0.5 * std::clamp ((int) std::lround (2.0 * r), 1, 24);
                std::printf ("  key %d window %.1f s: %.3f Hz, x%.1f, %+.2f c\n", k, win, pf[0], cand, cents (r, cand));
            }
            if (k == 36)
            {
                // what the servo sees: a voice rendered the same way, its measurements every 0.2 s
                Voice v;
                engineJet (v);
                v.prepare (FS, 3);
                v.start (p, 1200.0 * std::log2 (kcalFor (k)));
                std::vector<double> L (19200), R (19200), mo (19200);
                for (int b = 0; b < 13; ++b)
                {
                    v.render (19200, L.data(), R.data(), mo.data());
                    std::printf ("    %.1f s: measured %.3f Hz ratio %.1f locked %d cents %+.2f servo %+.2f regime %.1f\n", 0.2 * (b + 1), v.measuredHz, v.measuredRatio,
                                 (int) v.measuredLocked, v.measuredCents, v.servoCents(), v.servoRegime());
                }
            }
        }
        return 0;
    }
    if (what == "holediag")
    {
        // the side hole after release (stress trial 42's settings): energy 15-16 s vs 25-26 s and the strongest partials
        auto run = [] (double hole, double cMult, double loss, int midi, double fsHost) {
            Engine e;
            e.prepare (fsHost, 480);
            Globals g;
            g.wind = 115; g.loss = loss; g.jetGain = 3.0; g.cMult = cMult; g.fmDepth = 0.2; g.fmRatio = 0.1; g.sideHole = hole;
            g.crossDrive = 1.0; g.nicking = 0; g.reverb = 0;
            e.setGlobals (g);
            e.noteOn (0, midi, 100);
            std::vector<float> L (480), R (480), tail;
            const int bps = (int) (fsHost / 480);
            double e15 = 0, e25 = 0;
            for (int b = 0; b < 26 * bps; ++b)
            {
                if (b == (int) (1.2 * bps)) e.allOff (false);
                e.process (L.data(), R.data(), 480);
                for (int i = 0; i < 480; ++i)
                {
                    const double v = L[i];
                    if (b >= 15 * bps && b < 16 * bps) e15 += v * v;
                    if (b >= 25 * bps) { e25 += v * v; tail.push_back (L[i]); }
                }
            }
            std::vector<double> d (tail.begin(), tail.end());
            double pf[3] { 0, 0, 0 }, pdb[3] { 0, 0, 0 };
            if (d.size() > 4096) spectralPeaks (d.data(), d.size(), fsHost, 3, pf, pdb);
            std::printf ("  hole %.2f cMult %.2f loss %.2f midi %3d fs %.0f: e25/e15 %.3f, tail %.1f dBFS, peaks %.0f %.0f %.0f Hz\n", hole, cMult, loss, midi, fsHost,
                         e25 / std::max (1e-300, e15), 10 * std::log10 (e25 / std::max<size_t> (1, tail.size()) + 1e-30), pf[0], pf[1], pf[2]);
        };
        // stress trial 171: a 3 Hz LFO on the length glide (+-1.9 st) with the hole open; energy per 3 s after release
        for (int mode = 0; mode < 3; ++mode)
        {
            Engine e;
            e.prepare (44100, 480);
            Globals g;
            g.wind = 465; g.loss = 0.71; g.cMult = 0.73; g.rhoMult = 0.2; g.reverb = 0;
            g.sideHole = mode == 1 ? 0.0 : 0.92;
            if (mode != 2) { g.modSrc1 = MS_LFO2; g.modDst1 = 12; g.modAmt1 = -0.08; g.lfo2Rate = 3.0; }
            e.setGlobals (g);
            e.noteOn (0, 16, 100);
            std::vector<float> L (480), R (480);
            const int bps = 44100 / 480;
            std::printf ("  trial-171 setting, %s:", mode == 0 ? "hole + LFO glide" : (mode == 1 ? "LFO glide, no hole" : "hole, no LFO"));
            double en = 0;
            for (int b = 0; b < 21 * bps; ++b)
            {
                if (b == (int) (1.2 * bps)) e.allOff (false);
                e.process (L.data(), R.data(), 480);
                for (int i = 0; i < 480; ++i) en += (double) L[i] * L[i];
                if (b % (3 * bps) == 3 * bps - 1) { std::printf (" %.0f", 10 * std::log10 (en / (3.0 * bps * 480) + 1e-30)); en = 0; }
            }
            std::printf (" dBFS (3 s steps)\n");
        }
        for (int midi : { 10, 31, 55 })
            for (double hole : { 0.0, 0.62 })
                run (hole, 2.0, 0.43, midi, 48000);
        run (0.62, 1.0, 0.43, 31, 48000);
        run (0.62, 2.0, 2.5, 31, 48000);
        run (0.62, 2.0, 0.43, 31, 96000);
        run (0.2, 2.0, 0.43, 31, 48000);
        run (0.95, 2.0, 0.43, 31, 48000);
        return 0;
    }
    if (what == "stopdiag")
    {
        // a stopped pipe (morph 1): the strongest partials per key, with and without the jet's jitter
        const int variant = argc > 2 ? std::atoi (argv[2]) : 0;
        for (double jit : { Engine::kJetJitter, 0.0 })
            for (int k : { 36, 48, 60, 72, 84 })
            {
                Globals G;
                G.morph = 1;
                if (variant == 2) G.noise = 0;
                if (variant == 3) G.kick = 0;
                if (variant == 4) G.pitchLock = 0;
                ParamVec p;
                voiceParams (k, G, VoiceOverride {}, designPrinzipal (k), p);
                const auto m = renderCpp (p, 2.6, FS, variant != 1, -1, nullptr, 1200.0 * std::log2 (kcalFor (k)), jit);
                const size_t i0 = (size_t) (2.0 * FS);
                double pf[4], pdb[4];
                spectralPeaks (m.data() + i0, m.size() - i0, FS, 4, pf, pdb);
                double s = 0;
                for (size_t i = i0; i < m.size(); ++i) s += m[i] * m[i];
                std::printf ("jitter %.2f key %d (f %.1f): level %.1f dB; peaks", jit, k, p[P_FTARGET], 10 * std::log10 (s / (m.size() - i0) / 4e-10));
                for (int i = 0; i < 4; ++i) std::printf ("  %.1f Hz (x%.2f, %.0f dB)", pf[i], pf[i] / p[P_FTARGET], pdb[i]);
                std::printf ("\n");
            }
        return 0;
    }
    if (what == "windsweep")
    {
        // level, pitch and regime of a steady note against the chest pressure (diagnostic)
        const int keys[] { 48, 60, 72 };
        const double jit = argc > 2 ? std::atof (argv[2]) : 0.0;
        const int lockFrom = argc > 3 ? std::atoi (argv[3]) : 1;
        std::printf ("  jet jitter %.2f\n", jit);
        for (int lock = lockFrom; lock >= 0; --lock)
            for (int midi : keys)
            {
                std::printf ("\n  MIDI %d, pitch lock %d %%:\n   wind  p_foot   U_j    I    level   f/key  cents  regime  h2-h1  noise-floor\n", midi, lock * 100);
                for (double wind : { 5.0, 10.0, 15.0, 20.0, 30.0, 40.0, 50.0, 60.0, 75.0, 100.0, 150.0, 200.0, 300.0, 500.0, 1000.0 })
                {
                    Globals G;
                    G.wind = wind;
                    G.pitchLock = lock;
                    ParamVec p;
                    voiceParams (midi, G, VoiceOverride {}, designPrinzipal (midi), p);
                    Voice v;
                    if (jit > 0.0) engineJet (v);
                    v.jetJitter = jit;
                    v.prepare (FS, 3);
                    v.start (p, 1200.0 * std::log2 (kcalFor (midi)));
                    const int n = (int) (2.5 * FS);
                    std::vector<double> mouth ((size_t) n), L ((size_t) n), R ((size_t) n);
                    for (int i = 0; i < n; i += 4096) v.render (std::min (4096, n - i), L.data() + i, R.data() + i, mouth.data() + i);
                    const Metrics m = analyze (mouth, FS, p[P_FTARGET], 1.5);
                    // noise share: energy outside +-3 % of the harmonics vs total
                    double fr[1], db[1];
                    spectralPeaks (mouth.data() + (size_t) (1.5 * FS), (size_t) FS, FS, 1, fr, db);
                    const Derived& d = v.derived();
                    const double I = std::sqrt (2.0 * std::max (0.0, v.pf) * p[P_FLUE] / (p[P_RHO] * d.W * d.W * d.W)) / d.fres;
                    std::printf ("  %5.0f  %6.1f  %5.1f  %4.2f  %5.1f dB  %5.3f  %+6.1f  %4.2f   %+5.1f  peak %.0f Hz\n", wind, v.pf / MMWS, v.Uj, I, m.db,
                                 m.f / p[P_FTARGET], cents (m.f, p[P_FTARGET] * std::max (0.5, std::round (2.0 * m.f / p[P_FTARGET]) / 2.0)), m.regime,
                                 m.hm[1], fr[0]);
                }
            }
        return 0;
    }
    if (what == "malletbench")
    {
        volatile double sink = 0;
        auto tm = [&] (const char* nm, int N, auto fn)
        {
            const auto t0 = std::chrono::steady_clock::now();
            double acc = 0;
            for (int i = 0; i < N; ++i) acc += fn (i);
            sink = acc;
            std::printf ("  %-26s %8.2f us\n", nm, std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now() - t0).count() / N);
        };
        tm ("bessel J(5, x)", 20000, [] (int i) { return okl::bessel::J (5, 0.5 + 1e-4 * i); });
        tm ("bessel Y(5, x)", 20000, [] (int i) { return okl::bessel::Y (5, 0.5 + 1e-4 * i); });
        tm ("bessel K(5, x)", 20000, [] (int i) { return okl::bessel::K (5, 0.5 + 1e-4 * i); });
        tm ("bessel I(5, x)", 20000, [] (int i) { return okl::bessel::I (5, 0.5 + 1e-4 * i); });
        tm ("shell eigen (Flügge 3x3)", 20000, [] (int i) { return okl::malletTestShellDelta (0.05 + 1e-6 * i, 3, 0.36, 6e-5, 0); });
        Globals G;
        G.excite = 1;
        for (int midi : { 36, 60, 84 })
        {
            const StrikeParams s = pipeOf (midi, G);
            char nm[64];
            std::snprintf (nm, sizeof nm, "design, MIDI %d", midi);
            tm (nm, 20, [&] (int) { MalletModel m; m.prepare (48000); m.setParams (s); m.strike (1.0); return (double) m.modeCount(); });
            std::snprintf (nm, sizeof nm, "re-strike, MIDI %d", midi);
            MalletModel m; m.prepare (48000); m.setParams (s); m.strike (1.0);
            tm (nm, 200, [&] (int) { m.strike (1.0); return (double) m.modeCount(); });
        }
        (void) sink;
        return 0;
    }
    if (what == "trace" && argc > 3)
    {
        traceState (argv[2], std::atoi (argv[3]));
        return 0;
    }
    if (what == "port" || what == "all") testPort();
    if (what == "derive" || what == "all") testDerive();
    if (what == "midi" || what == "all") testMidi();
    if (what == "pitchlock" || what == "all") testPitchLock();
    if (what == "wind" || what == "all") { testWind(); testWindEngine(); }
    if (what == "tone" || what == "all") testTone();
    if (what == "blocks" || what == "all") testBlocks();
    if (what == "cpu" || what == "all") testCpu();
    if (what == "stress" && argc > 2) stressVariant = std::atoi (argv[2]);
    if (what == "stress" && argc > 3) stressOnly = std::atoi (argv[3]);
    if (what == "stress" || what == "all") testStress();
    if (what == "mallet" || what == "all") testMallet();
    if (what == "room" || what == "all") testRoom();
    if (what == "lab" || what == "all") testLab();
    if (what == "simd" || what == "all") testSimd();
    std::printf ("\n%s (%d failed checks)\n", g_fail ? "FAILED" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
