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
 *  10. tone      : the losses (radiation stage vs the physics) and the Prinzipal's partials across the compass
 *
 *   labium_check [port|derive|midi|pitchlock|wind|blocks|tone|cpu|stress|mallet|room|lab|simd|all]
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
#include <memory>
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
    // Phase 1 base_params(design_prinzipal(midi), k_cal) through the plugin's own path
    const PipeDesign pd = designPrinzipal (midi);
    ParamVec v;
    Globals G;
    G.wind = 75.0;               // Phase 1: 75 mmWS in the windchest (the plugin's reference is 500 Pa since 0.6.1)
    G.pitchLock = 1.0;           // Phase 1 base_params: pitch lock on (the plugin's default is off since v1.1)
    voiceParams (midi, G, VoiceOverride {}, pd, v);
    // undo the knob defaults that differ from Phase 1 base_params: voicing follow 0, pallet 12 ms
    v[P_VFOLLOW] = 0.0;
    v[P_PALLET_TAU] = 0.012;
    return v;
}

/* jitter: P5 jet convection jitter (0 = Phase 1, for the port checks; the plugin's engine uses Engine::kJetJitter);
   phase1: the Phase 1 jet and losses instead of v1.1's (the port checks against the Phase 1 C core) */
std::vector<double> renderCpp (const ParamVec& p, double dur, double fs, bool servo, double gateOff = -1, int* resets = nullptr, double servoStart = 0.0,
                               double jitter = 0.0, bool phase1 = false)
{
    const int n = (int) std::lround (dur * fs);
    Voice v;
    v.jetJitter = jitter;
    v.phase1 = phase1;
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
    std::printf ("\n== 1. port: C++ voice vs Phase 1 C core and Phase 1 reference (96 kHz, servo off; the C++ voice with its Phase 1 jet and losses) ==\n");
    std::printf ("%-22s %10s %10s %10s %8s %8s %7s %7s %7s %6s %6s\n", "state", "ref f", "C f", "C++ f", "C++-ref", "C++-C", "ref dB", "C++ dB", "d dB", "hm2-6", "reset");
    double worstC = 0, worstRef = 0, worstDb = 0, worstHm = 0, seedVar = 0, cBase = 0;
    auto row = [&] (const char* name, int midi, const ParamVec& p, double dur, double steady, double gateOff,
                    double rf, double rreg, double rdb, const double* rhm)
    {
        const double f0 = midiToHz (midi);
        int rc = 0, rcpp = 0;
        const Metrics mc = analyze (renderC (p, dur, gateOff, &rc), FS, f0, steady);
        const Metrics mp = analyze (renderCpp (p, dur, FS, false, gateOff, &rcpp, 0.0, 0.0, true), FS, f0, steady);
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
        const Metrics mp = analyze (renderCpp (p, dur, FS, false, -1, nullptr, 0.0, 0.0, true), FS, f0, st);
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
    // (v1.1: b is the jet's half-width where it meets the labium, 1.3 to 11 times the flue's; beyond about 1.2 b the jet no
    //  longer crosses the edge and the pipe is silent, so the states move by 1 b, as 1.5 flue half-widths did before)
    { "labium -1.0b", [] (Globals& g) { g.y0b = -1.0; } },
    { "labium +1.0b", [] (Globals& g) { g.y0b = 1.0; } },
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
            G.pitchLock = 1.0;       // (off by default since v1.1)
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
            Globals G;
            G.wind = 75.0;           // the voicing pressure (75 mmWS = Wind pressure 1.47): the states are the Phase 3 ones
            G.pitchLock = 1.0;
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
        { "wind 75 -> 300", [] (Globals& g, double x) { g.wind = 75 * std::pow (4.0, x); } },
        { "cut-up x1 -> x1.6", [] (Globals& g, double x) { g.cutup = 1 + 0.6 * x; } },
        { "labium 0.5 -> -1.0 b", [] (Globals& g, double x) { g.y0b = 0.5 - 1.5 * x; } },
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
            v.jetJitter = Engine::kJetJitter;
            v.prepare (fs, 3);
            Globals G;
            G.wind = 75.0;           // (the voicing pressure, as the states above)
            G.pitchLock = 1.0;
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
                g.wind = 75.0;
                g.pitchLock = 1.0;
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
                g.wind = 75.0;
                g.pitchLock = 1.0;
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
Globals stopGlobals (int stop);
/* the engine's load (% of one core) with n voices sounding on these knobs, over 8 s (a cut-up wobble if moving) */
double cpuLoad (Globals G, int voicesN, bool moving, double seconds = 8.0, double jitter = Engine::kJetJitter, int key = -1)
{
    Engine e;
    e.jetJitter = jitter;
    e.prepare (48000, 512);
    e.setGlobals (G);
    for (int i = 0; i < voicesN; ++i) e.noteOn (i, key >= 0 ? key : 36 + (i * 7) % 60, 100);
    std::vector<float> L (512), R (512);
    // settle
    for (int b = 0; b < 100; ++b) e.process (L.data(), R.data(), 512);
    const int blocks = (int) (seconds * 48000 / 512);
    const double cut0 = G.cutup;
    const auto t0 = std::chrono::steady_clock::now();
    for (int b = 0; b < blocks; ++b)
    {
        if (moving)
        {
            // a knob moving continuously (cut-up wobble) keeps derive() busy in every voice
            G.cutup = cut0 + 0.2 * std::sin (b * 0.05);
            e.setGlobals (G);
        }
        e.process (L.data(), R.data(), 512);
    }
    const double sec = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
    return sec / (blocks * 512 / 48000.0) * 100.0;
}

void testCpu()
{
    std::printf ("\n== 3. CPU: engine at 48 kHz host (96 kHz internal), 512-sample blocks, one core ==\n");
    const int nv[] { 1, 8, 16, 32 };
    for (int stop : { 0, 1 })
        for (int voicesN : nv)
            for (bool moving : { false, true })
            {
                const double load = cpuLoad (stop ? stopGlobals (STOP_GEDACKT) : Globals(), voicesN, moving);
                std::printf ("  %s %2d voices %-14s %6.1f %% of one core  (%.2f %% per voice)\n", stop ? "Gedackt  " : "Prinzipal", voicesN,
                             moving ? "knob moving" : "steady", load, load / voicesN);
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
            // (decaying: at least 1 dB less from 14-15 s to 24-25 s; e.g. a stopped pipe of 11 m at loss x0.3 rings at
            //  15 Hz, falling 0.3 dB a second, energy ratio 0.5-0.6)
            if (e25 < 0.8 * e15) ++longRings;
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
        g.ears = 0.0; g.arch = 0.0;            // (V2: parts Phase 1 does not have)
        const int midi = 24 + (int) (U (rng) * 85);
        ParamVec p;
        voiceParams (midi, g, VoiceOverride {}, designPrinzipal (midi), p);
        p[P_KCAL] *= std::exp2 ((U (rng) - 0.5) * 300.0 / 1200.0);     // servo corrections
        const double fs = rates[trial % 4];
        Derived dv;
        derive (p.data(), fs, dv, true);            // (the Phase 1 losses: v1.1 adds the radiation stage, checked in "tone")
        double ref[8];
        labium_debug_derive (p.data(), fs, ref);
        const double rg = std::max (std::abs ((ref[0] + ref[1]) / (1.0 + ref[2])), std::abs ((ref[0] - ref[1]) / (1.0 - ref[2])));
        if (rg > 0.9995) { ++clampedN; continue; }       // P3 scales non-passive fits back
        const double dA = std::abs (dv.la1 - ref[2]), dB = std::max (std::abs (dv.lb0 - ref[0]), std::abs (dv.lb1 - ref[1]));
        const double dD = std::abs (dv.dline - ref[3]);
        // (the delay to 1e-6 samples, or 1e-9 of itself: the two cores round differently, about 2e-10 of a long delay,
        //  e.g. 1.3e-6 of 6800 samples at MIDI 24, 96 kHz)
        if (dA > 1e-8 || dB > 1e-8 || dD > std::max (1e-6, 1e-9 * std::abs (ref[3])))
        {
            ++big;
            std::printf ("    differs: trial %d midi %d fs %.0f stop %.0f temperament %.0f pitchA %.1f | d a1 %.2e d b %.2e d delay %.2e (delay %.3f samples)\n",
                         trial, midi, fs, g.stop, g.temperament, g.pitchA, dA, dB, dD, ref[3]);
        }
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
        // V2: the yarn head, F = K x^p (p 2.54): its rigid-wall contact time from the energy integral
        s.head = kNumHeads - 1;
        m.setParams (s);
        m.strike (1.0);
        m.render (9600, sh.data(), q.data());
        const double p = headProps (s.head).p, qq = 1.0 / (p + 1.0);
        const double tP = 2.0 * std::pow ((p + 1.0) * m.malletMass() / (2.0 * m.contactStiffness()), qq) * std::sqrt (kPi)
                          * std::tgamma (1.0 + qq) / std::tgamma (0.5 + qq);
        std::printf ("  contact, yarn on thick zinc at 1 m/s: %.3f ms (F = K x^%.2f, rigid wall: %.3f ms), peak force %.1f N\n",
                     m.lastContactTime() * 1e3, p, tP * 1e3, m.lastPeakForce());
        check (std::string (headProps (s.head).name) == "Yarn" && m.lastContactTime() > 0.95 * tP && m.lastContactTime() < 1.15 * tP
                   && m.lastContactTime() > 3.0 * tH,
               "yarn: contact time follows its power law, several times the rubber head's");
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
            g.metal = (double) (t % 5); g.head = (double) ((t / 5) % kNumHeads);
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
    v.jetJitter = Engine::kJetJitter;
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
/* V2: the knobs of a stop as built (what selecting it sets) */
Globals stopGlobals (int stop)
{
    double v[kMaxParamsCheck];
    stopValues (stop, v);
    Globals g;
    for (int i = 0; i < kNumParams; ++i) g.*(kParamDefs[i].field) = v[i];
    return g;
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
    // --- V2: the compass is MIDI 24-96 for both stops: its ends sound, the notes beyond it do nothing
    {
        Globals g;
        const std::vector<float> lo = engineNote (g, 24, 3.0, 1.5), hi = engineNote (g, 96, 2.0);
        const std::vector<float> below = engineNote (g, 23, 1.0), above = engineNote (g, 97, 1.0);
        const Globals gg = stopGlobals (STOP_GEDACKT);
        const std::vector<float> glo = engineNote (gg, 24, 3.0, 1.5), ghi = engineNote (gg, 96, 2.0), gout = engineNote (gg, 97, 1.0);
        auto peakOf = [] (const std::vector<float>& x, bool& fin) { double p = 0; for (float v : x) { p = std::max (p, (double) std::abs (v)); fin = fin && std::isfinite (v); } return p; };
        bool fin = true;
        const double pl = peakOf (lo, fin), ph = peakOf (hi, fin), pb = peakOf (below, fin), pa = peakOf (above, fin);
        const double gl = peakOf (glo, fin), gh = peakOf (ghi, fin), go = peakOf (gout, fin);
        std::printf ("  Prinzipal MIDI 24 (%.1f Hz, a %.2f m pipe): peak %.3f; MIDI 96: peak %.4f; MIDI 23 / 97: peak %.1e / %.1e\n",
                     midiToHz (24), designPrinzipal (24).lPhys, pl, ph, pb, pa);
        std::printf ("  Gedackt MIDI 24 (a %.2f m body): peak %.3f; MIDI 96 (%.0f mm): peak %.4f; MIDI 97: peak %.1e\n",
                     designGedackt (24).lPhys, gl, designGedackt (96).lPhys * 1e3, gh, go);
        check (fin && pl <= 1.0 && ph <= 1.0 && pl > 1e-3 && ph > 1e-4 && gl > 1e-3 && gh > 1e-4, "the compass's ends (MIDI 24, 96) sound on both stops, finite");
        check (pb == 0.0 && pa == 0.0 && go == 0.0, "notes outside MIDI 24-96 sound nothing");
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
        // (the 1.x recipe "Overblow closes the pipe": full wind on a mouth cut low, overblow -> open/stopped)
        Globals g2;
        g2.bellows = 5; g2.cutup = 0.65; g2.pitchLock = 0; g2.modSrc1 = MS_OVERBLOW; g2.modDst1 = 5; g2.modAmt1 = 1.0;
        g2.reverb = 0;
        e2.setGlobals (g2);
        e2.noteOn (48, 48, 100);       // (below c1, where 480 mmWS overblows)
        double mMax = 0, obMax = 0;
        for (int b = 0; b < 300; ++b)
        {
            e2.process (L.data(), R.data(), 480);
            for (int i = 0; i < Engine::kMaxVoices; ++i) if (e2.voice (i).active) { mMax = std::max (mMax, e2.voice (i).frame()[P_MORPH]); obMax = std::max (obMax, e2.voice (i).modOver); }
        }
        std::printf ("  'Overblow closes the pipe' (c, Wind pressure 5.00, cut-up x0.65): overblow source up to %.2f, top closed up to %.0f %%\n", obMax, mMax * 100);
        check (obMax > 0.5 && mMax > 0.5, "matrix: the pipe's own overblowing closes its top");
    }
    // --- every program (V2: every stop) plays a short chord, finite and within full scale
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
        std::printf ("  %d programs (stops): chord and release, bad samples or silent programs %d\n", (int) factoryPresets().size(), bad);
        check (bad == 0, "every program sounds, finite and within full scale");
        check ((int) factoryPresets().size() == kNumStops, "the programs are the stops");
    }
}

/* ================================================================ 11. V2: Gedackt 8' */
ParamVec gedacktPipe (int midi, const Globals& g)
{
    ParamVec p;
    voiceParams (midi, g, VoiceOverride {}, designGedackt (midi), p);
    return p;
}

/* partials 1..12 of a steady segment, dB re the fundamental (the measured f), and the level [dB SPL at 1 m] */
Metrics gedacktTone (const ParamVec& p, double dur, double from, bool servo = false, double servoStart = 0.0)
{
    const auto m = renderCpp (p, dur, FS, servo, -1, nullptr, servoStart, Engine::kJetJitter);
    return analyze (m, FS, p[P_FTARGET], from);
}

void testGedackt()
{
    std::printf ("\n== 11. Gedackt 8' (V2): design, speech across the compass, open top, pitch lock, ears, arched cut-up, stop change, wooden box ==\n");
    const Globals G = stopGlobals (STOP_GEDACKT);
    // (a) the design: Dickson's Stopped Diapason (CC 82.5 x 101.6 mm, halving every 19.7 HT; v2.0 until then Janke's
    // 60 x 75 mm), Janke's norm cut-up + 5 HT (his +7.5, 2.5 lower at low wind), a quarter wave less Fletcher & Rossing's
    // mouth correction
    {
        std::printf ("  key  f [Hz]   W x D [mm]       cut-up  H/W   flue   M [mm]  body [mm]  Ising  (75 mmWS)\n");
        bool sane = true;
        for (int k : { 24, 36, 48, 60, 72, 84, 96 })
        {
            const PipeDesign d = designGedackt (k);
            char nm[8];
            organName (k, nm);
            std::printf ("  %-4s %7.1f  %6.1f x %6.1f  %6.2f  %4.2f  %5.2f  %6.1f  %8.1f  %5.2f\n", nm, d.f0, d.mouthW * 1e3, d.depth * 1e3, d.cutup * 1e3,
                         d.cutup / d.mouthW, d.flue * 1e3, d.M * 1e3, d.lPhys * 1e3, d.ising);
            sane = sane && d.lPhys > 0.0 && d.ising > 1.0 && d.ising < 3.0 && std::abs (d.mouthW / d.depth - 82.5 / 101.6) < 1e-9;
        }
        const PipeDesign c = designGedackt (36), c1 = designGedackt (60), tc = designGedackt (48);
        check (std::abs (c.mouthW - 0.0825) < 1e-9 && std::abs (c.depth - 0.1016) < 1e-9 && std::abs (tc.mouthW * 1e3 - 54.0) < 0.3
                   && std::abs (tc.depth * 1e3 - 66.7) < 0.3,
               "Gedackt C 82.5 x 101.6 mm, tenor C 54.0 x 66.7 mm inside (Dickson's Stopped Diapason)");
        check (std::abs (c1.cutup - 1.464 * std::pow (2.0, 5.0 / 16.0) * kPi * 0.1555 * std::pow (2.0, -24.0 / 16.0) / 16.0) < 1e-12,
               "cut-up: Janke's norm cut-up of the sounding pitch + 7.5 - 2.5 HT (low wind) (c1: 10.8 mm x 2^(5/16)), x the voicing (c1 1.464)");
        {
            // v2.0: the user's voicing per key range on Janke's rule (C as his own house-organ Gedeckt 8', within 10 % of 35 mm)
            auto rule = [] (int k) { return std::pow (2.0, 5.0 / 16.0) * kPi * 0.1555 * std::pow (2.0, -(k - 36) / 16.0) / 16.0; };
            bool ok = std::abs (c.cutup * 1e3 - 35.0) < 3.5;
            const int keys[] { 24, 36, 54, 55, 66, 67, 78, 79, 91, 92, 96 };
            const double want[] { 1.00, 1.00, 1.00, 1.468, 1.457, 1.607, 1.580, 1.605, 1.532, 1.456, 1.410 };
            for (int i = 0; i < 11; ++i) ok = ok && std::abs (designGedackt (keys[i]).cutup / rule (keys[i]) - want[i]) < 1e-9;
            check (ok, "cut-up voicing per key: x1.00 to f#, g 1.468 .. f#1 1.457, g1 1.607 .. f#2 1.580, g2 1.605 .. g3 1.532, g#3 1.456 .. c4 1.410 (C within 10 % of Janke's 35 mm)");
        }
        check (std::abs (kGedacktFlue47 * std::pow (9.0 / 47.0, kGedacktFlueExp) - 0.6e-3) < 0.006e-3
                   && std::abs (c.flue - kGedacktFlue47 * std::pow (c.mouthW / 0.047, kGedacktFlueExp)) < 1e-12
                   && tc.flue <= 1.5875e-3 && c.flue < 3.175e-3 && designGedackt (24).flue < 3.175e-3,
               "flue of real wooden pipes (BDO 47 mm: 1 mm, Schuke 9 mm: 0.6 mm), within Dickson's (tenor C <= 1/16 in, CC < 1/8 in)");
        check (sane && c.lPhys > 1.10 && c.lPhys < 1.30 && designGedackt (24).lPhys > 2.3 && designGedackt (96).lPhys > 0.012
               && designGedackt (96).lPhys < 0.045, "body lengths: C about 1.2 m (Dickson: about 4 ft), C, about 2.4 m, c4 2 cm; Ising 1..3");
        // the stop's own defaults (RESET) and the Prinzipal's untouched
        int prBad = 0;
        for (int i = 0; i < kNumParams; ++i) if (std::string (kParamDefs[i].id) != "stop" && stopDefault (STOP_PRINZIPAL, i) != kParamDefs[i].def) ++prBad;
        check (prBad == 0 && G.morph == 1.0 && G.ears == 0.0 && G.arch == 0.0 && G.stop == 1.0 && std::abs (G.loss - 1.27) < 1e-12
               && std::abs (G.scaleHT - gedacktScaleHT()) < 1e-12 && G.y0b == 0.0,
               "stop defaults: Gedackt closed 100 %, no ears (0 %), arch 0 % (straight), wall loss 1.27, lip on the jet (0 b); Prinzipal's as before");
    }
    // (b) every third key, as built (pitch lock off): speaks on its fundamental, near the key, odd partials over even ones
    {
        std::printf ("  key    f [Hz]  cents  regime  level |   H2    H3    H4    H5    H6    H7\n");
        {
            const Metrics mp = analyze (renderCpp ([] { ParamVec v; Globals g; voiceParams (24, g, VoiceOverride {}, designPrinzipal (24), v); return v; }(),
                                                   4.0, FS, false, -1, nullptr, 0.0, Engine::kJetJitter), FS, midiToHz (24), 3.0);
            std::printf ("  (Prinzipal C, for comparison: %.1f dB, x%.2f)\n", mp.db, mp.regime);
        }
        int notFund = 0, offKey = 0, quiet = 0, evenOverOdd = 0, n = 0;
        double evenGap = 0;
        for (int k = 24; k <= 96; k += 3)
        {
            const ParamVec p = gedacktPipe (k, G);
            const double dur = k < 40 ? 4.0 : 2.6;
            const Metrics m = gedacktTone (p, dur, dur - 1.0);
            double cand = 0;
            const double c = std::isfinite (m.f) ? cents (m.f, p[P_FTARGET]) : 999.0;
            (void) cand;
            char nm[8];
            organName (k, nm);
            std::printf ("  %-5s %8.2f %+6.1f  %5.2f  %5.1f | %5.1f %5.1f %5.1f %5.1f %5.1f %5.1f\n", nm, m.f, c, m.regime, m.db,
                         m.hm[1], m.hm[2], m.hm[3], m.hm[4], m.hm[5], m.hm[6]);
            if (! (std::abs (m.regime - 1.0) < 0.06)) ++notFund;
            if (! (std::abs (c) < 10.0)) ++offKey;
            if (! (m.db > 45.0)) ++quiet;
            // the even partials (2, 4) against their odd neighbours (1 and 3, 3 and 5), where they are below 15 kHz
            if (std::isfinite (m.hm[1]) && std::isfinite (m.hm[2]))
            {
                const double g2 = m.hm[1] - 0.5 * (0.0 + m.hm[2]);
                evenGap += g2; ++n;
                if (g2 > 0) ++evenOverOdd;
            }
        }
        std::printf ("  even partial H2 against the mean of H1 and H3: %.1f dB on average; H2 above that mean at %d of %d keys\n", n ? evenGap / n : 0.0, evenOverOdd, n);
        check (quiet == 0, "Gedackt: every key speaks (above 45 dB at 1 m from the mouth)");
        check (notFund == 0, "Gedackt: every key on its fundamental (stopped pipe, closed)");
        check (offKey == 0, "Gedackt as built (pitch lock off, its stopper set at the reference wind): within 10 cents of the key");
        check (n > 0 && evenGap / n < -10.0, "Gedackt: the even partials well below their odd neighbours (stopped pipe)");
        // v2.0: the lip on the jet (0 b) generates no even partial; the Bernoulli term of the jet drive leaves a weak one
        {
            const Metrics mb = gedacktTone (gedacktPipe (60, G), 2.6, 1.6);
            ParamVec po;
            voiceParams (60, Globals(), VoiceOverride {}, designPrinzipal (60), po);
            const Metrics mp = gedacktTone (po, 2.6, 1.6);
            std::printf ("  c1, lip on the jet: H2 %.1f dB, H3 %.1f dB (the Bernoulli term); Prinzipal H2 %.1f dB\n", mb.hm[1], mb.hm[2], mp.hm[1]);
            check (mb.hm[1] > -90.0 && mb.hm[1] < -50.0 && mb.hm[1] < mb.hm[2] - 40.0,
                   "Gedackt, lip on the jet: a weak octave from the Bernoulli term of the jet drive (Fletcher 1976), far below the twelfth");
        }
    }
    // (c) the stopper drawn (Open 100 %): the same body sounds about an octave up, a little flat (end correction at the open top)
    {
        Globals go = G;
        go.morph = 0.0;
        int octave = 0, n = 0;
        std::printf ("  open:");
        for (int k : { 36, 48, 60, 72, 84 })
        {
            const ParamVec p = gedacktPipe (k, go);
            const Metrics m = gedacktTone (p, 2.6, 1.6);
            std::printf ("  %d: x%.3f (%+.0f c re the octave)", k, m.regime, cents (m.regime, 2.0));
            // the open top's physics is checked on the pipe without the user's voicing of the cut-up: with it (x1.5-1.7
            // the cut-up from g up) the opened c2 and c3 do not settle on the octave (x2.32, x2.69; printed only)
            PipeDesign du = designGedackt (k);
            du.cutup /= gedacktCutVoicing (k);
            ParamVec pu;
            voiceParams (k, go, VoiceOverride {}, du, pu);
            const double ru = gedacktTone (pu, 2.6, 1.6).regime;
            std::printf (" [unvoiced x%.3f]", ru);
            ++n;
            if (ru > 1.75 && ru < 2.05) ++octave;
        }
        std::printf ("\n");
        check (octave == n, "Gedackt opened (cut-up unvoiced): about an octave up, flat (the open top's end correction, the jet slower than the new resonance)");
    }
    // (d) pitch lock 100 % with the servo: on the key
    {
        int bad = 0;
        std::printf ("  pitch lock 100 %%:");
        for (int k : { 36, 48, 60, 72, 84, 96 })
        {
            Globals gl = G;
            gl.pitchLock = 1.0;
            const ParamVec p = gedacktPipe (k, gl);
            const auto m = renderCpp (p, 3.0, FS, true, -1, nullptr, 1200.0 * std::log2 (kcalForStop (STOP_GEDACKT, k)), Engine::kJetJitter);
            double cand = 0;
            const size_t i0 = (size_t) (2.0 * FS);
            const double e = pitchError (m.data() + i0, m.size() - i0, FS, p[P_FTARGET], &cand);
            std::printf ("  %d: %+.2f c (x%.1f)", k, e, cand);
            if (! (std::abs (e) < 3.0 && cand == 1.0)) ++bad;
        }
        std::printf ("\n");
        check (bad == 0, "Gedackt, pitch lock 100 %: within 3 cents of the key");
    }
    // (e) ears and (f) the arched cut-up, at c1 (pitch lock off)
    {
        Globals gE = G;
        gE.ears = 1.0;          // (v2.0: the Gedackt starts without them, 0 %)
        const Metrics mE = gedacktTone (gedacktPipe (60, gE), 2.6, 1.6), mN = gedacktTone (gedacktPipe (60, G), 2.6, 1.6);
        const double dM = mouthCorrection (true, designGedackt (60).d, designGedackt (60).mouthW, designGedackt (60).cutup, 1.0)
                          - mouthCorrection (true, designGedackt (60).d, designGedackt (60).mouthW, designGedackt (60).cutup, 0.0);
        std::printf ("  ears at c1: %.2f Hz with, %.2f Hz without (%+.1f c; their end correction %+.1f mm); level %+.1f dB, H3 %+.1f dB\n", mE.f, mN.f,
                     cents (mE.f, mN.f), dM * 1e3, mE.db - mN.db, mE.hm[2] - mN.hm[2]);
        check (mE.f < mN.f && cents (mE.f, mN.f) > -40.0, "ears: the pipe a little flatter (their air plug at the mouth)");
        Globals ga = G;
        ga.arch = 0.0;
        const Metrics mS = gedacktTone (gedacktPipe (60, ga), 2.6, 1.6);
        std::printf ("  arched cut-up at c1: level %.1f dB (straight %.1f), H3 %.1f (%.1f), H5 %.1f (%.1f), f %.2f (%.2f) Hz\n", mE.db, mS.db,
                     mE.hm[2], mS.hm[2], mE.hm[4], mS.hm[4], mE.f, mS.f);
        check (std::abs (mE.regime - 1.0) < 0.06 && std::abs (mS.regime - 1.0) < 0.06 && mE.db > 50 && mS.db > 50, "arched and straight cut-up both speak on the fundamental");
        // the strips' mean is the straight cut-up: same mouth area, same end correction
        ParamVec pa = gedacktPipe (60, G), ps = gedacktPipe (60, ga);
        Derived da, ds;
        derive (pa.data(), FS, da);
        derive (ps.data(), FS, ds);
        check (std::abs (da.M - ds.M) < 1e-12 && std::abs (da.Leff - ds.Leff) < 1e-12, "the arch keeps the mouth's area and end correction");
    }
    // (g) the rectangular bore's wall loss: 2 S / P, x 1.27 for wood
    {
        ParamVec p = gedacktPipe (48, G);
        Derived dv;
        derive (p.data(), FS, dv);
        const double aEff = p[P_MOUTHW] * p[P_DEPTH] / (p[P_MOUTHW] + p[P_DEPTH]), aEq = 0.5 * p[P_D];
        std::printf ("  c (130.8 Hz): %.1f x %.1f mm, loss radius 2S/P %.2f mm vs equal-area %.2f mm (x%.3f the round pipe's loss), loss %.2f\n",
                     p[P_MOUTHW] * 1e3, p[P_DEPTH] * 1e3, aEff * 1e3, aEq * 1e3, aEq / aEff, p[P_LOSS]);
        const double asp = p[P_DEPTH] / p[P_MOUTHW];         // (Dickson's 101.6 / 82.5; was Janke's 75 / 60 = 1.25)
        check (std::abs (aEq / aEff - (std::sqrt (asp) + 1.0 / std::sqrt (asp)) / std::sqrt (kPi)) < 1e-9, "rectangular wall loss: (sqrt e + 1/sqrt e) / sqrt pi of the round pipe (Rucz et al.)");
    }
    // (g2) a stopped top radiates nothing: the voice's output (L + R = 1.2 (mouth + top)) is the mouth alone
    {
        const ParamVec p = gedacktPipe (60, G);
        const int n = (int) (2.0 * FS);
        Voice v;
        v.jetJitter = Engine::kJetJitter;
        v.servoEnabled = false;
        v.prepare (FS, 3);
        v.start (p, 0.0);
        std::vector<double> mouth ((size_t) n), L ((size_t) n), R ((size_t) n);
        for (int i = 0; i < n; i += 4096) { const int m = std::min (4096, n - i); v.render (m, L.data() + i, R.data() + i, mouth.data() + i); }
        double et = 0, em = 0;
        for (int i = n / 2; i < n; ++i)
        {
            const double top = (L[(size_t) i] + R[(size_t) i]) / 1.2 - mouth[(size_t) i];
            et += top * top; em += mouth[(size_t) i] * mouth[(size_t) i];
        }
        std::printf ("  c1 stopped: the top radiates %.1f dB re the mouth\n", 10 * std::log10 (et / em + 1e-30));
        check (et < 1e-6 * em, "a stopped top radiates nothing (the pipe's lumped losses are not a flow through the stopper)");
    }
    // (h) another stop drawn while a key is held: the old pipe dies away, the key speaks on the new stop
    {
        Engine e;
        e.prepare (48000, 480);
        Globals gp = stopGlobals (STOP_PRINZIPAL);
        gp.reverb = 0;
        e.setGlobals (gp);
        std::vector<float> L (480), R (480);
        e.noteOn (60, 60, 100);
        for (int b = 0; b < 50; ++b) e.process (L.data(), R.data(), 480);
        Globals gg = G;
        gg.reverb = 0;
        e.setGlobals (gg);
        int newStop = -1, frozenAt = -1;
        for (int b = 0; b < 100; ++b)
        {
            e.process (L.data(), R.data(), 480);
            int frozen = 0;
            for (int i = 0; i < Engine::kMaxVoices; ++i)
            {
                const Voice& v = e.voice (i);
                if (v.active && v.id == 60) newStop = v.stop;
                if (v.active && v.frozen) ++frozen;
            }
            if (frozen == 0 && frozenAt < 0) frozenAt = b;
        }
        std::printf ("  stop change with c1 held: the key now on stop %d; the Prinzipal's pipe gone after %d ms\n", newStop, frozenAt * 10);
        check (newStop == STOP_GEDACKT && frozenAt >= 0 && frozenAt < 80, "stop change: the held key speaks on the new stop, the old pipe dies away");
        // what the screens show of it: the pitch against the key (the Gedackt's key is its stopped fundamental)
        for (int b = 0; b < 150; ++b) e.process (L.data(), R.data(), 480);
        Telemetry t;
        e.fillTelemetry (t);
        std::printf ("  its telemetry: %.2f Hz, regime x%.1f of the open resonance, %+.2f cents against the key\n", t.fMeas, t.ratio, t.cents);
        check (std::abs (t.ratio - 0.5f) < 0.01f && std::abs (t.cents) < 10.0f, "the screens' pitch: the Gedackt on its key (not an octave off)");
    }
    // (i) the wooden box struck (MALLET): plate modes of the front board, contact, decay, the air column it drives
    {
        for (int k : { 36, 60, 84 })
        {
            const ParamVec p = gedacktPipe (k, G);
            Voice v;
            v.prepare (96000, 3);
            v.setHostRate (48000);
            ParamVec pm = p;
            pm[P_EXCITE] = 1.0; pm[P_GATE] = 0.0;
            v.start (pm, 0.0);
            v.strike (1.0);
            const MalletModel& md = v.malletModel();
            // the (1, 1) mode against Leissa's formula for the board
            const WoodProps& wd = woodProps (0);
            const double b = p[P_MOUTHW], Lb = p[P_LPHYS], h = woodWallThickness (b);
            const double ET = wd.ETL * wd.EL, nuTL = wd.nuLT * ET / wd.EL, den = 12.0 * (1.0 - wd.nuLT * nuTL);
            const double d11 = wd.EL * h * h * h / den, d22 = ET * h * h * h / den, d12 = wd.nuLT * d22, d66 = wd.GLTL * wd.EL * h * h * h / 12.0;
            auto fmn = [&] (int m, int n) { const double x = m / Lb, y = n / b; return kPi / 2.0 * std::sqrt ((d11 * x * x * x * x + 2 * (d12 + 2 * d66) * x * x * y * y + d22 * y * y * y * y) / (wd.rho * h)); };
            double f11 = -1;
            for (int i = 0; i < md.modeCount(); ++i) if (md.modeN (i) == 1 && md.modeM (i) == 1) f11 = md.modeFreq (i);
            std::vector<double> L (48000), R (48000), hm (24000), hs (24000);
            std::vector<double> mixL (96000), mixR (96000);
            double pk = 0, e0 = 0, e1 = 0;
            bool fin = true;
            for (int blk = 0; blk < 4; ++blk)
            {
                std::fill (hm.begin(), hm.end(), 0.0);
                std::fill (mixL.begin(), mixL.end(), 0.0);
                v.render (48000, mixL.data(), mixR.data(), nullptr, hm.data(), nullptr);
                for (int i = 0; i < 24000; ++i)
                {
                    const double y = hm[(size_t) i] + 0.5 * (mixL[(size_t) (2 * i)] + mixL[(size_t) (2 * i + 1)]);
                    fin = fin && std::isfinite (y);
                    pk = std::max (pk, std::abs (y));
                    if (blk == 0 && i < 12000) e0 += y * y;
                    if (blk == 3) e1 += y * y;
                }
            }
            std::printf ("  struck %d (oak, board %.1f mm wide, %.1f mm thick): %d modes, f(1,1) %.0f Hz (Leissa %.0f), contact %.3f ms, peak %.2f Pa, 1.5-2 s vs 0-0.25 s %.1f dB\n",
                         k, b * 1e3, h * 1e3, md.modeCount(), f11, fmn (1, 1), md.lastContactTime() * 1e3, pk, 10 * std::log10 ((e1 / 24000.0) / std::max (1e-30, e0 / 12000.0)));
            check (fin && pk > 1e-3 && pk < 1e3, "struck wooden box: finite, it sounds");
            check (f11 < 0 || std::abs (f11 / fmn (1, 1) - 1.0) < 1e-6, "the board's (1,1) mode = Leissa's simply supported orthotropic plate");
            check (e1 < e0, "struck wooden box: it dies away");
        }
    }
}

/* V2: the Gedackt's pitch-lock calibration table (labium_check kcalgen): the factor that puts the locked, servo-free pipe
   on its key, iterated twice; printed as src/dsp/KcalTableGedackt.inc */
void kcalGen()
{
    const Globals G0 = stopGlobals (STOP_GEDACKT);
    std::printf ("// V2: the Gedackt 8' pitch-lock calibration (labium_check kcalgen: pitch lock 100 %%, servo off, the engine's jet jitter)\n");
    std::printf ("static constexpr int kKcalGFirst = 24, kKcalGCount = 73;\nstatic constexpr double kKcalG[kKcalGCount] = {\n");
    for (int k = 24; k <= 96; ++k)
    {
        // (v2.0: until it is within 0.1 cent, at most 8 times: with Dickson's wide scale the mouth's lumped mass is up to
        // half of the quarter wave, and the pitch follows the stopper less than in proportion)
        double kc = 1.0;
        for (int it = 0; it < 8; ++it)
        {
            Globals g = G0;
            g.pitchLock = 1.0;
            ParamVec p = gedacktPipe (k, g);
            p[P_KCAL] = kc;
            p[P_FDESIGN] = p[P_FTARGET] / kc;          // (the cut-up voiced for the stopper being tried: baseParams)
            const double dur = k < 40 ? 5.0 : 2.6, win = k < 40 ? 3.0 : 1.0;      // (the 16' octave: as a tuner hears it, 2-5 s)
            const auto m = renderCpp (p, dur, FS, false, -1, nullptr, 0.0, Engine::kJetJitter);
            double cand = 0, f = 0;
            const size_t i0 = (size_t) ((dur - win) * FS);
            pitchError (m.data() + i0, m.size() - i0, FS, p[P_FTARGET], &cand, &f);
            if (! (cand > 0 && f > 0)) break;
            const double r = f / (cand * p[P_FTARGET]);
            kc *= r;
            if (std::abs (1200.0 * std::log2 (r)) < 0.1) break;
        }
        std::printf ("    %.6f,  // %d\n", kc, k);
    }
    std::printf ("};\n");
}

/* ================================================================ 12. v2.0: tuning */
/* temperaments from their circles of fifths, Scala files (tuning-library), the engine playing the tuned pipes */
void testTuning()
{
    std::printf ("\n== 12. tuning (v2.0): equal at 440 Hz moves nothing, the temperaments, Pitch a1, Scala .scl / .kbm, the tuned pipes ==\n");
    const CustomTuning none;
    // (a) equal temperament at 440 Hz: every key its own pipe, nothing cut (the sound of 2.0 without tuning, bit for bit)
    {
        Globals g;
        int bad = 0;
        for (int m = 0; m < 128; ++m)
        {
            double s = -1.0, rest = -1.0;
            const bool on = keyTuning (g, none, m, s);
            const int k = pipeKeyFor (m, s, rest);
            if (! on || s != 0.0 || k != m || rest != 0.0) ++bad;
        }
        check (bad == 0, "equal temperament, a1 = 440 Hz: every key exactly its own pipe (0.0 semitones)");
    }
    // (b) Werckmeister III against Barbour's table (cents from C), a1 on the pitch
    {
        const double ref[12] { 0, -9.775, -7.820, -5.865, -9.775, -1.955, -11.730, -3.910, -7.820, -11.730, -3.910, -7.820 };
        double worst = 0;
        for (int pc = 0; pc < 12; ++pc)
            worst = std::max (worst, std::abs (temperamentCents (TEMP_WERCKMEISTER3, pc) - temperamentCents (TEMP_WERCKMEISTER3, 0) - ref[pc]));
        std::printf ("  Werckmeister III: C %+.2f, E %+.2f, F# %+.2f, A %+.2f cents from equal (a1 kept); worst against the table %.4f\n",
                     temperamentCents (TEMP_WERCKMEISTER3, 0), temperamentCents (TEMP_WERCKMEISTER3, 4), temperamentCents (TEMP_WERCKMEISTER3, 6),
                     temperamentCents (TEMP_WERCKMEISTER3, 9), worst);
        check (worst < 0.01 && temperamentCents (TEMP_WERCKMEISTER3, 9) == 0.0, "Werckmeister III: Barbour's departures from equal temperament, a1 unmoved");
    }
    // (c) what each circle promises: pure thirds of meantone and Kirnberger III, pure fifths of Pythagorean, Young's 1/6-comma fifth
    {
        auto iv = [] (int t, int lo, int hi, int semis) { return 100.0 * semis + temperamentCents (t, hi) - temperamentCents (t, lo); };
        const double third = 1200.0 * std::log2 (1.25), fifth = 1200.0 * std::log2 (1.5), pc = 1200.0 * std::log2 (531441.0 / 524288.0);
        const double mt = iv (TEMP_MEANTONE, 0, 4, 4), k3 = iv (TEMP_KIRNBERGER3, 0, 4, 4), py = iv (TEMP_PYTHAGOREAN, 0, 7, 7);
        const double yo = iv (TEMP_YOUNG, 0, 7, 7), va = iv (TEMP_VALLOTTI, 11, 6, 7);
        std::printf ("  C-E: meantone %.3f, Kirnberger III %.3f (5:4 = %.3f); C-G: Pythagorean %.3f, Young %.3f; B-F#: Vallotti %.3f cents\n",
                     mt, k3, third, py, yo, va);
        check (std::abs (mt - third) < 1e-9 && std::abs (k3 - third) < 1e-9 && std::abs (py - fifth) < 1e-9 && std::abs (yo - (fifth - pc / 6.0)) < 1e-9
                   && std::abs (va - fifth) < 1e-9,
               "the circles of fifths: meantone and Kirnberger III with a pure C-E, Pythagorean C-G pure, Young's C-G 1/6 comma narrow, Vallotti's B-F# pure");
    }
    // (d) Pitch a1: a1 sounds at the knob in every temperament
    {
        Globals g;
        g.temperament = TEMP_WERCKMEISTER3;
        g.pitchA = 415.0;
        double s = 0;
        keyTuning (g, none, 69, s);
        std::printf ("  Werckmeister III at a1 = 415 Hz: a1 %.4f Hz\n", midiToHz (69) * std::exp2 (s / 12.0));
        check (std::abs (midiToHz (69) * std::exp2 (s / 12.0) - 415.0) < 1e-9, "Pitch a1: a1 on the knob");
    }
    // (e) a Scala scale (five equal steps of 240 cents, written here) without a .kbm: 1/1 on c1, a1 on the pitch; a .kbm leaving c#1 out
    const std::string scl5 = "! five.scl\n!\nFive equal steps of 240 cents (labium_check)\n 5\n!\n 240.0\n 480.0\n 720.0\n 960.0\n 2/1\n";
    const std::string kbmX = "! no c#1\n 12\n 0\n 127\n 60\n 69\n 440.0\n 12\n! keys\n 0\n x\n 2\n 3\n 4\n 5\n 6\n 7\n 8\n 9\n 10\n 11\n";
    CustomTuning five, gap;
    {
        TuningInfo info;
        const bool ok = buildCustomTuning (scl5, "", five, info);
        std::printf ("  five.scl: %s, \"%s\", %d notes; c1 %+.4f, a1 %+.4f, a#1 %+.4f semitones from equal\n", ok ? "read" : info.error.c_str(),
                     info.description.c_str(), info.notes, five.semis[60], five.semis[69], five.semis[70]);
        check (ok && info.notes == 5 && five.usesPitch && std::abs (five.semis[69]) < 1e-9 && std::abs (five.semis[70] - 1.4) < 1e-9
                   && std::abs (five.semis[60] + 12.6) < 1e-9,
               "a .scl without a .kbm: 1/1 on c1, a1 at 440 Hz (a#1 = a1 + 240 cents: +1.4 semitones)");
        TuningInfo info2;
        const bool ok2 = buildCustomTuning ("", kbmX, gap, info2);
        std::printf ("  no-c#1.kbm alone: %s; c1 %s, c#1 %s, d1 %s (%+.4f)\n", ok2 ? "read" : info2.error.c_str(), gap.mapped[60] ? "mapped" : "out",
                     gap.mapped[61] ? "mapped" : "out", gap.mapped[62] ? "mapped" : "out", gap.semis[62]);
        check (ok2 && ! gap.usesPitch && ! gap.mapped[61] && gap.mapped[60] && gap.mapped[62] && std::abs (gap.semis[62]) < 1e-9,
               "a .kbm alone (equal scale): the key it leaves out is unmapped, the others where they were");
        CustomTuning keep = five;
        TuningInfo bad;
        const bool okBad = buildCustomTuning ("! bad\nnot a scale\n 3\n 100.0\n", "", keep, bad);
        std::printf ("  a broken .scl: %s\n", okBad ? "accepted" : bad.error.c_str());
        check (! okBad && ! bad.error.empty() && keep.valid && keep.semis[70] == five.semis[70], "a broken .scl is refused with a reason, the tuning stays");
    }
    // (f) the engine: Werckmeister III with pitch lock on the tuned pitch; the five-step scale on other keys' pipes; a left-out key silent
    {
        Engine e;
        e.prepare (48000, 480);
        Globals g;
        g.reverb = 0;
        g.temperament = TEMP_WERCKMEISTER3;
        g.pitchLock = 1.0;
        e.setGlobals (g);
        std::vector<float> L (480), R (480);
        e.noteOn (64, 64, 100);
        for (int b = 0; b < 200; ++b) e.process (L.data(), R.data(), 480);
        Telemetry t;
        e.fillTelemetry (t);
        const double want = midiToHz (64) * std::exp2 (temperamentCents (TEMP_WERCKMEISTER3, 4) / 1200.0);
        std::printf ("  Werckmeister III, e1, pitch lock 100 %%: target %.3f Hz (table %.3f), sounding %.3f Hz, %+.2f cents\n", t.fTarget, want, t.fMeas, t.cents);
        check (std::abs (t.fTarget - want) < 1e-3 && std::abs (t.cents) < 3.0, "a tempered key: its pipe on the tempered pitch (pitch lock)");
        e.noteOff (64);
        for (int b = 0; b < 300; ++b) e.process (L.data(), R.data(), 480);
        g.temperament = TEMP_CUSTOM;
        e.setGlobals (g);
        e.setCustomTuning (five);
        e.noteOn (70, 70, 100);
        for (int b = 0; b < 20; ++b) e.process (L.data(), R.data(), 480);
        int pipe = -1;
        for (int i = 0; i < Engine::kMaxVoices; ++i)
            if (e.voice (i).active && e.voice (i).id == 70) pipe = e.voice (i).pipeKey;
        e.fillTelemetry (t);
        std::printf ("  five.scl, a#1: sounds the pipe of key %d, target %.3f Hz (a1 + 240 cents: %.3f)\n", pipe, t.fTarget, 440.0 * std::exp2 (0.2));
        check (pipe == 71 && std::abs (t.fTarget - 440.0 * std::exp2 (0.2)) < 1e-3, "a key 1.4 semitones up: the pipe of the nearest key (h1), cut 0.4 semitones shorter");
        e.noteOff (70);
        e.setCustomTuning (gap);
        e.noteOn (61, 61, 100);
        int sounding = 0;
        for (int i = 0; i < Engine::kMaxVoices; ++i) if (e.voice (i).active && e.voice (i).id == 61) ++sounding;
        check (sounding == 0, "a key the .kbm leaves out has no pipe");
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

/* v1.1: the excitation and losses of an organ pipe (the spreading jet, the radiation stage) */
void testTone()
{
    std::printf ("\n== 10. tone: the bore's losses and the Prinzipal's partials ==\n");
    // (a) the wall shelf x the radiation stage against the physical round trip (wall x Norris & Sheng |R| of both ends),
    //     random knob settings: stable, passive, and close to the physics at the harmonics
    {
        std::mt19937 rng (11);
        std::uniform_real_distribution<double> U (0.0, 1.0);
        auto rNS = [] (double ka) { return ka > 3.5 ? 0.0987 * (3.5 / ka) * (3.5 / ka) : (1 + 0.2 * ka - 0.084 * ka * ka) / (1 + 0.2 * ka + 0.416 * ka * ka); };
        double worst = 0, worstDefault = 0, worstGain = 0;
        int unstable = 0, n = 0;
        char worstAt[160] = "", worstDefAt[160] = "";
        for (int trial = 0; trial < 4000; ++trial)
        {
            Globals g;
            const bool dflt = trial < 400;               // the first sets: default knobs, every key
            if (! dflt)
                for (int i = 0; i < kNumParams; ++i)
                {
                    const ParamDef& d = kParamDefs[i];
                    double x = U (rng);
                    if (U (rng) < 0.4) x = toNorm (d, d.def);
                    g.*(d.field) = fromNorm (d, x);
                }
            const int midi = dflt ? 24 + trial % 85 : 24 + (int) (U (rng) * 85);
            const double fs = trial % 2 ? 96000.0 : 88200.0;
            ParamVec p;
            voiceParams (midi, g, VoiceOverride {}, designPrinzipal (midi), p);
            Derived dv;
            derive (p.data(), fs, dv);
            {
                // pole radius of 1 + ra1 z^-1 + ra2 z^-2
                const double disc = dv.ra1 * dv.ra1 - 4.0 * dv.ra2;
                const double rmax = disc >= 0 ? std::max (std::abs (-dv.ra1 + std::sqrt (disc)), std::abs (-dv.ra1 - std::sqrt (disc))) / 2.0
                                              : std::sqrt (dv.ra2);
                if (! (rmax < 1.0 - 1e-9) || ! (std::abs (dv.la1) < 1.0)) ++unstable;
            }
            const double c = p[P_C], a = 0.5 * p[P_D], am = std::sqrt (p[P_MOUTHW] * dv.W / kPi), morph = std::clamp (p[P_MORPH], 0.0, 1.0);
            const double f1 = c / (2.0 * dv.Leff), nu = p[P_MU] / p[P_RHO];
            auto H = [&] (double f) {
                const std::complex<double> z1 = std::polar (1.0, -kTwoPi * f / fs), z2 = z1 * z1;
                return std::abs ((dv.lb0 + dv.lb1 * z1) / (1.0 + dv.la1 * z1) * (dv.rb0 + dv.rb1 * z1 + dv.rb2 * z2) / (1.0 + dv.ra1 * z1 + dv.ra2 * z2));
            };
            for (int i = 0; i <= 64; ++i) worstGain = std::max (worstGain, H (0.5 * fs * i / 64.0));
            for (int k = 1; k <= 16 && k * f1 < std::min (0.4 * fs, 20000.0); ++k)     // (the audible band: the bilinear stage warps near Nyquist)
            {
                const double f = k * f1, w = kTwoPi * f, kk = w / c;
                const double alpha = std::sqrt (w * nu / 2.0) / (a * c) * (1.0 + (p[P_GAMMA] - 1.0) / std::sqrt (0.71));
                const double G = std::exp (-2.0 * alpha * dv.Leff * p[P_LOSS]) * std::pow (rNS (kk * a), 1.0 - morph) * rNS (kk * am);
                const double e = std::abs (H (f) - G);
                char at[160];
                std::snprintf (at, sizeof at, "MIDI %d H%d (%.0f Hz, ka %.2f, k am %.2f, morph %.2f, loss %.1f): %.3f vs %.3f",
                               midi, k, f, kk * a, kk * am, morph, p[P_LOSS], H (f), G);
                if (e > worst) { worst = e; std::snprintf (worstAt, sizeof worstAt, "%s", at); }
                if (dflt && e > worstDefault) { worstDefault = e; std::snprintf (worstDefAt, sizeof worstDefAt, "%s", at); }
            }
            ++n;
        }
        std::printf ("  %d sets: round-trip gain vs physics at harmonics 1-16 below 20 kHz: worst |dG| %.3f (default knobs, keys C,, .. c5: %.3f); "
                     "max gain %.4f; unstable %d\n    worst: %s\n    worst at default knobs: %s\n", n, worst, worstDefault, worstGain, unstable,
                     worstAt, worstDefAt);
        check (unstable == 0 && worstGain <= 1.0, "loss stages stable and passive");
        check (worstDefault < 0.07, "loss stages within 0.07 of the physical round trip (default knobs)");
        check (worst < 0.15, "loss stages within 0.15 of the physical round trip (any knobs)");
    }
    // (b) the default rank (pitch lock off, reverb off, 48 kHz): partials of each ear (the mouth is panned left, the top right),
    //     the jet's swing at the labium in its local half-widths
    {
        std::printf ("  key    level |   H2    H3    H4    H5    H6    H8   H10 | H6-12 | swing p50 p95\n");
        for (int key : { 36, 48, 60, 72, 84, 96 })
        {
            const double fs = 48000.0;
            auto e = std::make_unique<Engine>();
            e->prepare (fs, 64);
            Globals g;
            for (int i = 0; i < kNumParams; ++i) g.*(kParamDefs[i].field) = kParamDefs[i].def;
            g.reverb = 0;
            e->setGlobals (g);
            e->noteOn (key, key, 100);
            const int B = 7, n = (int) (2.5 * fs);
            std::vector<float> L (B), R (B);
            std::vector<double> yl, yr, sw;
            for (int s = 0; s + B <= n; s += B)
            {
                e->process (L.data(), R.data(), B);
                if (s >= (int) (1.5 * fs))
                {
                    for (int i = 0; i < B; ++i) { yl.push_back (L[(size_t) i]); yr.push_back (R[(size_t) i]); }
                    for (int v = 0; v < Engine::kMaxVoices; ++v)
                        if (e->voice (v).active && e->voice (v).id == key) sw.push_back (std::abs (e->voice (v).etaN));
                }
            }
            const size_t N = nextPow2 (yl.size());
            std::vector<std::complex<double>> XL (N), XR (N);
            double en = 0;
            for (size_t i = 0; i < yl.size(); ++i)
            {
                const double w = 0.5 - 0.5 * std::cos (2 * kPi * i / (yl.size() - 1));
                XL[i] = yl[i] * w; XR[i] = yr[i] * w;
                en += 0.5 * (yl[i] * yl[i] + yr[i] * yr[i]);
            }
            fft (XL); fft (XR);
            const double df = fs / (double) N, f0 = midiToHz (key);
            size_t k1 = 0;
            double best = 0;
            for (size_t k = (size_t) (0.95 * f0 / df); k <= (size_t) (1.05 * f0 / df); ++k)
                if (std::norm (XL[k]) + std::norm (XR[k]) > best) { best = std::norm (XL[k]) + std::norm (XR[k]); k1 = k; }
            const double f1 = (double) k1 * df;
            double Hp[13] {};
            for (int h = 1; h <= 12; ++h)
            {
                if (h * f1 > 0.45 * fs) { Hp[h] = 0; continue; }
                const size_t kc = (size_t) std::lround (h * f1 / df);
                for (size_t k = kc - 3; k <= kc + 3; ++k) Hp[h] += 0.5 * (std::norm (XL[k]) + std::norm (XR[k]));
            }
            auto dB = [&] (double x) { return x > 0 ? 10 * std::log10 (x / Hp[1]) : -99.0; };
            double up = 0;
            bool fundStrongest = true;
            for (int h = 2; h <= 12; ++h) { if (h >= 6) up += Hp[h]; if (Hp[h] > Hp[1]) fundStrongest = false; }
            std::sort (sw.begin(), sw.end());
            const double p50 = sw.empty() ? 0 : sw[sw.size() / 2], p95 = sw.empty() ? 0 : sw[(size_t) (0.95 * (sw.size() - 1))];
            char nm[8];
            organName (key, nm);
            std::printf ("  %-5s %6.1f | %5.1f %5.1f %5.1f %5.1f %5.1f %5.1f %5.1f | %5.1f | %5.1f %5.1f\n", nm, 10 * std::log10 (en / yl.size()),
                         dB (Hp[2]), dB (Hp[3]), dB (Hp[4]), dB (Hp[5]), dB (Hp[6]), dB (Hp[8]), dB (Hp[10]), dB (up), p50, p95);
            check (fundStrongest, "the fundamental is the strongest partial");
            check (dB (Hp[2]) > -27.0 && dB (Hp[2]) < -5.0, "the octave (H2) between -27 and -5 dB");
            check (dB (up) < -20.0, "partials 6-12 together below -20 dB (no string-like upper partials)");
            check (p95 > 1.5 && p95 < 5.5, "jet swing at the labium 1.5 .. 5.5 local half-widths");
        }
    }
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
            if (pass == 2) v.jetJitter = Engine::kJetJitter;         // P5: the jet's turbulent jitter, as the engine runs it
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
                v.jetJitter = Engine::kJetJitter;
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
    if (what == "kcalgen")
    {
        kcalGen();
        return 0;
    }
    if (what == "gsustain")
    {
        // does the Gedackt hold its tone? level at 1-2 s and 7-8 s (as built; argv[2] cut-up factor, argv[3] wind factor)
        const double cut = argc > 2 ? std::atof (argv[2]) : 1.0, wind = argc > 3 ? std::atof (argv[3]) : 1.0;
        const int prin = argc > 4 ? std::atoi (argv[4]) : 0;
        for (int k = 24; k <= 60; k += 2)
        {
            Globals g = stopGlobals (prin ? STOP_PRINZIPAL : STOP_GEDACKT);
            g.cutup = cut; g.bellows = wind;
            ParamVec p;
            voiceParams (k, g, VoiceOverride {}, prin ? designPrinzipal (k) : designGedackt (k), p);
            const auto m = renderCpp (p, 8.0, FS, false, -1, nullptr, 0.0, Engine::kJetJitter);
            auto lev = [&] (double a, double b) { double e = 0; for (size_t i = (size_t) (a * FS); i < (size_t) (b * FS); ++i) e += m[i] * m[i]; return 10 * std::log10 (e / ((b - a) * FS) / 4e-10); };
            std::printf ("  %d: %.1f dB -> %.1f dB\n", k, lev (1, 2), lev (7, 8));
        }
        return 0;
    }
    if (what == "mal07")
    {
        // the host test's renders 06 and 07 on the engine: a zinc chord struck with brass, then common metal with rubber on the
        // same keys. Twice, the second time on memory filled with junk first: an uninitialised read shows as a difference
        auto run = [] (bool junk) {
            if (junk)
            {
                std::vector<std::unique_ptr<std::vector<double>>> js;
                for (int i = 0; i < 64; ++i) js.push_back (std::make_unique<std::vector<double>> ((size_t) 1 << 16, 1.2345e300 * (i + 1)));
                for (int i = 0; i < 64; ++i) { auto* p = new char[(size_t) 1 << 20]; std::memset (p, 0x7f, (size_t) 1 << 20); delete[] p; }
            }
            auto e = std::make_unique<Engine>();
            e->prepare (48000, 512);
            Globals G;
            G.excite = 1; G.metal = 3; G.head = 3;
            e->setGlobals (G);
            std::vector<float> L (512), R (512);
            uint64_t h = 1469598103934665603ull;
            auto blocks = [&] (double sec, auto&& at) {
                const int nb = (int) std::lround (sec * 48000 / 512);
                for (int b = 0; b < nb; ++b)
                {
                    at (b);
                    e->process (L.data(), R.data(), 512);
                    for (int i = 0; i < 512; ++i) { uint32_t u; std::memcpy (&u, &L[(size_t) i], 4); h = (h ^ u) * 1099511628211ull; }
                }
            };
            blocks (4.0, [&] (int b) { if (b == 9) for (int k : { 48, 55, 64, 72 }) e->noteOn (k, k, 110); });
            const uint64_t h06 = h;
            G.metal = 0; G.head = 0;
            e->setGlobals (G);
            blocks (0.5, [&] (int b) { if (b == 0) e->allOff (false); });
            blocks (4.0, [&] (int b) { if (b == 9) for (int k : { 48, 55, 64, 72 }) e->noteOn (100 + k, k, 110); });
            std::printf ("  %s: chord 1 %016llx, both %016llx\n", junk ? "junk first" : "clean     ", (unsigned long long) h06, (unsigned long long) h);
        };
        run (false);
        run (true);
        run (false);
        return 0;
    }
    if (what == "gtone")
    {
        // the Gedackt as built at a few keys (pitch lock off): pitch, level, partials (with Arched cut-up up: Voice::kStrips)
        std::printf ("  %d strips\n  key    f [Hz]  cents  level |   H2    H3    H4    H5    H6    H7\n", OKL_STRIPS);
        const Globals g = stopGlobals (STOP_GEDACKT);
        for (int k : { 24, 36, 48, 60, 72, 84, 96 })
        {
            const ParamVec p = gedacktPipe (k, g);
            const double dur = k < 40 ? 4.0 : 2.6;
            const Metrics m = gedacktTone (p, dur, dur - 1.0);
            char nm[8];
            organName (k, nm);
            std::printf ("  %-5s %8.2f %+6.2f  %5.1f | %5.1f %5.1f %5.1f %5.1f %5.1f %5.1f\n", nm, m.f, cents (m.f, p[P_FTARGET]), m.db,
                         m.hm[1], m.hm[2], m.hm[3], m.hm[4], m.hm[5], m.hm[6]);
        }
        return 0;
    }
    if (what == "gopen")
    {
        // the stopper drawn (Open/Stopped 0 %) with the cut-up following the pitch (Cut-up follow 100 %, as built) or the
        // mouth left as cut (0 %)
        std::printf ("  key   follow   f [Hz]  x stopped  regime  level |   H2    H3\n");
        for (int k : { 36, 48, 60, 72, 84 })
            for (double fo : { 1.0, 0.0 })
            {
                Globals g = stopGlobals (STOP_GEDACKT);
                g.morph = 0.0; g.vFollow = fo;
                const ParamVec p = gedacktPipe (k, g);
                const Metrics m = gedacktTone (p, 2.6, 1.6);
                char nm[8];
                organName (k, nm);
                std::printf ("  %-5s %4.0f %%  %8.2f  %7.3f  %6.2f  %5.1f | %5.1f %5.1f\n", nm, fo * 100, m.f, m.f / midiToHz (k), m.regime, m.db, m.hm[1], m.hm[2]);
            }
        return 0;
    }
    if (what == "gcpu")
    {
        // where the Gedackt's time goes: n voices (argv[2], default 8) with the arched cut-up at 100 %, as built (straight),
        // the Prinzipal (the least of 3 runs of 3 s each; argv[3] = 0: without the jet's jitter)
        const int n = argc > 2 ? std::atoi (argv[2]) : 8;
        const double jit = argc > 3 ? std::atof (argv[3]) : Engine::kJetJitter;
        Globals g = stopGlobals (STOP_GEDACKT), gs = g;
        g.arch = 1.0;
        if (n == 0)
        {
            // one voice per key: where is it dear?
            for (int k = 24; k <= 96; k += 4)
            {
                double a = 1e9, b = 1e9, c = 1e9;
                for (int rep = 0; rep < 2; ++rep)
                {
                    a = std::min (a, cpuLoad (g, 1, false, 1.5, jit, k));
                    b = std::min (b, cpuLoad (gs, 1, false, 1.5, jit, k));
                    c = std::min (c, cpuLoad (Globals(), 1, false, 1.5, jit, k));
                }
                std::printf ("  key %d: Gedackt arched %.2f %%, as built %.2f %%, Prinzipal %.2f %%\n", k, a, b, c);
            }
            return 0;
        }
        double a = 1e9, b = 1e9, c = 1e9;
        for (int rep = 0; rep < 3; ++rep)
        {
            a = std::min (a, cpuLoad (g, n, false, 3.0, jit));
            b = std::min (b, cpuLoad (gs, n, false, 3.0, jit));
            c = std::min (c, cpuLoad (Globals(), n, false, 3.0, jit));
        }
        std::printf ("  %d voices (jitter %.2f): Gedackt arched %.1f %%, as built %.1f %%, Prinzipal %.1f %% of one core (least of 3)\n", n, jit, a, b, c);
        return 0;
    }
    if (what == "gpitch" && argc > 2)
    {
        // a Gedackt key held 12 s (pitch lock 100 %, servo off, kcal 1): pitch and level in 1 s windows
        const int k = std::atoi (argv[2]);
        const double dur = argc > 3 ? std::atof (argv[3]) : 12.0;
        Globals g = stopGlobals (STOP_GEDACKT);
        g.pitchLock = 1.0;
        if (argc > 4) g.cutup = std::atof (argv[4]);
        ParamVec p = gedacktPipe (k, g);
        p[P_KCAL] = 1.0;
        const auto m = renderCpp (p, dur, FS, false, -1, nullptr, 0.0, Engine::kJetJitter);
        for (int s = 1; s < (int) dur; s += std::max (1, (int) dur / 12))
        {
            double cand = 0, f = 0, e = 0;
            const size_t i0 = (size_t) (s * FS);
            const double c = pitchError (m.data() + i0, (size_t) FS, FS, p[P_FTARGET], &cand, &f);
            for (size_t i = i0; i < i0 + (size_t) FS; ++i) e += m[i] * m[i];
            std::printf ("  %2d s: %.3f Hz  %+.1f c (x%.1f)  %.1f dB\n", s, f, c, cand, 10 * std::log10 (e / FS / 4e-10));
        }
        return 0;
    }
    if (what == "gscan")
    {
        // v2.0 diagnostic: the Gedackt as built at every third key (pitch lock off; argv[2]: pitch lock): pitch, regime,
        // level, partials 2..7
        Globals g = stopGlobals (STOP_GEDACKT);
        if (argc > 2) g.pitchLock = std::atof (argv[2]);
        std::printf ("  key    f [Hz]  cents  regime level |   H2    H3    H4    H5    H6    H7   (M/Leff)\n");
        for (int k = 24; k <= 96; k += 3)
        {
            const ParamVec p = gedacktPipe (k, g);
            const double dur = k < 40 ? 4.0 : 2.6;
            const Metrics m = gedacktTone (p, dur, dur - 1.0);
            const PipeDesign d = designGedackt (k);
            char nm[8];
            organName (k, nm);
            std::printf ("  %-5s %8.2f %+6.1f  %5.2f  %5.1f | %5.1f %5.1f %5.1f %5.1f %5.1f %5.1f   %.3f\n", nm, m.f,
                         std::isfinite (m.f) ? cents (m.f, p[P_FTARGET]) : 999.0, m.regime, m.db,
                         m.hm[1], m.hm[2], m.hm[3], m.hm[4], m.hm[5], m.hm[6], d.M / d.Leff);
        }
        return 0;
    }
    if (what == "gsrc")
    {
        // v2.0 diagnostic: where the partials come from. Q: the flow into the pipe over one period (the jet's source), dB re its
        // fundamental; T: Fletcher & Douglas's source 1 + tanh (a sin wt / b) for the swing a measured at the fundamental
        // (lip on the jet); O: the output's partials; R = O - Q - 40 log n: what the pipe's resonances add (the drive is
        // d/dt of the flow, the radiation d/dt of the mouth flow: n^2)
        auto dftDb = [] (const std::vector<double>& x, int nh, std::vector<double>& out) {
            const int N = (int) x.size();
            std::vector<double> a ((size_t) nh + 1);
            for (int h = 1; h <= nh; ++h)
            {
                double re = 0, im = 0;
                for (int i = 0; i < N; ++i) { re += x[(size_t) i] * std::cos (kTwoPi * h * i / N); im += x[(size_t) i] * std::sin (kTwoPi * h * i / N); }
                a[(size_t) h] = std::sqrt (re * re + im * im) * 2.0 / N;
            }
            out.assign ((size_t) nh + 1, 0.0);
            for (int h = 1; h <= nh; ++h) out[(size_t) h] = 20 * std::log10 (std::max (1e-12, a[(size_t) h] / a[1]));
            return a[1];
        };
        for (int k : { 36, 48, 60, 72, 84 })
            for (int vr = 0; vr < 2; ++vr)
            {
                Globals g = vr == 1 ? Globals() : stopGlobals (STOP_GEDACKT);
                ParamVec p;
                voiceParams (k, g, VoiceOverride {}, vr == 1 ? designPrinzipal (k) : designGedackt (k), p);
                Voice v;
                v.jetJitter = Engine::kJetJitter;
                v.servoEnabled = false;
                v.prepare (FS, 3);
                v.start (p, 0.0);
                const int n = (int) (3.0 * FS);
                std::vector<double> mouth ((size_t) n), L ((size_t) n), R ((size_t) n), out ((size_t) n);
                for (int i = 0; i < n; i += 4096) { const int m = std::min (4096, n - i); v.render (m, L.data() + i, R.data() + i, mouth.data() + i); }
                for (int i = 0; i < n; ++i) out[(size_t) i] = L[(size_t) i] + R[(size_t) i];
                const Metrics mo = analyze (out, FS, p[P_FTARGET], 1.5);
                v.requestCapture (FS / mo.f);
                std::vector<double> sk (8192), skL (8192), skR (8192);
                for (int it = 0; it < 40 && ! v.captureReady(); ++it) v.render (1024, skL.data(), skR.data(), sk.data());
                if (! v.captureReady()) continue;
                const PeriodCapture& c = v.capture();
                std::vector<double> q (K_SNAP), e (K_SNAP), Q, E;
                for (int i = 0; i < K_SNAP; ++i) { q[(size_t) i] = c.inflow[i]; e[(size_t) i] = c.eta[i]; }
                dftDb (q, 7, Q);
                const double a1 = dftDb (e, 7, E);
                std::vector<double> th (256), T;
                for (int i = 0; i < 256; ++i) th[(size_t) i] = 1.0 + std::tanh (a1 * std::sin (kTwoPi * i / 256.0));
                dftDb (th, 7, T);
                char nm[8];
                organName (k, nm);
                std::printf ("  %-4s %-9s swing a1 = %.2f b\n", nm, vr == 1 ? "Prinzipal" : "Gedackt", a1);
                std::printf ("        n:      2      3      4      5      6      7\n");
                std::printf ("    Q source "); for (int h = 2; h <= 7; ++h) std::printf (" %6.1f", Q[(size_t) h]); std::printf ("\n");
                std::printf ("    T theory "); for (int h = 2; h <= 7; ++h) std::printf (" %6.1f", T[(size_t) h]); std::printf ("\n");
                std::printf ("    O output "); for (int h = 2; h <= 7; ++h) std::printf (" %6.1f", mo.hm[h - 1]); std::printf ("\n");
                std::printf ("    R pipe   "); for (int h = 2; h <= 7; ++h) std::printf (" %6.1f", mo.hm[h - 1] - Q[(size_t) h] - 40 * std::log10 ((double) h)); std::printf ("\n");
            }
        return 0;
    }
    if (what == "gcut")
    {
        // v2.0 diagnostic: the Gedackt with Janke's cut-up rule (as built) and with Dickson's own, a third of the mouth's
        // width (argv[2]: wall-loss factor on top of the wood's, default 1): cut-up, Ising, the jet's swing at the lip
        // (fundamental of eta, in b), pitch, level, partials
        const double lossX = argc > 2 ? std::atof (argv[2]) : 1.0;
        std::printf ("  key  rule    cut [mm]  H/W  Ising  swing/b  cents  regime level |   H2    H3    H4    H5    H6    H7\n");
        for (int k : { 24, 30, 36, 42, 48, 60, 72, 84, 96 })
            for (int vr = 0; vr < 4; ++vr)
            {
                const PipeDesign d = designGedackt (k);
                Globals g = stopGlobals (STOP_GEDACKT);
                g.loss *= lossX;
                if (vr == 1) g.cutup = d.mouthW / 3.0 / d.cutup;
                if (vr == 2) g.arch = 1.0;     // Janke's height, arched (Klais's Gedackt 8')
                if (vr == 3) g.y0b = 0.5;      // the lip off the jet's centre as before v2.0's y0b = 0
                const ParamVec p = gedacktPipe (k, g);
                Voice v;
                v.jetJitter = Engine::kJetJitter;
                v.servoEnabled = false;
                v.prepare (FS, 3);
                v.start (p, 0.0);
                const double dur = k < 40 ? 4.0 : 2.6;
                const int n = (int) (dur * FS);
                std::vector<double> mouth ((size_t) n), L ((size_t) n), R ((size_t) n), out ((size_t) n);
                for (int i = 0; i < n; i += 4096) { const int m = std::min (4096, n - i); v.render (m, L.data() + i, R.data() + i, mouth.data() + i); }
                for (int i = 0; i < n; ++i) out[(size_t) i] = L[(size_t) i] + R[(size_t) i];
                const Metrics mo = analyze (out, FS, p[P_FTARGET], dur - 1.0);
                double a1 = 0.0;
                if (std::isfinite (mo.f) && mo.f > 0)
                {
                    v.requestCapture (FS / mo.f);
                    std::vector<double> sk (8192), skL (8192), skR (8192);
                    for (int it = 0; it < 40 && ! v.captureReady(); ++it) v.render (1024, skL.data(), skR.data(), sk.data());
                    if (v.captureReady())
                    {
                        const PeriodCapture& c = v.capture();
                        double re = 0, im = 0;
                        for (int i = 0; i < K_SNAP; ++i) { re += c.eta[i] * std::cos (kTwoPi * i / K_SNAP); im += c.eta[i] * std::sin (kTwoPi * i / K_SNAP); }
                        a1 = std::sqrt (re * re + im * im) * 2.0 / K_SNAP;
                    }
                }
                const double cf = vr == 1 ? g.cutup : 1.0;
                char nm[8];
                organName (k, nm);
                std::printf ("  %-4s %-7s  %6.1f  %4.2f  %5.2f  %6.2f  %+6.1f  %5.2f  %5.1f | %5.1f %5.1f %5.1f %5.1f %5.1f %5.1f\n", nm,
                             vr == 1 ? "Dickson" : vr == 2 ? "arched" : vr == 3 ? "y0 0.5b" : "Janke", d.cutup * cf * 1e3, d.cutup * cf / d.mouthW, d.ising * std::pow (cf, -1.5), a1,
                             std::isfinite (mo.f) ? cents (mo.f, p[P_FTARGET]) : 999.0, mo.regime, mo.db,
                             mo.hm[1], mo.hm[2], mo.hm[3], mo.hm[4], mo.hm[5], mo.hm[6]);
            }
        return 0;
    }
    if (what == "gfitkey" && argc > 3)
    {
        // v2.0 diagnostic: per key (argv[2]..argv[3]) the factor (x Janke's rule) whose pipe, as the plugin builds it (no
        // ears, cut-up follow from its built resonance) and with its stopper set to its key, has the level B the user heard:
        // the distributed Gedackt (ears 100 %, its stopper table: give the old table's file in argv[4]) with the Cut-up
        // knob at the user's first factors (1.50 g..f#1, 1.67 g1..f#2, 1.71 g2..g3, 1.58 g#3..c4). Regula falsi on the
        // level, each try tuned (kcal from the last try, by steps of the measured cents)
        std::vector<double> kOld;
        if (argc > 4)
        {
            std::FILE* f = std::fopen (argv[4], "r");
            char line[256];
            while (f && std::fgets (line, sizeof line, f)) { double x; int key; if (std::sscanf (line, " %lf, // %d", &x, &key) == 2) kOld.push_back (x); }
            if (f) std::fclose (f);
        }
        if (kOld.size() != 73) { std::printf ("  the old stopper table (73 factors) is needed in argv[4]\n"); return 1; }
        auto knob = [] (int k) { return k <= 54 ? 1.00 : k <= 66 ? 1.50 : k <= 78 ? 1.67 : k <= 91 ? 1.71 : 1.58; };
        std::printf ("  key  B dB   factor  kcal     cents   dB    (tries)\n");
        for (int k = std::atoi (argv[2]); k <= std::atoi (argv[3]); ++k)
        {
            const double dur = k < 40 ? 4.0 : 2.6;
            double B;
            {
                PipeDesign d = designGedackt (k);
                d.cutup /= gedacktCutVoicing (k);
                d.M = mouthCorrection (true, d.d, d.mouthW, d.cutup, 1.0);
                const double kc = kOld[(size_t) (k - 24)];
                d.Leff = d.c / (4.0 * d.f0) * kc;
                d.lPhys = d.Leff - d.M;
                Globals g = stopGlobals (STOP_GEDACKT);
                g.ears = 1.0; g.cutup = knob (k);
                ParamVec p;
                voiceParams (k, g, VoiceOverride {}, d, p);
                p[P_KCAL] = kc; p[P_FDESIGN] = d.f0 / kc;
                B = gedacktTone (p, dur, dur - 1.0).db;
            }
            double kcWarm = 1.0;
            int tries = 0;
            auto level = [&] (double F, double& kcOut, double& cOut) {
                double kc = kcWarm;
                Metrics m;
                for (int it = 0; it < 10; ++it)
                {
                    PipeDesign d = designGedackt (k);
                    d.cutup *= F / gedacktCutVoicing (k);
                    d.M = mouthCorrection (true, d.d, d.mouthW, d.cutup, 0.0);
                    d.Leff = d.c / (4.0 * d.f0) * kc;
                    d.lPhys = d.Leff - d.M;
                    Globals g = stopGlobals (STOP_GEDACKT);
                    ParamVec p;
                    voiceParams (k, g, VoiceOverride {}, d, p);
                    p[P_KCAL] = kc; p[P_FDESIGN] = d.f0 / kc;
                    m = gedacktTone (p, dur, dur - 1.0);
                    ++tries;
                    if (! std::isfinite (m.f)) break;
                    const double c = cents (m.f, midiToHz (k));
                    if (std::abs (c) < 0.3 || std::abs (c) > 600.0) break;
                    kc *= std::exp2 (c / 1200.0);
                }
                cOut = std::isfinite (m.f) ? cents (m.f, midiToHz (k)) : 9999.0;
                kcOut = kc;
                if (std::abs (cOut) < 5.0) kcWarm = kc;
                return std::abs (cOut) < 5.0 ? m.db : 0.0;      // (a pipe not on its key counts as silent)
            };
            // bracket: the level falls as the cut-up rises
            double kc, c;
            double F0 = gedacktCutVoicing (k), L0 = level (F0, kc, c);
            double Fa = F0, La = L0, Fb = F0, Lb = L0;
            for (int s = 0; s < 12 && ! ((La - B) >= 0 && (Lb - B) <= 0); ++s)
            {
                if (La - B < 0) { Fb = Fa; Lb = La; Fa -= 0.03; La = level (Fa, kc, c); }
                else { Fa = Fb; La = Lb; Fb += 0.03; Lb = level (Fb, kc, c); }
            }
            double F = F0, L = L0;
            for (int it = 0; it < 12; ++it)
            {
                F = (La - Lb) > 1e-9 ? Fa + (La - B) / (La - Lb) * (Fb - Fa) : 0.5 * (Fa + Fb);
                F = std::clamp (F, Fa + 0.1 * (Fb - Fa), Fb - 0.1 * (Fb - Fa));
                L = level (F, kc, c);
                if (std::abs (L - B) < 0.1 || Fb - Fa < 0.002) break;
                if (L > B) { Fa = F; La = L; } else { Fb = F; Lb = L; }
            }
            L = level (F, kc, c);
            std::printf ("  %3d  %5.1f   %.3f  %.5f %+6.1f  %5.1f   (%d)\n", k, B, F, kc, c, L, tries);
            std::fflush (stdout);
        }
        return 0;
    }
    if (what == "gfit" && argc > 3)
    {
        // v2.0 diagnostic: the user's voicing, fitted to the level they heard with the knob. For the keys argv[2]..argv[3]
        // (every argv[4]-th, default 1): B = the distributed Gedackt (no voicing, ears 100 %, the stopper table compiled in)
        // with the Cut-up knob at the voicing's factor; then, for each factor in argv[5..] (absolute, x Janke's rule), the
        // pipe as the plugin will build it (that cut-up, no ears, cut-up follow from its built resonance) with its stopper
        // set so that it speaks on its key (kcal, from 1 by steps of the measured cents). Mean levels over the keys.
        const int lo = std::atoi (argv[2]), hi = std::atoi (argv[3]);
        const int step = argc > 4 ? std::max (1, std::atoi (argv[4])) : 1;
        std::vector<double> facs;
        for (int i = 5; i < argc; ++i) facs.push_back (std::atof (argv[i]));
        auto built = [] (int k, double F, double kc, ParamVec& p) {
            PipeDesign d = designGedackt (k);
            d.cutup *= F / gedacktCutVoicing (k);
            d.M = mouthCorrection (true, d.d, d.mouthW, d.cutup, 0.0);
            d.Leff = d.c / (4.0 * d.f0) * kc;
            d.lPhys = d.Leff - d.M;
            Globals g = stopGlobals (STOP_GEDACKT);
            voiceParams (k, g, VoiceOverride {}, d, p);
            p[P_KCAL] = kc;
            p[P_FDESIGN] = d.f0 / kc;
        };
        double sumB = 0;
        std::vector<double> sumF (facs.size(), 0.0);
        int n = 0;
        std::printf ("  key   B: cents  dB ");
        for (double F : facs) std::printf ("|  x%.3f: kcal   cents   dB ", F);
        std::printf ("\n");
        for (int k = lo; k <= hi; k += step)
        {
            const double dur = k < 40 ? 4.0 : 2.6;
            Metrics mb;
            {
                PipeDesign d = designGedackt (k);
                const double fac = gedacktCutVoicing (k);
                d.cutup /= fac;
                d.M = mouthCorrection (true, d.d, d.mouthW, d.cutup, 1.0);
                const double kc = kcalForStop (1, k);
                d.Leff = d.c / (4.0 * d.f0) * kc;
                d.lPhys = d.Leff - d.M;
                Globals g = stopGlobals (STOP_GEDACKT);
                g.ears = 1.0; g.cutup = fac;
                ParamVec p;
                voiceParams (k, g, VoiceOverride {}, d, p);
                p[P_KCAL] = kc; p[P_FDESIGN] = d.f0 / kc;
                mb = gedacktTone (p, dur, dur - 1.0);
            }
            std::printf ("  %3d  %+7.1f %5.1f ", k, cents (mb.f, midiToHz (k)), mb.db);
            sumB += mb.db;
            for (size_t i = 0; i < facs.size(); ++i)
            {
                double kc = 1.0;
                ParamVec p;
                built (k, facs[i], kc, p);
                Metrics m = gedacktTone (p, dur, dur - 1.0);
                for (int it = 0; it < 8; ++it)
                {
                    if (! std::isfinite (m.f)) break;
                    const double c = cents (m.f, midiToHz (k));
                    if (std::abs (c) < 0.5 || std::abs (c) > 600.0) break;
                    kc *= std::exp2 (c / 1200.0);
                    built (k, facs[i], kc, p);
                    m = gedacktTone (p, dur, dur - 1.0);
                }
                std::printf ("|        %.4f %+7.1f %5.1f ", kc, std::isfinite (m.f) ? cents (m.f, midiToHz (k)) : 9999.0, m.db);
                sumF[i] += m.db;
            }
            std::printf ("\n");
            ++n;
        }
        std::printf ("  mean  B %.2f dB", sumB / n);
        for (size_t i = 0; i < facs.size(); ++i) std::printf ("  |  x%.3f %.2f dB", facs[i], sumF[i] / n);
        std::printf ("\n");
        return 0;
    }
    if (what == "gvoicetab" && argc > 2)
    {
        // v2.0 diagnostic: every key, three Gedackts (pitch lock off, reverb none, the level of gedacktTone: dB SPL at 1 m):
        //   A  as distributed before the voicing: cut-up x1.00, ears 100 %, the stopper table compiled in now
        //   B  A with the Cut-up knob at the voicing's factor (the stopper left where it was)
        //   C  the voiced design (factor built in, no ears) with the stopper table of argv[2] (kcalgen of that design)
        std::vector<double> kc;
        {
            std::FILE* f = std::fopen (argv[2], "r");
            if (! f) { std::printf ("  cannot read %s\n", argv[2]); return 1; }
            char line[256];
            while (std::fgets (line, sizeof line, f))
            {
                double x; int key;
                if (std::sscanf (line, " %lf, // %d", &x, &key) == 2) kc.push_back (x);
            }
            std::fclose (f);
        }
        if (kc.size() != 73) { std::printf ("  %zu factors in %s (73 wanted)\n", kc.size(), argv[2]); return 1; }
        //   D  B with its stopper moved until it speaks on its key (the body lengthened or shortened, 3 steps), ears 100 %
        //   E  the voiced design (no ears) with its stopper moved the same way (instead of kcalgen's table)
        std::printf ("  key  name  factor |  A: cents regime   dB |  B: cents regime   dB |  C: cents regime   dB |  D: cents regime   dB |  E: cents regime   dB\n");
        for (int k = 24; k <= 96; ++k)
        {
            const double fac = gedacktCutVoicing (k);
            const double dur = k < 40 ? 4.0 : 2.6;
            Metrics m[5];
            for (int s = 0; s < 5; ++s)
            {
                if (s >= 3)
                {
                    PipeDesign d = designGedackt (k);
                    Globals g = stopGlobals (STOP_GEDACKT);
                    double kcal = kcalForStop (1, k);
                    if (s == 3)
                    {
                        d.cutup /= fac;
                        d.M = mouthCorrection (true, d.d, d.mouthW, d.cutup, 1.0);
                        g.ears = 1.0;
                        g.cutup = fac;
                    }
                    else kcal = 1.0;
                    d.Leff = d.c / (4.0 * d.f0) * kcal;
                    d.lPhys = d.Leff - d.M;
                    ParamVec p;
                    voiceParams (k, g, VoiceOverride {}, d, p);
                    p[P_KCAL] = kcal;
                    p[P_FDESIGN] = d.f0 / kcal;
                    Metrics mm = gedacktTone (p, dur, dur - 1.0);
                    for (int it = 0; it < 3 && std::isfinite (mm.f) && std::abs (mm.regime - 1.0) < 0.06; ++it)
                    {
                        const double c = cents (mm.f, midiToHz (k));
                        if (std::abs (c) < 0.5) break;
                        p[P_LPHYS] += d.c / (4.0 * mm.f) * (std::exp2 (c / 1200.0) - 1.0);
                        mm = gedacktTone (p, dur, dur - 1.0);
                    }
                    m[s] = mm;
                    continue;
                }
                PipeDesign d = designGedackt (k);
                Globals g = stopGlobals (STOP_GEDACKT);
                double kcal = kcalForStop (1, k);
                if (s < 2)
                {
                    d.cutup /= fac;
                    d.M = mouthCorrection (true, d.d, d.mouthW, d.cutup, 1.0);
                    g.ears = 1.0;
                    if (s == 1) g.cutup = fac;
                }
                else kcal = kc[(size_t) (k - 24)];
                d.Leff = d.c / (4.0 * d.f0) * kcal;
                d.lPhys = d.Leff - d.M;
                ParamVec p;
                voiceParams (k, g, VoiceOverride {}, d, p);
                p[P_KCAL] = kcal;
                p[P_FDESIGN] = d.f0 / kcal;
                m[s] = gedacktTone (p, dur, dur - 1.0);
            }
            char nm[8];
            organName (k, nm);
            std::printf ("  %3d  %-4s  %5.2f ", k, nm, fac);
            for (int s = 0; s < 5; ++s)
                std::printf (" | %+7.1f  %5.2f %5.1f", std::isfinite (m[s].f) ? cents (m[s].f, midiToHz (k)) : 9999.0, m[s].regime, m[s].db);
            std::printf ("\n");
        }
        return 0;
    }
    if (what == "gonset")
    {
        // v2.0 diagnostic: the pitch at the speech of a note through the engine (reverb off), as played (pitch lock as the
        // stop's default): cents against the settled pitch (1.0-1.5 s) in 20 ms windows every 20 ms, for velocities 30, 64,
        // 100, 127 (argv[2]: key, argv[3]: stop 0/1, argv[4]: legato from this key, held 1 s, -1 none)
        const int k = argc > 2 ? std::atoi (argv[2]) : 60;
        const int st = argc > 3 ? std::atoi (argv[3]) : 1;
        const int from = argc > 4 ? std::atoi (argv[4]) : -1;
        const double fs = 48000.0;
        auto pitchAt = [&] (const std::vector<float>& x, size_t c, double f0) {
            // normalised autocorrelation over 20 ms around c, lag searched +-6 % of the period, parabolic peak
            const int W = 960, T0 = (int) std::lround (fs / f0);
            const int lo = (int) (T0 * 0.94), hi = (int) (T0 * 1.06) + 1;
            if (c < (size_t) (W / 2 + hi + 2) || c + W / 2 >= x.size()) return 0.0;
            const size_t a = c - W / 2;
            std::vector<double> r ((size_t) (hi + 2), 0.0);
            double e0 = 0;
            for (int i = 0; i < W; ++i) e0 += (double) x[a + i] * x[a + i];
            if (e0 <= 1e-20) return 0.0;
            int best = lo;
            for (int L = lo - 1; L <= hi + 1; ++L)
            {
                double s = 0, e1 = 0;
                for (int i = 0; i < W; ++i) { s += (double) x[a + i] * x[a + i - L]; e1 += (double) x[a + i - L] * x[a + i - L]; }
                r[(size_t) L] = s / std::sqrt (e0 * e1 + 1e-30);
            }
            for (int L = lo; L <= hi; ++L) if (r[(size_t) L] > r[(size_t) best]) best = L;
            const double y0 = r[(size_t) best - 1], y1 = r[(size_t) best], y2 = r[(size_t) best + 1];
            const double den = y0 - 2 * y1 + y2;
            const double d = den < 0 ? 0.5 * (y0 - y2) / den : 0.0;
            return r[(size_t) best] > 0.5 ? fs / (best + d) : 0.0;
        };
        Globals G = st == 1 ? stopGlobals (STOP_GEDACKT) : Globals();
        G.reverb = 0;
        // argv[5]: 1 no starting-vortex pulse, 2 jet feedback x0.5 (a less saturated jet), 3 turbulence 0, 4 velocity sensitivity 0
        const int var = argc > 5 ? std::atoi (argv[5]) : 0;
        if (var == 1) G.kick = 0.0;
        if (var == 2) G.jetGain = 0.5;
        if (var == 3) G.noise = 0.0;
        if (var == 4) G.velSens = 0.0;
        const double f0 = midiToHz (k);
        if (from == -2)
        {
            // the release: held 1.5 s, then the key up; pitch and level after it, every 10 ms
            std::printf ("  key %d, %s: release, cents against the held pitch / level dB re held, from the key up\n", k, st == 1 ? "Gedackt" : "Prinzipal");
            for (int vel : { 64 })
            {
                auto e = std::make_unique<Engine>();
                e->prepare (fs, 512);
                e->setGlobals (G);
                std::vector<float> L (480), R (480), out;
                e->noteOn (1, k, vel);
                for (int b = 0; b < 150; ++b) { e->process (L.data(), R.data(), 480); for (int i = 0; i < 480; ++i) out.push_back (L[(size_t) i] + R[(size_t) i]); }
                e->noteOff (1);
                for (int b = 0; b < 60; ++b) { e->process (L.data(), R.data(), 480); for (int i = 0; i < 480; ++i) out.push_back (L[(size_t) i] + R[(size_t) i]); }
                std::vector<double> fsx;
                for (size_t c = 48000; c < 72000; c += 480) { const double f = pitchAt (out, c, f0); if (f > 0) fsx.push_back (f); }
                std::sort (fsx.begin(), fsx.end());
                const double fset = fsx.empty() ? f0 : fsx[fsx.size() / 2];
                double eset = 0;
                for (size_t i = 48000; i < 72000; ++i) eset += (double) out[i] * out[i];
                eset /= 24000;
                for (int t = 0; t <= 400; t += 20)
                {
                    const size_t c = 72000 + (size_t) t * 48;
                    const double f = pitchAt (out, c, fset);
                    double ee = 0;
                    for (size_t i = c - 480; i < c + 480 && i < out.size(); ++i) ee += (double) out[i] * out[i];
                    std::printf ("   %3d ms  %s  %6.1f dB\n", t, f > 0 ? (std::to_string ((int) std::lround (1200 * std::log2 (f / fset))) + " c").c_str() : "   -  ",
                                 10 * std::log10 (ee / 960 / (eset + 1e-30) + 1e-12));
                }
            }
            return 0;
        }
        std::printf ("  key %d, %s%s: cents against the settled pitch (level dB re settled below), from the key\n", k,
                     st == 1 ? "Gedackt" : "Prinzipal", from >= 0 ? (", legato from " + std::to_string (from)).c_str() : "");
        std::printf ("   ms  ");
        for (int t = 10; t <= 300; t += 20) std::printf (" %5d", t);
        std::printf ("    settled\n");
        for (int vel : { 30, 64, 100, 127 })
        {
            auto e = std::make_unique<Engine>();
            e->prepare (fs, 512);
            e->setGlobals (G);
            std::vector<float> L (480), R (480), out;
            if (from >= 0)
            {
                e->noteOn (2, from, vel);
                for (int b = 0; b < 100; ++b) e->process (L.data(), R.data(), 480);
            }
            e->noteOn (1, k, vel);
            if (from >= 0) e->noteOff (2);
            std::vector<double> pfs;      // the focus pipe's foot pressure at the end of each 10 ms block
            auto tel = std::make_unique<Telemetry>();
            for (int b = 0; b < 160; ++b)
            {
                e->process (L.data(), R.data(), 480);
                for (int i = 0; i < 480; ++i) out.push_back (L[(size_t) i] + R[(size_t) i]);
                e->fillTelemetry (*tel);
                pfs.push_back (tel->pf);
            }
            if (var == 9)
            {
                // the fundamental alone: an RBJ band-pass at the key (Q 4), forwards then backwards (no phase)
                const double w = kTwoPi * f0 / fs, al = std::sin (w) / 8.0, a0 = 1 + al;
                const double b0 = al / a0, a1 = -2 * std::cos (w) / a0, a2 = (1 - al) / a0;
                for (int pass = 0; pass < 2; ++pass)
                {
                    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
                    auto run = [&] (float& s) { const double x = s, y = b0 * x - b0 * x2 - a1 * y1 - a2 * y2; x2 = x1; x1 = x; y2 = y1; y1 = y; s = (float) y; };
                    if (pass == 0) for (auto& s : out) run (s); else for (auto it = out.rbegin(); it != out.rend(); ++it) run (*it);
                }
            }
            std::vector<double> fsx;
            for (size_t c = 48000; c < 72000; c += 480) { const double f = pitchAt (out, c, f0); if (f > 0) fsx.push_back (f); }
            std::sort (fsx.begin(), fsx.end());
            const double fset = fsx.empty() ? f0 : fsx[fsx.size() / 2];
            double eset = 0;
            for (size_t i = 48000; i < 72000; ++i) eset += (double) out[i] * out[i];
            eset /= 24000;
            std::printf ("  v%3d ", vel);
            for (int t = 10; t <= 300; t += 20)
            {
                const double f = pitchAt (out, (size_t) (t * 48), fset);
                if (f > 0) std::printf (" %+5.0f", 1200 * std::log2 (f / fset)); else std::printf ("     -");
            }
            std::printf ("    %+.1f c vs key\n       ", 1200 * std::log2 (fset / f0));
            for (int t = 10; t <= 300; t += 20)
            {
                double ee = 0;
                for (int i = t * 48 - 480; i < t * 48 + 480; ++i) if (i >= 0) ee += (double) out[(size_t) i] * out[(size_t) i];
                std::printf (" %5.0f", 10 * std::log10 (ee / 960 / (eset + 1e-30) + 1e-12));
            }
            std::printf ("    (level)\n       ");
            for (int t = 10; t <= 300; t += 20) std::printf (" %5.0f", 100.0 * pfs[(size_t) (t / 10)] / (pfs.back() + 1e-30));
            std::printf ("    (foot pressure %% of settled, %.1f mmWS)\n", pfs.back() / MMWS);
        }
        return 0;
    }
    if (what == "gcand")
    {
        // v2.0 diagnostic: how many wall modes lie below the top of hearing (the candidates, capped at 1150) and how many are
        // kept (64), per key, for the Gedackt's board and the Prinzipal's shell, with every head
        std::printf ("  key   Gedackt board: candidates/kept per head          | Prinzipal shell: candidates/kept per head\n");
        for (int k = 24; k <= 96; k += 6)
        {
            std::printf ("  %3d ", k);
            for (int st = 1; st >= 0; --st)
            {
                const PipeDesign d = st == 1 ? designGedackt (k) : designPrinzipal (k);
                for (int hd = 0; hd < kNumHeads; ++hd)
                {
                    StrikeParams s;
                    s.L = d.lPhys; s.d = d.d; s.head = hd; s.box = st == 1; s.width = d.mouthW; s.depth = d.depth;
                    s.morph = st == 1 ? 1.0 : 0.0;
                    MalletModel m;
                    m.prepare (48000.0);
                    m.setParams (s);
                    m.strike (1.0);
                    std::printf (" %4d/%2d", m.candidateCount(), m.modeCount());
                }
                std::printf (st == 1 ? "  |" : "\n");
            }
        }
        return 0;
    }
    if (what == "gmal")
    {
        // v2.0 diagnostic: the Gedackt in MALLET mode through the engine (reverb off), one stroke per key (argv[2]: head,
        // default 4 = yarn; argv[3]: velocity, default 100; argv[4]: a key to follow in time instead). Per key: the wall's
        // modes (count, lowest), the contact, the level and a brightness (rms of the first difference over rms, as Hz)
        const int head = argc > 2 ? std::atoi (argv[2]) : 4;
        const int vel = argc > 3 ? std::atoi (argv[3]) : 100;
        const int follow = argc > 4 ? std::atoi (argv[4]) : -1;
        const bool prinz = argc > 5 && std::atoi (argv[5]) == 0;       // argv[5] 0: the Prinzipal
        auto e = std::make_unique<Engine>();
        e->prepare (48000, 512);
        Globals G = prinz ? Globals() : stopGlobals (STOP_GEDACKT);
        G.excite = 1; G.head = head; G.reverb = 0;
        e->setGlobals (G);
        std::vector<float> L (512), R (512);
        auto Telem = std::make_unique<Telemetry>();
        auto bright = [] (const std::vector<float>& x, size_t a, size_t b) {
            double s = 0, d = 0;
            for (size_t i = a + 1; i < b; ++i) { s += (double) x[i] * x[i]; d += (double) (x[i] - x[i - 1]) * (x[i] - x[i - 1]); }
            return std::pair<double, double> { 10 * std::log10 (s / (b - a) + 1e-30), s > 0 ? 48000.0 / kTwoPi * std::sqrt (d / s) : 0.0 };
        };
        int id = 1;
        if (follow < 0)
            std::printf ("  key  modes active lowest[Hz] contacts tC[ms] Fpk[N] |  0-50 ms dB  Hz | 50-300 ms dB  Hz | 0.3-1 s dB  Hz\n");
        for (int k = (follow < 0 ? 24 : follow); k <= (follow < 0 ? 96 : follow); k += 2)
        {
            e->allOff (true);
            std::vector<float> out;
            for (int b = 0; b < 20; ++b) e->process (L.data(), R.data(), 512);
            e->noteOn (id, k, vel);
            int nModes = -1, nAct = -1, contacts = 0;
            double lowest = 0, tc = 0, fpk = 0;
            for (int b = 0; b < 94; ++b)
            {
                e->process (L.data(), R.data(), 512);
                out.insert (out.end(), L.begin(), L.end());
                if (b == 4)
                {
                    e->fillTelemetry (*Telem);
                    nModes = Telem->mallet.nModes; nAct = Telem->mallet.nActive; contacts = Telem->mallet.contacts;
                    tc = Telem->mallet.tContact * 1e3; fpk = Telem->mallet.fPeak;
                    lowest = 1e9;
                    for (int i = 0; i < Telem->mallet.nShown; ++i) lowest = std::min (lowest, (double) Telem->mallet.modes[i].f);
                    if (Telem->mallet.nShown == 0) lowest = 0;
                }
            }
            e->noteOff (id);
            ++id;
            if (follow >= 0)
            {
                std::printf ("  key %d: modes %d, active %d, lowest shown %.0f Hz, contacts %d, contact %.2f ms, peak %.2f N\n", k, nModes, nAct, lowest, contacts, tc, fpk);
                for (size_t a = 0; a + 1200 <= out.size(); a += 1200)
                {
                    const auto r = bright (out, a, a + 1200);
                    std::printf ("    %4.0f ms  %6.1f dB  %6.0f Hz\n", a / 48.0, r.first, r.second);
                }
                break;
            }
            const auto a = bright (out, 0, 2400), b2 = bright (out, 2400, 14400), c = bright (out, 14400, 48000);
            char nm[8];
            organName (k, nm);
            std::printf ("  %-4s %5d %6d %9.0f %8d %6.2f %6.2f | %7.1f %5.0f | %7.1f %5.0f | %7.1f %5.0f\n", nm, nModes, nAct, lowest, contacts, tc, fpk,
                         a.first, a.second, b2.first, b2.second, c.first, c.second);
        }
        return 0;
    }
    if (what == "gflue")
    {
        // v2.0 diagnostic: the Gedackt (straight cut-up) with the Prinzipal's flue rule (as built) and with the flue of real
        // wooden pipes, h = 1.0 mm (W / 47 mm)^0.309 through BDO's 47 x 60 mm pipe (about 1 mm) and Fischer et al.'s
        // Schuke stopped pipe (9 mm, 0.6 mm); the toe follows (same foot pressure). Flue, Re_h, Ising, swing, noise, partials
        std::printf ("  key  flue      h [mm]  Re_h   Ising  swing/b  cents  level  noise |   H2    H3    H4    H5    H6    H7\n");
        for (int k : { 24, 30, 36, 42, 48, 54, 60, 72, 84, 96 })
            for (int vr = 0; vr < 2; ++vr)
            {
                PipeDesign d = designGedackt (k);
                if (vr == 1)
                {
                    const double x = 1.0e-3 * std::pow (d.mouthW / 0.047, 0.309) / d.flue;
                    d.flue *= x;
                    d.toe *= std::sqrt (x);
                    d.footVol = kPi / 12.0 * d.footLen * (d.d * d.d + d.d * d.toe + d.toe * d.toe);
                    d.ising *= std::sqrt (x);
                }
                const Globals g = stopGlobals (STOP_GEDACKT);
                ParamVec p;
                voiceParams (k, g, VoiceOverride {}, d, p);
                Voice v;
                v.jetJitter = Engine::kJetJitter;
                v.servoEnabled = false;
                v.prepare (FS, 3);
                v.start (p, 0.0);
                const double dur = k < 40 ? 4.0 : 2.6;
                const int n = (int) (dur * FS);
                std::vector<double> mouth ((size_t) n), L ((size_t) n), R ((size_t) n), out ((size_t) n);
                for (int i = 0; i < n; i += 4096) { const int m = std::min (4096, n - i); v.render (m, L.data() + i, R.data() + i, mouth.data() + i); }
                for (int i = 0; i < n; ++i) out[(size_t) i] = L[(size_t) i] + R[(size_t) i];
                const Metrics mo = analyze (out, FS, p[P_FTARGET], dur - 1.0);
                // noise: what one period later does not repeat
                const double T = FS / (std::isfinite (mo.f) && mo.f > 0 ? mo.f : p[P_FTARGET]);
                double es = 0, en = 0;
                const int i0 = (int) ((dur - 1.0) * FS);
                for (int i = i0; i < n; ++i)
                {
                    const double pos = i - T;
                    const int j = (int) pos;
                    const double fr = pos - j;
                    const double xd = out[(size_t) j] * (1 - fr) + out[(size_t) j + 1] * fr;
                    es += out[(size_t) i] * out[(size_t) i]; en += 0.5 * (out[(size_t) i] - xd) * (out[(size_t) i] - xd);
                }
                double a1 = 0.0;
                v.requestCapture (T);
                std::vector<double> sk (8192), skL (8192), skR (8192);
                for (int it = 0; it < 40 && ! v.captureReady(); ++it) v.render (1024, skL.data(), skR.data(), sk.data());
                if (v.captureReady())
                {
                    const PeriodCapture& c = v.capture();
                    double re = 0, im = 0;
                    for (int i = 0; i < K_SNAP; ++i) { re += c.eta[i] * std::cos (kTwoPi * i / K_SNAP); im += c.eta[i] * std::sin (kTwoPi * i / K_SNAP); }
                    a1 = std::sqrt (re * re + im * im) * 2.0 / K_SNAP;
                }
                const double Re = d.Uj * d.flue * d.rho / d.mu;
                char nm[8];
                organName (k, nm);
                std::printf ("  %-4s %-8s  %5.2f  %5.0f  %5.2f  %6.2f  %+6.1f  %5.1f  %5.1f | %5.1f %5.1f %5.1f %5.1f %5.1f %5.1f\n", nm,
                             vr == 1 ? "real wood" : "as built", d.flue * 1e3, Re, d.ising, a1,
                             std::isfinite (mo.f) ? cents (mo.f, p[P_FTARGET]) : 999.0, mo.db, 10 * std::log10 (en / es),
                             mo.hm[1], mo.hm[2], mo.hm[3], mo.hm[4], mo.hm[5], mo.hm[6]);
            }
        return 0;
    }
    if (what == "gjet")
    {
        // v2.0 diagnostic: the jet at the lip over one period (peak |eta - y0| / b, the share of the period the flow is
        // switched fully in or out) and the noise (what one period later does not repeat: var(x(t) - x(t - T)) / 2 against
        // the signal's power), the Gedackt as built and with Turbulence x4, and the Prinzipal
        std::printf ("  key  pipe              swing/b  sat%%   noise dB re signal   level\n");
        for (int k : { 36, 48, 60, 72, 84 })
            for (int vr = 0; vr < 3; ++vr)
            {
                Globals g = vr == 2 ? Globals() : stopGlobals (STOP_GEDACKT);
                if (vr == 1) g.noise *= 4.0;
                ParamVec p;
                voiceParams (k, g, VoiceOverride {}, vr == 2 ? designPrinzipal (k) : designGedackt (k), p);
                Voice v;
                v.jetJitter = Engine::kJetJitter;
                v.servoEnabled = false;
                v.prepare (FS, 3);
                v.start (p, 0.0);
                const int n = (int) (3.0 * FS);
                std::vector<double> mouth ((size_t) n), L ((size_t) n), R ((size_t) n);
                for (int i = 0; i < n; i += 4096) { const int m = std::min (4096, n - i); v.render (m, L.data() + i, R.data() + i, mouth.data() + i); }
                const double T = FS / p[P_FTARGET];
                v.requestCapture (T);
                std::vector<double> sk (8192), skL (8192), skR (8192);
                for (int it = 0; it < 40 && ! v.captureReady(); ++it) v.render (1024, skL.data(), skR.data(), sk.data());
                double sw = 0, sat = 0;
                int ks = 0;
                if (v.captureReady())
                {
                    const PeriodCapture& c = v.capture();
                    for (int i = 0; i < (int) (sizeof (c.eta) / sizeof (c.eta[0])); ++i)
                    {
                        sw = std::max (sw, (double) std::abs (c.eta[i]));
                        if (c.inflow[i] < 0.05f || c.inflow[i] > 0.95f) sat += 1;
                        ++ks;
                    }
                }
                double es = 0, en = 0;
                const int i0 = (int) (1.5 * FS);
                for (int i = i0; i < n; ++i)
                {
                    const double x = L[(size_t) i] + R[(size_t) i];
                    const double pos = i - T;
                    const int j = (int) pos;
                    const double fr = pos - j;
                    const double xd = (L[(size_t) j] + R[(size_t) j]) * (1 - fr) + (L[(size_t) j + 1] + R[(size_t) j + 1]) * fr;
                    es += x * x; en += 0.5 * (x - xd) * (x - xd);
                }
                const double lev = 10 * std::log10 (es / (n - i0) / 4e-10);
                std::printf ("  %3d  %-16s  %6.1f  %4.0f   %8.1f            %5.1f\n", k, vr == 0 ? "Gedackt" : vr == 1 ? "Gedackt turb x4" : "Prinzipal",
                             sw, ks ? 100.0 * sat / ks : 0.0, 10 * std::log10 (en / es), lev);
            }
        return 0;
    }
    if (what == "gtimbre")
    {
        // the Gedackt's tone colour: partials 1..9 (dB re the fundamental) of the mouth signal and of the voice's output
        // (L + R, what the plugin plays), as built and with one knob moved; the Prinzipal for comparison (argv[2]: key)
        const int k = argc > 2 ? std::atoi (argv[2]) : 60;
        const Globals G0 = stopGlobals (STOP_GEDACKT);
        struct V { const char* nm; void (*f) (Globals&); };
        const V vars[] {
            { "as built", [] (Globals&) {} },
            { "cut-up +2.5 HT (Janke +7.5)", [] (Globals& g) { g.cutup = std::exp2 (2.5 / 16.0); } },
            { "cut-up x1.25", [] (Globals& g) { g.cutup = 1.25; } },
            { "cut-up x0.80", [] (Globals& g) { g.cutup = 0.80; } },
            { "labium offset 0 b", [] (Globals& g) { g.y0b = 0.0; } },
            { "labium offset 1 b", [] (Globals& g) { g.y0b = 1.0; } },
            { "wind x0.6", [] (Globals& g) { g.bellows = 0.6; } },
            { "wind x1.5", [] (Globals& g) { g.bellows = 1.5; } },
            { "jet feedback x0.5", [] (Globals& g) { g.jetGain = 0.5; } },
            { "mouth 1/4 circ. (narrower)", [] (Globals& g) { g.mouthFrac = 0.20; } },
            { "scale +4 HT (wider)", [] (Globals& g) { g.scaleHT += 4.0; } },
            { "scale -4 HT (narrower)", [] (Globals& g) { g.scaleHT -= 4.0; } },
            { "wall loss x2.5", [] (Globals& g) { g.loss = 2.5 * 1.27; } },
            { "ears 0", [] (Globals& g) { g.ears = 0; } },
            { "arch 100 %", [] (Globals& g) { g.arch = 1; } },
            { "turbulence 0", [] (Globals& g) { g.noise = 0; } },
            { "cut-up follow 0", [] (Globals& g) { g.vFollow = 0; } },
        };
        char nm[8];
        organName (k, nm);
        std::printf ("  key %s: partials dB re H1 (mouth signal | voice output L+R), level dB SPL\n", nm);
        auto row = [&] (const char* name, const ParamVec& p) {
            const int n = (int) (3.0 * FS);
            Voice v;
            v.jetJitter = Engine::kJetJitter;
            v.servoEnabled = false;
            v.prepare (FS, 3);
            v.start (p, 0.0);
            std::vector<double> mouth ((size_t) n), L ((size_t) n), R ((size_t) n), out ((size_t) n);
            for (int i = 0; i < n; i += 4096) { const int m = std::min (4096, n - i); v.render (m, L.data() + i, R.data() + i, mouth.data() + i); }
            for (int i = 0; i < n; ++i) out[(size_t) i] = L[(size_t) i] + R[(size_t) i];
            const Metrics a = analyze (mouth, FS, p[P_FTARGET], 2.0), b = analyze (out, FS, p[P_FTARGET], 2.0);
            if (std::getenv ("OKL_TOP"))
            {
                // what the top radiates: (L + R) = 1.2 (mouth + top) without a side hole
                std::vector<double> top ((size_t) n);
                for (int i = 0; i < n; ++i) top[(size_t) i] = out[(size_t) i] / 1.2 - mouth[(size_t) i];
                const Metrics c = analyze (top, FS, p[P_FTARGET], 2.0);
                double et = 0, em = 0;
                for (int i = (int) (2.0 * FS); i < n; ++i) { et += top[(size_t) i] * top[(size_t) i]; em += mouth[(size_t) i] * mouth[(size_t) i]; }
                std::printf ("  %-28s top %5.1f dB re mouth; top's partials re its H1:", name, 10 * std::log10 (et / em));
                for (int h = 1; h < 9; ++h) std::printf (" %5.1f", c.hm[h]);
                std::printf ("\n");
            }
            std::printf ("  %-28s %5.1f |", name, a.db);
            for (int h = 1; h < 9; ++h) std::printf (" %5.1f", a.hm[h]);
            std::printf (" |");
            for (int h = 1; h < 9; ++h) std::printf (" %5.1f", b.hm[h]);
            std::printf ("  x%.2f\n", a.regime);
        };
        std::printf ("  %-28s %5s | %5s %5s %5s %5s %5s %5s %5s %5s | (output: same)\n", "", "level", "H2", "H3", "H4", "H5", "H6", "H7", "H8", "H9");
        for (const auto& vr : vars)
        {
            Globals g = G0;
            vr.f (g);
            row (vr.nm, gedacktPipe (k, g));
        }
        {
            // the foot: its volume a tenth (a foot of 17 mm instead of 170 mm)
            ParamVec pf = gedacktPipe (k, G0);
            pf[P_FOOTVOL] *= 0.1;
            row ("foot volume x0.1", pf);
        }
        ParamVec pp;
        voiceParams (k, Globals(), VoiceOverride {}, designPrinzipal (k), pp);
        row ("Prinzipal (as built)", pp);
        if (std::getenv ("OKL_ATTACK"))
        {
            // the speech: foot pressure and mouth level in the first 60 ms, as built and with the foot a tenth
            for (double fv : { 1.0, 0.1 })
            {
                ParamVec p = gedacktPipe (k, G0);
                p[P_FOOTVOL] *= fv;
                const int n = (int) (0.06 * FS);
                Voice v;
                v.jetJitter = Engine::kJetJitter;
                v.servoEnabled = false;
                v.prepare (FS, 3);
                v.start (p, 0.0);
                std::vector<double> mouth ((size_t) n), L ((size_t) n), R ((size_t) n);
                std::printf ("  foot x%.1f: ", fv);
                for (int i = 0; i < n; i += (int) (0.005 * FS))
                {
                    const int m = (int) (0.005 * FS);
                    v.render (m, L.data() + i, R.data() + i, mouth.data() + i);
                    double e = 0;
                    for (int j = i; j < i + m; ++j) e += mouth[(size_t) j] * mouth[(size_t) j];
                    std::printf (" %2.0f ms pf %4.0f Pa %5.1f dB |", (i + m) / FS * 1e3, v.pf, 10 * std::log10 (e / m / 4e-10 + 1e-30));
                }
                std::printf ("\n");
            }
        }
        return 0;
    }
    if (what == "gdiag")
    {
        // the Gedackt's bass: which knob makes it speak (level, pitch, regime over the last second)
        const Globals G0 = stopGlobals (STOP_GEDACKT);
        struct V { const char* nm; void (*f) (Globals&); };
        const V vars[] {
            { "as built", [] (Globals&) {} },
            { "cut-up x0.897 (-2.5 HT)", [] (Globals& g) { g.cutup = 0.897; } },
            { "cut-up x0.80", [] (Globals& g) { g.cutup = 0.80; } },
            { "cut-up x0.65", [] (Globals& g) { g.cutup = 0.65; } },
            { "wind 75 mmWS", [] (Globals& g) { g.bellows = 75.0 * MMWS / 500.0; } },
            { "arch 100 %", [] (Globals& g) { g.arch = 1; } },
            { "ears 0", [] (Globals& g) { g.ears = 0; } },
            { "loss 0.5", [] (Globals& g) { g.loss = 0.5; } },
            { "jet feedback x2", [] (Globals& g) { g.jetGain = 2.0; } },
            { "scale +4 HT", [] (Globals& g) { g.scaleHT += 4.0; } },
        };
        const int keys[] { 24, 30, 36, 39, 48 };
        for (const auto& v : vars)
        {
            std::printf ("  %-26s", v.nm);
            for (int k : keys)
            {
                Globals g = G0;
                v.f (g);
                const ParamVec p = gedacktPipe (k, g);
                const Metrics m = gedacktTone (p, 4.0, 3.0);
                std::printf ("  %d: %5.1f dB %+5.0f c x%.2f", k, m.db, std::isfinite (m.f) ? cents (m.f, p[P_FTARGET] * std::max (0.5, std::round (2 * m.regime) / 2)) : 0.0, m.regime);
            }
            std::printf ("\n");
        }
        return 0;
    }
    if (what == "port" || what == "all") testPort();
    if (what == "derive" || what == "all") testDerive();
    if (what == "midi" || what == "all") testMidi();
    if (what == "pitchlock" || what == "all") testPitchLock();
    if (what == "wind" || what == "all") { testWind(); testWindEngine(); }
    if (what == "blocks" || what == "all") testBlocks();
    if (what == "tone" || what == "all") testTone();
    if (what == "cpu" || what == "all") testCpu();
    if (what == "stress" && argc > 2) stressVariant = std::atoi (argv[2]);
    if (what == "stress" && argc > 3) stressOnly = std::atoi (argv[3]);
    if (what == "stress" || what == "all") testStress();
    if (what == "mallet" || what == "all") testMallet();
    if (what == "room" || what == "all") testRoom();
    if (what == "lab" || what == "all") testLab();
    if (what == "simd" || what == "all") testSimd();
    if (what == "gedackt" || what == "all") testGedackt();
    if (what == "tuning" || what == "all") testTuning();
    std::printf ("\n%s (%d failed checks)\n", g_fail ? "FAILED" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
