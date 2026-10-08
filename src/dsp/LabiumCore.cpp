/*
 * OkumuLab 1 — Labium DSP core (see LabiumCore.h)
 */
#include "LabiumCore.h"
#include "Arch.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#if OKL_X86
 #include <emmintrin.h>
#endif

namespace okl
{

namespace
{
#include "KcalTable.inc"

constexpr double R_GAS = 8.314462618;
constexpr double kPLin = 0.5;        // [Pa] orifice flow is linear below this pressure difference (P3)
constexpr double P_ATM = 101325.0;

struct Gas { double M, gamma, mu; };
constexpr Gas kAir { 0.028965, 1.400, 1.81e-5 };
constexpr Gas kHe { 0.0040026, 1.667, 1.96e-5 };
constexpr Gas kCO2 { 0.04401, 1.289, 1.47e-5 };

double interp (double x, const double* xs, const double* ys, int n)
{
    if (x <= xs[0]) return ys[0];
    for (int i = 1; i < n; ++i)
        if (x <= xs[i])
        {
            const double f = (x - xs[i - 1]) / (xs[i] - xs[i - 1]);
            return ys[i - 1] + f * (ys[i] - ys[i - 1]);
        }
    return ys[n - 1];
}

/* tanh as a rational function (the Eigen coefficients), error ~1e-7: far below the jet noise */
inline double fastTanh (double x)
{
    // branchless clamp: the jet swings through saturation every period (branches would mispredict)
#if OKL_X86
    const double xc = _mm_cvtsd_f64 (_mm_min_sd (_mm_max_sd (_mm_set_sd (x), _mm_set_sd (-7.90531110763549805)), _mm_set_sd (7.90531110763549805)));
#else
    const double xc = std::fmin (std::fmax (x, -7.90531110763549805), 7.90531110763549805);     // (fmax / fmin on ARM64)
#endif
    // Estrin's scheme: short dependency chains (this sits on the per-sample critical path)
    const double x2 = xc * xc, x4 = x2 * x2, x8 = x4 * x4;
    const double p01 = 4.89352455891786e-03 + 6.37261928875436e-04 * x2;
    const double p23 = 1.48572235717979e-05 + 5.12229709037114e-08 * x2;
    const double p45 = -8.60467152213735e-11 + 2.00018790482477e-13 * x2;
    const double p = xc * ((p01 + p23 * x4) + (p45 - 2.76076847742355e-16 * x4) * x8);
    const double q = (4.89352518554385e-03 + 2.26843463243900e-03 * x2) + (1.18534705686654e-04 + 1.19825839466702e-06 * x2) * x4;
    return p / q;
}

double defaultIsing (int midi)
{
    static const double xs[] { 36.0, 72.0 }, ys[] { 2.0, 2.4 };
    return interp ((double) midi, xs, ys, 2);
}

double lossGain (double f, double a, double am, double Leff, double c, double rho, double gam, double mu, double loss, double morph)
{
    const double Pr = 0.71;
    const double w = kTwoPi * f;
    const double nu = mu / rho;
    const double alpha = std::sqrt (w * nu / 2.0) / (a * c) * (1.0 + (gam - 1.0) / std::sqrt (Pr));
    const double k = w / c;
    const double radTop = 0.5 * (k * a) * (k * a) * (1.0 - morph);
    const double radMouth = 0.5 * (k * am) * (k * am);
    return std::exp (-2.0 * alpha * Leff * loss - radTop - radMouth);
}

/* residual of the third fit point for a pole a1: |H|^2 (1 + a1^2 + 2 a1 cw) = u + v cw */
/* P5: a side hole as the inertance of its air plug, a shunt at L1 on a lossless bore of length L
   (Z0 / (w L_h) = q / k, q = S_h / (S t_e)). The first resonance solves
     open top     sin kL + (q/k) sin kL1 sin kL2 = 0
     stopped top  cos kL + (q/k) sin kL1 cos kL2 = 0,    L2 = L - L1
   holeRatio: that resonance over the hole-free one (>= 1). */
double holeRatio (double L, double L1, double q, bool stopped)
{
    L1 = std::min (L1, L);
    const double L2 = L - L1, k0 = (stopped ? 0.5 : 1.0) * kPi / L, kTop = (stopped ? 1.5 : 2.0) * kPi / L;
    auto G = [&] (double k) {
        const double s1 = std::sin (k * L1) * q / k;
        return stopped ? std::cos (k * L) + s1 * std::cos (k * L2) : std::sin (k * L) + s1 * std::sin (k * L2);
    };
    double lo = k0, hi = kTop;     // G(k0) >= 0 >= G(kTop): the first sign change, then bisection
    for (int i = 1; i <= 12; ++i)
    {
        const double k = k0 + (kTop - k0) * i / 12.0;
        if (G (k) <= 0.0) { hi = k; break; }
        lo = k;
    }
    for (int i = 0; i < 18; ++i)        // (bracket k0/12 -> 2e-7 of k0)
    {
        const double m = 0.5 * (lo + hi);
        (G (m) > 0.0 ? lo : hi) = m;
    }
    return 0.5 * (lo + hi) / k0;
}
inline double fitResid (const double* Gt, const double* cw, double a1, double& u, double& v)
{
    const double y0 = Gt[0] * Gt[0] * (1 + a1 * a1 + 2 * a1 * cw[0]);
    const double y1 = Gt[1] * Gt[1] * (1 + a1 * a1 + 2 * a1 * cw[1]);
    v = (y0 - y1) / (cw[0] - cw[1]);
    u = y0 - v * cw[0];
    const double y2 = Gt[2] * Gt[2] * (1 + a1 * a1 + 2 * a1 * cw[2]);
    return (u + v * cw[2]) - y2;
}

void baseParams (const PipeDesign& pd, double kcal, ParamVec& v)
{
    v.fill (0.0);
    v[P_GATE] = 1.0;
    v[P_PCHEST] = pd.pChest;
    v[P_PALLET_TAU] = 0.012;
    v[P_D] = pd.d; v[P_LPHYS] = pd.lPhys; v[P_MOUTHW] = pd.mouthW; v[P_CUTUP] = pd.cutup;
    v[P_FLUE] = pd.flue; v[P_TOE] = pd.toe; v[P_FOOTVOL] = pd.footVol;
    v[P_C] = pd.c; v[P_RHO] = pd.rho; v[P_GAMMA] = pd.gamma; v[P_MU] = pd.mu;
    v[P_Y0] = 0.5 * 0.4 * pd.flue;
    v[P_NOISE] = 0.008;
    v[P_KAPPA] = 1.36;
    v[P_JETGAIN] = 1.0;
    v[P_MORPH] = 0.0;
    v[P_LOSS] = 2.5;
    v[P_FMF] = 0.0; v[P_FMD] = 0.0;
    v[P_PLOCK] = 1.0;
    v[P_FTARGET] = pd.f0;
    v[P_KCAL] = kcal;
    v[P_AMPCAP] = 4.0;
    v[P_EDGE] = 0.0;
    v[P_JETBW] = 0.16;
    v[P_ONSET_B] = 0.0;
    v[P_ONSET_T] = 15.0;
    v[P_KICK] = -0.5;
    v[P_VFOLLOW] = 0.0;
    v[P_FDESIGN] = pd.f0;
    v[P_EXCITE] = 0.0; v[P_METAL] = 1.0; v[P_WALL] = 1.0; v[P_HEAD] = 1.0; v[P_HEADD] = 25.0;
    v[P_STRIKE] = 0.3; v[P_DAMPER] = 0.6; v[P_TEMPK] = 293.15;
    v[P_NICK] = 0.0; v[P_FMTGT] = 0.0; v[P_CROSS] = 0.0; v[P_HOLE] = 0.0;
}
} // namespace

/* ------------------------------------------------------------------ gases */
GasProps gasProps (double air, double he, double co2, double tempC)
{
    const double tot = air + he + co2;
    const double T = tempC + 273.15;
    double M = 0, Cv = 0, mu = 0;
    const Gas* gs[3] { &kAir, &kHe, &kCO2 };
    const double xs[3] { air, he, co2 };
    for (int i = 0; i < 3; ++i)
    {
        if (xs[i] <= 0) continue;
        const double x = xs[i] / tot;
        M += x * gs[i]->M;
        Cv += x * R_GAS / (gs[i]->gamma - 1.0);
        mu += x * gs[i]->mu;
    }
    const double gamma = (Cv + R_GAS) / Cv;
    return { std::sqrt (gamma * R_GAS * T / M), P_ATM * M / (R_GAS * T), gamma, mu };
}

GasProps gasForKnob (double g, double tempC)
{
    if (g > 1e-6) return gasProps (1.0 - g, g, 0.0, tempC);
    if (g < -1e-6) return gasProps (1.0 + g, 0.0, -g, tempC);
    return gasProps (1.0, 0.0, 0.0, tempC);
}

double midiToHz (double m) { return 440.0 * std::pow (2.0, (m - 69.0) / 12.0); }

void organName (int m, char* buf)
{
    static const char* names[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "H" };
    static const char* lower[] { "c", "c#", "d", "d#", "e", "f", "f#", "g", "g#", "a", "a#", "h" };
    const int oct = (int) std::floor ((m - 36) / 12.0);
    const int pc = ((m % 12) + 12) % 12;
    if (oct < 0)
    {
        int n = std::snprintf (buf, 8, "%s", names[pc]);
        for (int i = 0; i < -oct && n < 7; ++i) buf[n++] = ',';
        buf[n] = 0;
    }
    else if (oct == 0) std::snprintf (buf, 8, "%s", names[pc]);
    else if (oct == 1) std::snprintf (buf, 8, "%s", lower[pc]);
    else std::snprintf (buf, 8, "%s%d", lower[pc], oct - 1);
}

/* ---------------------------------------------------------- Prinzipal 8' */
PipeDesign designPrinzipal (int midi, double voicingPa)
{
    const double scaleHT = -2.0, mouthFrac = 0.25, pChestMMWS = voicingPa / MMWS, flue0 = 0.65e-3;
    const double flueExp = 0.45, footRatio = 0.7, footLen = 0.17;
    const GasProps g = gasProps (1.0, 0.0, 0.0, 20.0);
    PipeDesign r;
    r.midi = midi;
    r.f0 = midiToHz (midi);
    const double k = midi - 36;
    r.d = 0.1555 * std::pow (2.0, -(k - scaleHT) / 16.0);     // Normalmensur, halving on the 17th
    r.ising = defaultIsing (midi);
    r.flue = flue0 * std::pow (r.d / 0.0504, flueExp);
    r.mouthW = mouthFrac * kPi * r.d;
    const double Sflue = r.flue * r.mouthW;
    const double Stoe = Sflue / std::sqrt (1.0 / footRatio - 1.0);
    r.toe = std::sqrt (4.0 * Stoe / kPi);
    r.toeConst = std::pow (r.toe * 1e3, 2) / (4.0 * mouthFrac * r.d * 1e3);
    r.pChest = pChestMMWS * MMWS;
    r.pFoot = r.pChest / (1.0 + std::pow (Sflue / Stoe, 2));
    r.cutup = std::pow (2.0 * r.pFoot * r.flue / (g.rho * r.ising * r.ising * r.f0 * r.f0), 1.0 / 3.0);   // Ising
    r.M = 0.3 * r.d * r.d / r.cutup;
    r.Leff = g.c / (2.0 * r.f0);
    r.lPhys = r.Leff - r.M - 0.3 * r.d;
    r.footLen = footLen;
    r.footVol = kPi / 12.0 * footLen * (r.d * r.d + r.d * r.toe + r.toe * r.toe);
    r.Uj = std::sqrt (2.0 * r.pFoot / g.rho);
    r.c = g.c; r.rho = g.rho; r.gamma = g.gamma; r.mu = g.mu;
    return r;
}

double kcalFor (double midi)
{
    static double xs[kKcalCount];
    static bool init = false;
    if (! init)
    {
        for (int i = 0; i < kKcalCount; ++i) xs[i] = kKcalFirst + i;
        init = true;
    }
    return interp (midi, xs, kKcal, kKcalCount);
}

void voiceParams (int midi, const Globals& G, const VoiceOverride& vo, const PipeDesign& pd, ParamVec& v)
{
    baseParams (pd, kcalFor (midi), v);
    const double k = midi - 36;
    const double d = 0.1555 * std::pow (2.0, -(k - G.scaleHT) / 16.0);
    v[P_D] = d;
    v[P_MOUTHW] = G.mouthFrac * kPi * d;
    v[P_CUTUP] = pd.cutup * G.cutup;
    v[P_Y0] = G.y0b * 0.4 * pd.flue;
    v[P_TOE] = pd.toe * G.toe;
    const double wind = vo.wind >= 0.0 ? vo.wind : G.wind * G.bellows;
    v[P_PCHEST] = std::max (0.0, wind) * MMWS;
    if (vo.tau > 0.0) v[P_PALLET_TAU] = vo.tau;
    v[P_NOISE] = G.noise;
    v[P_MORPH] = std::clamp (G.morph, 0.0, 1.0);
    v[P_PLOCK] = std::clamp (G.pitchLock, 0.0, 1.0);
    v[P_VFOLLOW] = G.vFollow;
    const double s = G.glide;
    const double corr = pd.Leff - pd.lPhys;
    v[P_LPHYS] = pd.Leff * std::pow (2.0, -s / 12.0) - corr;
    v[P_FTARGET] = pd.f0 * std::pow (2.0, s / 12.0);
    const GasProps gp = gasForKnob (G.gas, G.tempC);
    v[P_C] = gp.c * G.cMult;
    v[P_RHO] = gp.rho * G.rhoMult;
    v[P_GAMMA] = gp.gamma;
    v[P_MU] = gp.mu;
    v[P_FMF] = G.fmRatio * v[P_FTARGET];
    v[P_FMD] = G.fmDepth;
    v[P_JETGAIN] = G.jetGain;
    v[P_EDGE] = G.edge;
    v[P_KICK] = G.kick;
    v[P_LOSS] = G.loss;
    v[P_KAPPA] = G.kappa;
    // MALLET: the struck pipe keeps its physical length (pitch lock tunes the blown pipe's speaking pitch)
    v[P_EXCITE] = G.excite >= 0.5 ? 1.0 : 0.0;
    if (v[P_EXCITE] > 0.5) v[P_PLOCK] = 0.0;
    v[P_METAL] = std::clamp (std::round (G.metal), 0.0, (double) (kNumMetals - 1));
    v[P_WALL] = std::clamp (G.wallMult, 0.25, 4.0);
    v[P_HEAD] = std::clamp (std::round (G.head), 0.0, (double) (kNumHeads - 1));
    v[P_HEADD] = std::clamp (G.headD, 4.0, 80.0);
    v[P_STRIKE] = std::clamp (G.strikePos, 0.02, 0.98);
    v[P_DAMPER] = std::clamp (G.damper, 0.0, 1.0);
    v[P_TEMPK] = G.tempC + 273.15;
    // P5: nicking: the nicks break up the jet's coherent edge (a thicker, less amplifying jet, see render) and
    // calm the speech: less turbulence noise, a weaker starting vortex and edge tone
    const double nick = std::clamp (G.nicking, 0.0, 1.0);
    v[P_NICK] = nick;
    v[P_NOISE] = G.noise * (1.0 - 0.7 * nick);
    v[P_KICK] = G.kick * (1.0 - 0.8 * nick);
    v[P_EDGE] = G.edge * (1.0 - 0.8 * nick);
    v[P_FMTGT] = std::clamp (std::round (G.fmTarget), 0.0, 2.0);
    v[P_CROSS] = std::clamp (G.crossDrive, 0.0, 1.0);
    v[P_HOLE] = std::clamp (G.sideHole, 0.0, 1.0);
}

/* ------------------------------------------------------------- derived */
void derive (const double* p, double fs, Derived& dv)
{
    const double c = p[P_C];
    const double d = p[P_D], a = 0.5 * d;
    const double H = p[P_MOUTHW];
    double W = p[P_CUTUP];
    const double morph = std::clamp (p[P_MORPH], 0.0, 1.0);

    double M = 0, Leff = 0, fres = 0;
    for (int pass = 0; pass < 2; ++pass)
    {
        /* mouth end correction: M ~ 0.3 d^2 / W */
        M = 0.3 * d * d / W;
        const double LphysEff = p[P_LPHYS] + M + 0.3 * d * (1.0 - morph);
        const double Llock = c / (2.0 * p[P_FTARGET]) * p[P_KCAL];
        Leff = (1.0 - p[P_PLOCK]) * LphysEff + p[P_PLOCK] * Llock;
        if (Leff < 0.005) Leff = 0.005;
        fres = c / (2.0 * Leff) * (1.0 - 0.5 * morph);
        /* voicing follow: W ~ f^(-2/3) keeps the Ising number as the resonance moves */
        if (pass == 0 && p[P_VFOLLOW] > 0 && p[P_FDESIGN] > 0)
            W = W * std::pow (fres / p[P_FDESIGN], -2.0 / 3.0 * p[P_VFOLLOW]);
        else
            break;
    }
    dv.W = W;

    /* P5: an open side hole raises the first resonance (holeRatio). It is a tone hole: pitch lock keeps the
       closed pipe on the key and the servo, listening at the raised resonance, removes only the jet's pull, so
       the hole sounds its physical interval. (Lengthening the bore until the open-hole resonance is the key
       makes another instrument: with the hole near the middle that pipe speaks its inharmonic second mode.) */
    double holeR = 1.0, holeX = 0.0;
    if (p[P_HOLE] > 0.02)
    {
        const double bh = 0.24 * a, tw = wallThickness (d) * p[P_WALL];        // the hole of Voice::render, fully open
        const double q = bh * bh / (a * a * (tw + 1.45 * bh));
        const double x0 = W + 1.2 * H, x1 = std::max (x0, p[P_LPHYS] - 1.2 * a);
        const double LphysEff = p[P_LPHYS] + M + 0.3 * d * (1.0 - morph);
        // its place, from the open end of the mouth, as a fraction of the bore (the locked bore carries it along)
        const double lam = std::min (0.98, (M + x0 + (x1 - x0) * p[P_HOLE]) / LphysEff);
        const double L1 = lam * Leff;
        if (morph <= 0.0) holeR = holeRatio (Leff, L1, q, false);
        else if (morph >= 1.0) holeR = holeRatio (Leff, L1, q, true);
        else holeR = (1.0 - morph) * holeRatio (Leff, L1, q, false) + morph * holeRatio (Leff, L1, q, true);
        holeX = std::max (0.0, L1 - M);                                         // along the waveguide (after the mouth inertance)
        fres *= holeR;
    }
    dv.holeR = holeR;
    dv.holeX = holeX;

    /* loss filter: first-order shelf fitted to the physical round-trip loss at f1, 3 f1, 9 f1 */
    const double am = std::sqrt (H * W / kPi);
    const double f1 = c / (2.0 * Leff);
    double fa[3] { f1, 3.0 * f1, 9.0 * f1 };
    bool clamped = false;
    for (int i = 0; i < 3; ++i) if (fa[i] > 0.4 * fs) { fa[i] = 0.4 * fs * (0.6 + 0.2 * i); clamped = true; }
    double Gt[3], cw[3];
    for (int i = 0; i < 3; ++i) Gt[i] = lossGain (fa[i], a, am, Leff, c, p[P_RHO], p[P_GAMMA], p[P_MU], p[P_LOSS], morph);
    cw[0] = std::cos (kTwoPi * fa[0] / fs);
    if (! clamped)
    {
        // cos 3x and cos 9x from cos x (Chebyshev), instead of two more cos() calls
        cw[1] = cw[0] * (4.0 * cw[0] * cw[0] - 3.0);
        cw[2] = cw[1] * (4.0 * cw[1] * cw[1] - 3.0);
    }
    else
    {
        cw[1] = std::cos (kTwoPi * fa[1] / fs);
        cw[2] = std::cos (kTwoPi * fa[2] / fs);
    }
    double bestA1 = 0.0, bestU = Gt[0] * Gt[0], bestV = 0.0, bestR = 1e30;
    const double lo = -0.9995, hi = 0.9995;
    double rlo = 0, rhi = 0;
    /* the residual is a quadratic in a1: r = P (1 + a1^2) + 2 Q a1 (its roots multiply to 1, so at
       most one lies inside the unit circle). Solve it directly; the Phase 1 scan + bisection below
       is kept for the cases without a usable root and gives the same a1 when there is one. */
    bool solved = false;
    {
        const double g0 = Gt[0] * Gt[0], g1 = Gt[1] * Gt[1], g2 = Gt[2] * Gt[2];
        const double K = (cw[2] - cw[0]) / (cw[0] - cw[1]);
        const double P = (g0 - g2) + K * (g0 - g1);
        const double Q = (g0 * cw[0] - g2 * cw[2]) + K * (g0 * cw[0] - g1 * cw[1]);
        double root = 2.0;
        if (std::abs (P) < 1e-300) root = 0.0;
        else
        {
            const double disc = Q * Q - P * P;
            if (disc >= 0.0)
            {
                const double s = std::sqrt (disc);
                // the root of smaller magnitude, computed without cancellation
                const double q = -(Q + (Q >= 0 ? s : -s));
                root = q != 0.0 ? P / q : 2.0;
            }
        }
        if (root >= lo && root <= hi)
        {
            double u, v;
            fitResid (Gt, cw, root, u, v);
            if (u >= std::abs (v)) { bestA1 = root; bestU = u; bestV = v; solved = true; }
            else
            {
                /* the exact fit is not a valid filter (|H|^2 would go negative): Phase 1 then keeps the
                   last bisection step that is valid. The scan's first sign change is the grid cell that
                   holds the root, so start the same 50-step bisection there. */
                const int k = std::clamp ((int) std::floor ((root - lo) * 200.0 / (hi - lo)) + 1, 1, 200);
                const double aPrev = lo + (hi - lo) * (k - 1) / 200.0, aK = lo + (hi - lo) * k / 200.0;
                double ut, vt;
                const double rPrev = fitResid (Gt, cw, aPrev, ut, vt);
                const double rK = fitResid (Gt, cw, aK, ut, vt);
                if (rPrev * rK < 0 && aPrev < root)
                {
                    /* u + v and u - v are quadratics of the same form, so the valid a1 (u >= |v|) form
                       one interval [vL, vH]; the bisection path is decided by which side of the root
                       the midpoint falls (the sign of r), so it can be replayed without evaluating the
                       fit, stopping once the bracket has left the valid interval. */
                    const double invd = 1.0 / (cw[0] - cw[1]);
                    const double dg = (g0 - g1) * invd, dgc = (g0 * cw[0] - g1 * cw[1]) * invd;
                    double vL = -1.0, vH = 1.0;
                    auto keep = [&] (double Aq, double Bq)       // a1 in (-1, 1) with Aq (1 + a1^2) + 2 Bq a1 >= 0
                    {
                        const double dsc = Bq * Bq - Aq * Aq;
                        if (dsc <= 0.0) { if (Aq < 0.0) { vL = 1.0; vH = -1.0; } return; }
                        const double qq = -(Bq + (Bq >= 0 ? std::sqrt (dsc) : -std::sqrt (dsc)));
                        const double r0 = qq != 0.0 ? Aq / qq : 0.0;
                        const bool zeroSide = Aq >= 0.0;        // the sign at a1 = 0 is the sign of Aq
                        if (zeroSide == (r0 > 0.0)) vH = std::min (vH, r0); else vL = std::max (vL, r0);
                    };
                    keep (g0 + (1.0 - cw[0]) * dg, g0 * cw[0] + (1.0 - cw[0]) * dgc);     // u + v >= 0
                    keep (g0 - (1.0 + cw[0]) * dg, g0 * cw[0] - (1.0 + cw[0]) * dgc);     // u - v >= 0
                    double bl = aPrev, bh = aK, best = 0.0;
                    bool any = false;
                    for (int it = 0; it < 50 && bh >= vL && bl <= vH; ++it)
                    {
                        const double a1 = 0.5 * (bl + bh);
                        if (a1 >= root) bh = a1; else bl = a1;
                        if (a1 >= vL && a1 <= vH) { best = a1; any = true; }
                    }
                    if (any)
                    {
                        double ub, vb;
                        fitResid (Gt, cw, best, ub, vb);
                        if (ub >= std::abs (vb)) { bestA1 = best; bestU = ub; bestV = vb; solved = true; }
                    }
                    else
                    {
                        /* no valid bisection step: Phase 1 keeps the scan's valid grid point with the
                           smallest |r| up to the bracket. r is monotone inside the unit circle (its
                           extremum -Q/P lies outside), so that is the valid grid point nearest below the
                           root, or the bracket end above it. */
                        const int kHi = std::min (k - 1, (int) std::floor ((vH - lo) * 200.0 / (hi - lo)));
                        const int kLo = std::max (0, (int) std::ceil ((vL - lo) * 200.0 / (hi - lo)));
                        double bestAbs = 1e30;
                        for (int kk = kHi; kk >= kLo && kk >= kHi - 1; --kk)       // (one step back for rounding at vH)
                        {
                            const double a1 = lo + (hi - lo) * kk / 200.0;
                            double ub, vb;
                            const double r = fitResid (Gt, cw, a1, ub, vb);
                            if (ub >= std::abs (vb)) { bestAbs = std::abs (r); bestA1 = a1; bestU = ub; bestV = vb; solved = true; break; }
                        }
                        {
                            double ub, vb;
                            const double r = fitResid (Gt, cw, aK, ub, vb);
                            if (ub >= std::abs (vb) && std::abs (r) < bestAbs) { bestA1 = aK; bestU = ub; bestV = vb; solved = true; }
                        }
                    }
                }
            }
        }
    }
    if (! solved)
    {
        double prevA = lo, prevR = 0;
        bool found = false;
        for (int k = 0; k <= 200 && ! found; ++k)
        {
            const double a1 = lo + (hi - lo) * k / 200.0;
            double u, v;
            const double r = fitResid (Gt, cw, a1, u, v);
            if (u >= std::abs (v) && std::abs (r) < bestR) { bestR = std::abs (r); bestA1 = a1; bestU = u; bestV = v; }
            if (k > 0 && prevR * r < 0) { rlo = prevA; rhi = a1; found = true; }
            prevA = a1; prevR = r;
        }
        if (found)
        {
            for (int it = 0; it < 50; ++it)
            {
                const double a1 = 0.5 * (rlo + rhi);
                double u, v, ul, vl;
                const double r = fitResid (Gt, cw, a1, u, v);
                const double rl = fitResid (Gt, cw, rlo, ul, vl);
                if (rl * r <= 0) rhi = a1; else rlo = a1;
                if (u >= std::abs (v)) { bestA1 = a1; bestU = u; bestV = v; bestR = std::abs (r); }
            }
        }
    }
    const double sp = std::sqrt (std::max (bestU + bestV, 0.0)), sm = std::sqrt (std::max (bestU - bestV, 0.0));
    dv.lb0 = 0.5 * (sp + sm);
    dv.lb1 = 0.5 * (sp - sm);
    dv.la1 = bestA1;
    {
        /* P3: keep the loop passive. The fit can exceed unit gain at extreme settings (the peak of a
           first-order section is at DC or Nyquist); scale it back. Normal pipes stay well below 1. */
        const double g0 = std::abs ((dv.lb0 + dv.lb1) / (1.0 + dv.la1)), gpi = std::abs ((dv.lb0 - dv.lb1) / (1.0 - dv.la1));
        const double gmax = std::max (g0, gpi);
        if (gmax > 0.9995) { dv.lb0 *= 0.9995 / gmax; dv.lb1 *= 0.9995 / gmax; }
    }
    {
        const double w = kTwoPi * f1 / fs;
        const double cs = cw[0], sn = std::sqrt (std::max (0.0, 1.0 - cs * cs));     // 0 < w < pi
        const double nr = dv.lb0 + dv.lb1 * cs, ni = -dv.lb1 * sn;
        const double dr = 1.0 + dv.la1 * cs, di = -dv.la1 * sn;
        // arg(N) - arg(D) = arg(N conj(D)), one atan2; same branch as the difference for |a1| < 1, lb1 >= -lb0
        double ph = std::atan2 (ni * dr - nr * di, nr * dr + ni * di);
        dv.lpDelay = -ph / w;
    }
    /* mouth window = lumped inertance (time constant M/c) */
    dv.taum = M / c;
    /* open <-> stopped morph: crossover frequency from Nyquist down to 5 Hz (recomputed only when it moves) */
    if (morph != dv.morphFor || fs != dv.fsFor)
    {
        const double fx = 0.5 * fs * std::pow (5.0 / (0.5 * fs), morph);
        const double t = std::tan (std::min (kPi * fx / fs, kPi / 2.0 - 1e-9));
        dv.aMorph = (t - 1.0) / (t + 1.0);
        dv.morphFor = morph;
        dv.fsFor = fs;
    }
    /* one-way delay: round trip = 2 Leff, minus mouth inertance (2M) and loss-filter delay */
    double D = ((2.0 * Leff - 2.0 * M) * fs / c - dv.lpDelay) / 2.0;
    if (D < 3.0) D = 3.0;
    dv.dline = D;
    dv.M = M;
    dv.Leff = Leff;
    dv.fres = fres;
}

/* ----------------------------------------------------------- delay line */
void DelayLine::allocate (int minLen)
{
    int n = 1;
    while (n < minLen) n <<= 1;
    buf.assign ((size_t) n, 0.0);
    mask = n - 1;
    w = 0;
}

void DelayLine::clear()
{
    std::fill (buf.begin(), buf.end(), 0.0);
    w = 0;
}

/* ---------------------------------------------------------------- voice */
void Voice::prepare (double sampleRate, uint32_t seed)
{
    fs = sampleRate;
    const int len = (int) (0.34 * fs);              // the Phase 2 engine: 32768 at 96 kHz
    up.allocate (len);
    lo.allocate (len);
    vml.allocate (len);
    topl.allocate ((int) (0.04 * fs));             // v1.0: the top's path (MIDI 0: a 21 m pipe, 0.46 x that = 28 ms)
    maxTau = std::min (fs * 0.25, (double) vml.length() - 8.0);
    maxDline = (double) up.length() - 8.0;
    seed0 = seed;
    k_dq = 1.0 - std::exp (-kTwoPi * 12000.0 / fs);
    k_nz = 1.0 - std::exp (-kTwoPi * 4000.0 / fs);
    smooth = 1.0 - std::exp (-CTRL / (fs * 0.012));     // ~12 ms parameter glide
    zEnvDecay = std::exp (-1.0 / (0.03 * fs));
    kDc = 1.0 - std::exp (-kTwoPi * 5.0 / fs);
    mallet.prepare (fs);
    mShell.assign (1024, 0.0);
    mQ.assign (1024, 0.0);
    A.fill (0); B.fill (0); T.fill (0);
    reset();
}

void Voice::reset()
{
    up.clear(); lo.clear(); vml.clear(); topl.clear();
    etaMean = etaVar = 0; topD = -1; jetSatF = 1.0;
    uint32_t s = (uint32_t) ((uint64_t) seed0 * 2654435761u);
    rng = s ? s : 1u;
    pg = 0; pf = 0;
    qin_prev = 0; dq_lp = 0; vm = 0;
    lsh_x1 = lsh_y1 = dc_x1 = dc_y1 = 0;
    apm_x1 = apm_y1 = mq_prev = mu_prev = 0;
    vj = vj2 = 0; jl1 = jl2 = 0; gate_on = 0; onset_t = 0; kick_t = -1;
    um_prev = ut_prev = nz = phi = 0;
    resets = 0; fpos = 0; tau = -1;
    trem = 1.0;
    etaN = 0; inflow = 0.5; Uj = 0; peak = 0; quiet = 0;
    capState = 0;
    mallet.reset();
    holeU = holeP = holeOpen = holeUprev = 0;
    holePrevDh = holePrevDt = -1;
    holeA = holeB = holeI2Y0 = 0; holeGLow = 1.0; holeDh = 2;
    jitA = 1.0; jitGd = 0.0;
    s1x1 = s1x2 = s1y1 = s1y2 = s2x1 = s2x2 = s2y1 = s2y2 = 0;
    rawMean = eRaw = eBp = eN = 0; evalLen = 0.03 * fs; settle = 0; rawLp = 0; kRawLp = 1;
    lowShare = 0; scanIdx = -1; scanBest = 0; scanBestShare = 0; irregular = 0;
    fb = fbDesigned = 0; regime = 1.0;
    zEnv = zPrev = 0; zArmed = false;
    sampleClock = 0; zFirst = zLast = -1; zMinI = 1e30; zMaxI = 0; zCount = 0;
    fPrevMeas = 0;
    measuredHz = 0; measuredRatio = 0; measuredCents = 0; measuredLocked = false;
}

void Voice::start (const ParamVec& tgt, double servoCents)
{
    reset();
    servoD = servoApplied = servoCents;
    servoV = 0;
    T = tgt;
    if (servoEnabled) T[P_KCAL] = std::exp2 (servoApplied / 1200.0);
    A = T;
    B = T;
    derive (A.data(), fs, *dv0);
    derive (B.data(), fs, *dv1);
    regime = A[P_MORPH] > 0.5 ? 0.5 : 1.0;      // a stopped pipe speaks an octave down
    active = true;
}

void Voice::setTarget (const ParamVec& tgt)
{
    T = tgt;
    if (servoEnabled) T[P_KCAL] = std::exp2 (servoApplied / 1200.0);
}

/* advance to the next control frame: targets are glided, derive() only when something moved */
void Voice::nextFrame()
{
    A = B;
    std::swap (dv0, dv1);
    bool moved = false;
    const double a = smooth;
    for (int j = 0; j < NPAR; ++j)
    {
        const double tj = T[(size_t) j];
        const double bo = B[(size_t) j];
        double bj;
        if (j == P_GATE || j == P_PALLET_TAU || j == P_FMF || j == P_EXCITE || j == P_METAL || j == P_HEAD || j == P_FMTGT) bj = tj;     // the pallet model smooths the gate; choices jump
        else
        {
            bj = bo + (tj - bo) * a;
            if (std::abs (tj - bj) <= 1e-7 * std::abs (tj) + 1e-15) bj = tj;
        }
        if (bj != bo) { moved = true; B[(size_t) j] = bj; }
    }
    if (moved) derive (B.data(), fs, *dv1);
    else *dv1 = *dv0;
}


void Voice::setHostRate (double hostFs)
{
    osM = std::max (1, (int) std::lround (fs / hostFs));
    mallet.prepare (fs / osM);
}

void Voice::render (int n, double* mixL, double* mixR, double* sig, double* hostMix, double* hostSig)
{
    if (n > (int) mShell.size())         // (the engine renders 128 samples at a time; the checks may ask for more)
    {
        const int h = (int) mShell.size(), hh = h / osM;
        render (h, mixL, mixR, sig, hostMix, hostSig);
        render (n - h, mixL + h, mixR + h, sig ? sig + h : nullptr, hostMix ? hostMix + hh : nullptr, hostSig ? hostSig + hh : nullptr);
        return;
    }
    const bool capOn = capState == 1 || capState == 2;
    double eta = 0, qin = 0, om = 0, ot = 0;

    /* The per-sample state lives in locals for the whole block (as in the Phase 2 engine):
       MSVC assumes any store through a double* may hit a member, so member state on the
       critical path would be reloaded from memory every sample. */
    double* const upB = up.data();
    double* const loB = lo.data();
    double* const vmB = vml.data();
    const int dmask = up.maskBits(), vmask = vml.maskBits();
    int dw = up.writePos(), vw = vml.writePos();          // up and lo are written in lockstep
    const size_t dlen = (size_t) up.length(), vlen = (size_t) vml.length();

    double pgL = pg, pfL = pf, UjL = Uj, vmL = vm;
    double qin_prevL = qin_prev, dq_lpL = dq_lp, lsh_x1L = lsh_x1, lsh_y1L = lsh_y1, dc_x1L = dc_x1, dc_y1L = dc_y1;
    double apm_x1L = apm_x1, apm_y1L = apm_y1, mq_prevL = mq_prev, mu_prevL = mu_prev, vjL = vj, vj2L = vj2;
    double kick_tL = kick_t, um_prevL = um_prev, ut_prevL = ut_prev, nzL = nz, phiL = phi, tauL = tau;
    double dlineL = dline, dlineStepL = dlineStep, tauStepL = tauStep;
    uint32_t rngL = rng;
    int fposL = fpos, footStepsL = footSteps;
    double SpipeL = Spipe, SpSmL = SpSm, SflueL = Sflue, StoeL = Stoe, k2rL = k2r, kpL = kp, targetL = target, kfStepL = kfStep;
    double fmdL = fmd, dphiL = dphi, kjL = kj, hGL = hG, invbL = invb, bHL = bH, noiseAL = noiseA, y0cL = y0c;
    double pjKL = pjK, pvKL = pvK, kickL = kick, kickArmL = kickArm, kickStepL = kickStep, edgeKL = edgeK, radKL = radK;
    double KmL = Km, invKm1L = invKm1, invZcL = invZc, lb0L = lb0, lb1L = lb1, la1L = la1, amL = am;
    const double kdq = k_dq, knz = k_nz, fsL = fs, jitL = jetJitter;
    double jl1L = jl1, jl2L = jl2, jitAL = jitA, jitGdL = jitGd;
    // v1.0: the jet's swing at the lip, the top's path to the listener
    double etaMeanL = etaMean, etaVarL = etaVar, kEtaL = kEtaTrack, topDL = topD, topDStepL = topDStep, satL = jetSatF;
    const bool topPath = topSinElev > 0.0;
    double* const topB = topl.data();
    const int tmask = topl.maskBits();
    int topW = topl.writePos();

    /* MALLET: the wall and the mallet for the whole block first (the air column does not push back on the wall);
       the bore then takes the wall's breathing at the strike point, and the wall's radiation joins the output */
    const bool malOn = A[P_EXCITE] > 0.5 || mallet.active();
    double malDs = 0, malDl = 0, malK = 0;
    const int nh = n / osM;
    if (malOn)
    {
        // the wall at the host rate (its modes reach 20 kHz there); the bore takes the same volume velocity per host sample
        mallet.setParams (strikeParams (A));
        if (mallet.active()) mallet.render (nh, mShell.data(), mQ.data());
        else { std::fill (mShell.begin(), mShell.begin() + nh, 0.0); std::fill (mQ.begin(), mQ.begin() + nh, 0.0); }
        if (hostMix) for (int j = 0; j < nh; ++j) hostMix[j] += mShell[(size_t) j];
        if (hostSig) for (int j = 0; j < nh; ++j) hostSig[j] += mShell[(size_t) j];
        for (int j = 0; j < nh; ++j) peak = std::max (peak, std::abs (mShell[(size_t) j]));
        const Derived& d0 = *dv0;
        const double cc = A[P_C] > 1 ? A[P_C] : 343.0;
        malDl = std::min (d0.dline, maxDline);
        malDs = std::clamp (A[P_STRIKE] * A[P_LPHYS] * fs / cc, 1.0, std::max (1.0, malDl - 1.0));
        const double S = kPi * A[P_D] * A[P_D] / 4.0;
        malK = A[P_RHO] * cc / (2.0 * S);       // a volume source drives half its pressure each way
    }

    // P5: physical FM target, cross drive, side hole (frame constants, set again in each control frame)
    int fmTgtL = (int) A[P_FMTGT];
    double lipAmpL = 2.5 * 0.4 * A[P_FLUE], crossKL = A[P_CROSS];
    const double* const crossInL = crossIn;
    double* const vmOutL = vmOut;
    bool holeOnL = holeOpen > 0.0;
    int holePrevDhL = holePrevDh, holePrevDtL = holePrevDt;
    int holeDhL = holeDh;
    double holeAL = holeA, holeBL = holeB, holeI2Y0L = holeI2Y0, holeGLowL = holeGLow;

    auto lagr = [] (double pos, int& i, double* cf)
    {
        i = (int) pos;
        if ((double) i > pos) --i;
        const double f = pos - (double) i;
        cf[0] = -f * (f - 1.0) * (f - 2.0) / 6.0;
        cf[1] = (f + 1.0) * (f - 1.0) * (f - 2.0) / 2.0;
        cf[2] = -(f + 1.0) * f * (f - 2.0) / 2.0;
        cf[3] = (f + 1.0) * f * (f - 1.0) / 6.0;
    };

    for (int t = 0; t < n; ++t)
    {
        if (fposL == 0)
        {
            // P3: hand the servo's estimate to the pitch-lock length (deadband keeps derive() idle when settled)
            if (servoEnabled && std::abs (servoD - servoApplied) > 0.05)
            {
                servoApplied = servoD;
                T[P_KCAL] = std::exp2 (servoApplied / 1200.0);
            }
            nextFrame();
            const Derived& d0 = *dv0;
            const Derived& d1 = *dv1;
            c = A[P_C]; rho = A[P_RHO]; zc = rho * c;
            const double d = A[P_D], h = A[P_FLUE], W = d0.W;
            H = A[P_MOUTHW];
            SpipeL = kPi * d * d / 4.0;
            SpSmL = SpipeL / (H * W);
            SflueL = h * H;
            StoeL = kPi * A[P_TOE] * A[P_TOE] / 4.0;
            kf = (1.0 / (fsL * 4)) * rho * c * c / A[P_FOOTVOL];
            k2rL = 2.0 / rho;
            const double tau_p = A[P_PALLET_TAU] > 1e-4 ? A[P_PALLET_TAU] : 1e-4;
            kpL = 1.0 - std::exp (-1.0 / (tau_p * fsL));
            const double gt = A[P_GATE];
            targetL = gt * A[P_PCHEST] * trem;                      // tremulant on the wind
            fmdL = A[P_FMD]; dphiL = kTwoPi * A[P_FMF] / fsL;
            fmTgtL = (int) A[P_FMTGT];
            lipAmpL = 2.5 * 0.4 * A[P_FLUE];                     // lip FM: the labium offset swings up to +-2.5 b
            crossKL = A[P_CROSS];
            // jet
            const double fres = d0.fres;
            const double uc = A[P_KAPPA] * std::pow (UjL, 2.0 / 3.0) * std::cbrt (kTwoPi * fres * h);
            double tauN = W / uc * fsL;
            if (! (tauN >= 3.0)) tauN = 3.0;
            if (tauN > maxTau) tauN = maxTau;
            if (tauL < 0) tauL = tauN;
            tauStepL = (tauN - tauL) / CTRL;
            if (jitL > 0.0)
            {
                /* P5: the 7 jitter taps sample the Gaussian every dl samples, so their response repeats every fs / dl, where
                   all taps add in phase at full gain (a stopped c whistled there at 1.1 kHz). Two one-pole low-passes at
                   0.4 fs / dl ahead of the taps take those lobes down by 17 dB; the read moves earlier by their group delay,
                   so the mean delay and the low-frequency phase stay those of the Gaussian. */
                const double dlF = std::max (1.0, jitL * tauN * (2.0 / 3.0));
                jitAL = 1.0 - std::exp (-kTwoPi * 0.4 / dlF);
                jitGdL = 2.0 * (1.0 - jitAL) / jitAL;
            }
            double fjet = A[P_JETBW] * UjL / h;
            if (fjet > 0.4 * fsL) fjet = 0.4 * fsL;
            kjL = 1.0 - std::exp (-kTwoPi * fjet / fsL);
            // P5: nicking thickens the jet (b) and lowers its amplification (growth ~ 1/b); the jet's flow stays the same
            const double nk = 1.0 + 0.75 * A[P_NICK];
            const double b0 = 0.4 * h * nk;
            double ampexp = 0.4 * W / (h * nk);
            /* v1.0: Phase 1 capped the growth at e^4 since nothing else bounded the jet's swing. With the swing's saturation
               (below) the cap only bounds how far the long bass mouths (25 .. 35 h) overdrive the jet: e^6, 5 .. 15 x */
            const double ampCap = jetSat > 0.0 ? 6.0 : A[P_AMPCAP];
            if (ampexp > ampCap) ampexp = ampCap;
            /* v1.0: across the mouth the jet spreads, b(x) = b0 + s x, as much as it is turbulent: laminar below a Reynolds number
               U_j h / nu of 500 (the treble), transitional up to s = jetSpread from 2000 (bass pipes at strong wind). spRef: as
               the pipe was voiced (the voicing pressure, air at 20 C) */
            double sp = 0.0, spRef = 0.0;
            if (jetSpread > 0.0)
            {
                auto spreadAt = [&] (double U, double nu) { return jetSpread * std::clamp ((U * h / nu - 500.0) / 1500.0, 0.0, 1.0); };
                sp = spreadAt (UjL, A[P_MU] / std::max (1e-6, A[P_RHO]));
                spRef = spreadAt (jetUjRef, 1.81e-5 / 1.204);
            }
            kEtaL = 1.0 - std::exp (-fres / (2.0 * fsL));      // the swing over about two periods
            if (topPath)
            {
                // v1.0: the open top is the pipe's length above the mouth: its sound comes that x sin(elevation) later
                const double tgt = std::clamp (A[P_LPHYS] * topSinElev * fsL / std::max (1.0, A[P_C]), 0.0, (double) topl.length() - 4.0);
                if (topDL < 0) topDL = tgt;
                topDStepL = (tgt - topDL) / CTRL;
            }
            if (gt > 0.5)
            {
                if (! gate_on) { gate_on = 1; onset_t = 0.0; }
                onset_t += fres * CTRL / fsL;
            }
            else gate_on = 0;
            if (gate_on && A[P_ONSET_T] > 0) ampexp += A[P_ONSET_B] * std::exp (-onset_t / A[P_ONSET_T]);
            hGL = A[P_JETGAIN] * std::exp (ampexp) * h;
            b = b0 + sp * W; invbL = 1.0 / b; bHL = 0.4 * h * H;      // (the jet's flow stays that of the flue)
            /* v1.0: the jet's sinuous wave grows only until its swing is about its own width (Fletcher): beyond jetSat b the
               swing at the lip stays there. The wave's amplitude saturates, so within the period it stays a smooth wave; Phase 1's
               jet swung up to 9 b in the treble and switched like a valve between inside and outside (a square wave, a buzz). */
            if (jetSat > 0.0)
            {
                const double aLin = std::sqrt (2.0 * etaVarL);
                satL = aLin > jetSat * b ? jetSat * b / aLin : 1.0;
            }
            else satL = 1.0;
            noiseAL = A[P_NOISE] * h * 3.0;
            // v1.0: the voicer sets the upper lip off the arriving jet's axis by half its spread (with the default labium offset,
            // 0.5 b0 + 0.5 s W: half the jet's half-width at the lip), so the jet's pulse is asymmetric, as in a Principal
            y0cL = A[P_Y0] + jetLipVoice * spRef * W;
            pjKL = -rho * (4.0 / kPi * std::sqrt (2.0 * h * W)) / (W * H);
            pvKL = -0.5 * rho / 0.36;
            kickL = gate_on ? A[P_KICK] : 0; kickArmL = 0.5 * A[P_PCHEST] * 0.3; kickStepL = fres / fsL;
            if (! gate_on) kick_tL = -1.0;
            edgeKL = A[P_EDGE] / (H * W);
            radKL = rho / (4.0 * kPi) * fsL;
            KmL = 2.0 * d0.taum * fsL; lb0L = d0.lb0; lb1L = d0.lb1; la1L = d0.la1; amL = d0.aMorph;
            invKm1L = 1.0 / (KmL + 1.0);
            invZcL = 1.0 / zc;
            /* foot: 4 Euler sub-steps (labium_core.c) while the foot is stiff (pallet opening, near-zero
               pressure differences, wind FM); one step of 4x the size once it is not. The steady
               state (toe flow = flue flow) is the same either way. */
            {
                const double dp0 = std::abs (pgL - pfL), pr = std::max (targetL, 1.0);
                double stiff = 1e9;
                if (dp0 > 0.02 * pr && pfL > 0.05 * pr)
                    stiff = 4.0 * kf * k2rL * (StoeL / (2.0 * std::sqrt (k2rL * dp0)) + SflueL / (2.0 * std::sqrt (k2rL * pfL)));
                footStepsL = (stiff < 0.25 && std::abs (fmdL) < 0.02 && std::abs (targetL - pgL) < 0.02 * pr) ? 1 : 4;
                kfStepL = kf * (4 / footStepsL);
            }
            const double dl0 = std::min (d0.dline, maxDline), dl1 = std::min (d1.dline, maxDline);
            dlineL = dl0; dlineStepL = (dl1 - dl0) / CTRL;
            /* P5: side hole: a small open hole in the wall (radius 0.24 a, as drawn), a shunt on the bore with the
               inertance of its air plug rho t_e / S_h (t_e = wall + 1.45 b) and the radiation resistance of a
               small source, rho w^2 / (2 pi c); it opens and closes over ~12 ms */
            {
                const double sh = A[P_HOLE];
                const double openTgt = sh > 0.02 ? 1.0 : 0.0;
                holeOpen += (openTgt - holeOpen) * smooth;
                if (openTgt == 0.0 && holeOpen < 1e-3) { holeOpen = 0.0; holeOnL = false; holeU = holeP = 0.0; holePrevDhL = holePrevDtL = -1; }
                else
                {
                    holeOnL = true;
                    const double ar = 0.5 * d, bh = std::max (1e-5, 0.24 * ar * holeOpen);
                    const double Sh = kPi * bh * bh, tw = wallThickness (d) * A[P_WALL];
                    const double Lh = rho * (tw + 1.45 * bh) / Sh;
                    const double wr = kTwoPi * std::max (20.0, d0.fres);
                    holeAL = 0.5 / (fsL * Lh);
                    holeBL = rho * wr * wr / (kTwoPi * c) * holeAL;
                    holeI2Y0L = rho * c / (2.0 * SpipeL);
                    // from the mouth (derive(): above the upper lip .. one radius below the top, scaled with the lock)
                    holeDhL = (int) std::lround (d0.holeX * fsL / c);
                    /* the bore's wall loss is lumped in the filter at the top: waves the hole sends back to the mouth would
                       never meet it, and a pipe with its hole open rang on longer than with it closed. They get the lower
                       segment's share of it, |H(f1)|^(Dh/Dl), as they leave the junction (an attenuation: passive). */
                    const double w1 = kTwoPi * c / (2.0 * d0.Leff) / fsL, cw1 = std::cos (w1), sw1 = std::sin (w1);
                    const double nr = d0.lb0 + d0.lb1 * cw1, ni = -d0.lb1 * sw1, dr = 1.0 + d0.la1 * cw1, di = -d0.la1 * sw1;
                    const double G1 = std::sqrt ((nr * nr + ni * ni) / std::max (1e-30, dr * dr + di * di));
                    holeGLowL = std::pow (std::clamp (G1, 0.5, 1.0), std::clamp (holeDhL / std::max (1.0, dl0), 0.0, 1.0));
                }
            }
            // P3: servo band-pass follows the nominal resonance
            fb = c / (2.0 * d0.Leff) * d0.holeR;
            if (std::abs (fb - fbDesigned) > 0.004 * fbDesigned) servoDesign();
        }

        /* ---- wind: pallet + foot ---- */
        pgL += (targetL - pgL) * kpL;
        double pge = pgL, fmS = 0.0;
        if (fmdL != 0)
        {
            // physical FM at audio rate: of the wind (0), the pipe length (1, below) or the upper lip (2, below)
            fmS = std::sin (phiL);
            if (fmTgtL == 0) pge = pgL * (1.0 + fmdL * fmS);
            phiL += dphiL;
            if (phiL > kTwoPi) phiL -= kTwoPi;
        }
        for (int s = 0; s < footStepsL; ++s)
        {
            const double dp = pge - pfL;
            /* orifice flow q = S sqrt(2 |dp| / rho); P3: linear below kPLin (viscous flow at tiny
               pressure differences). The square-root law is infinitely stiff at zero and its Euler
               steps kept the foot pressure bouncing around 0 after the pallet closed. */
            const double adp = dp < 0 ? -dp : dp, apf = pfL > 0 ? pfL : 0.0;
            double sq[2];      // both square roots in one SSE2 instruction (x86-64)
#if OKL_X86
            _mm_storeu_pd (sq, _mm_sqrt_pd (_mm_set_pd (k2rL * std::max (adp, kPLin), k2rL * std::max (apf, kPLin))));
#else
            sq[1] = std::sqrt (k2rL * std::max (adp, kPLin));
            sq[0] = std::sqrt (k2rL * std::max (apf, kPLin));
#endif
            const double gt = adp < kPLin ? sq[1] * adp / kPLin : sq[1];
            const double gf = apf < kPLin ? sq[0] * apf / kPLin : sq[0];
            const double qt = dp > 0 ? StoeL * gt : -StoeL * gt;
            const double qf = SflueL * gf;
            /* explicit Euler as in labium_core.c while that is stable; P3: a linearised implicit step
               when the foot is stiff (high sound speed / density, tiny pipes), where explicit steps
               oscillate and keep a jet alive */
            const double dgt = adp < kPLin ? sq[1] / kPLin : 0.5 * k2rL / sq[1];
            const double dgf = apf < kPLin ? sq[0] / kPLin : 0.5 * k2rL / sq[0];
            const double stiff = kfStepL * (StoeL * dgt + SflueL * dgf);
            pfL += stiff < 0.5 ? kfStepL * (qt - qf) : kfStepL * (qt - qf) / (1.0 + stiff);
        }
        /* P3: the jet carries flow only with real foot pressure. labium_core.c floors U_j at 1 mm/s
           (for the 1/U_j below); that floor also kept a faint self-sustained oscillation alive after
           the pallet closed, so the flow terms use the unfloored velocity. */
        const double UjFlow = std::sqrt (pfL > 0 ? k2rL * pfL : 0.0);
        UjL = UjFlow + 1e-3;

        /* ---- jet ---- */
        tauL += tauStepL;
        double vdelRaw;
        if (jitL > 0.0)
        {
            // P5: the jet's convection time is not sharp (turbulent jitter, std jit x tau): what reaches the labium is
            // the perturbation averaged over that spread (7-tap Gaussian, taps 2/3 std apart, linear reads)
            const double sd = jitL * tauL, dl = sd * (2.0 / 3.0);
            const double c0 = std::max (3.0 + 3.0 * dl, std::min (tauL - jitGdL, maxTau - 3.0 * dl - 2.0));
            static constexpr double wj[7] { 0.0366, 0.1113, 0.2167, 0.2707, 0.2167, 0.1113, 0.0366 };
            // read positions one buffer length on (c0 + 3 dl < maxTau < vlen): always positive, so truncation is floor
            double pos = (double) (vw + (int) vlen) - (c0 - 3.0 * dl), acc = 0.0;
            for (int k = 0; k < 7; ++k, pos -= dl)
            {
                const int i = (int) pos;
                const double a = vmB[i & vmask];
                acc += wj[k] * (a + (vmB[(i + 1) & vmask] - a) * (pos - (double) i));
            }
            vdelRaw = acc;
        }
        else
        {
            int i;
            double cf[4];
            lagr ((double) vw - tauL, i, cf);
            vdelRaw = cf[0] * vmB[(i - 1) & vmask] + cf[1] * vmB[i & vmask] + cf[2] * vmB[(i + 1) & vmask] + cf[3] * vmB[(i + 2) & vmask];
        }
        vjL += (vdelRaw - vjL) * kjL;
        vj2L += (vjL - vj2L) * kjL;
        rngL ^= rngL << 13; rngL ^= rngL >> 17; rngL ^= rngL << 5;
        nzL += ((2.0 * (rngL * (1.0 / 4294967296.0)) - 1.0) - nzL) * knz;
        const double etaLin = -hGL / UjL * vj2L;
        {
            // v1.0: the jet's swing at the lip as linear growth would make it (mean and variance over about two periods)
            etaMeanL += (etaLin - etaMeanL) * kEtaL;
            const double de = etaLin - etaMeanL;
            etaVarL += (de * de - etaVarL) * kEtaL;
        }
        eta = etaLin * satL + noiseAL * nzL;
        const double y0e = fmTgtL == 2 ? y0cL + lipAmpL * fmdL * fmS : y0cL;
        qin = bHL * UjFlow * (1.0 + fastTanh ((eta - y0e) * invbL));
        const double dq = (qin - qin_prevL) * fsL;
        qin_prevL = qin;
        dq_lpL += (dq - dq_lpL) * kdq;
        double ps = pjKL * dq_lpL;
        if (gate_on) ps += pvKL * vmL * std::abs (vmL);          // vortex loss, labium_core.c form (previous sample's v)
        if (kickL != 0.0)
        {
            if (kick_tL < 0.0 && pfL > kickArmL) kick_tL = 0.0;
            if (kick_tL >= 0.0 && kick_tL < 0.25)
            {
                ps += kickL * pfL * std::sin (kPi * kick_tL / 0.25);
                kick_tL += kickStepL;
            }
        }

        /* ---- waveguide ---- */
        dlineL += dlineStepL;
        // length FM: the bore itself telescopes (+-50 % at full depth)
        const double dRead = fmTgtL == 1 && fmdL != 0.0 ? std::clamp (dlineL * (1.0 + 0.5 * fmdL * fmS), 3.0, maxDline) : dlineL;
        double pplusEnd, pminusM;
        {
            int i;
            double cf[4];
            lagr ((double) dw - dRead, i, cf);
            const int i0 = (i - 1) & dmask, i1 = i & dmask, i2 = (i + 1) & dmask, i3 = (i + 2) & dmask;
            pplusEnd = cf[0] * upB[i0] + cf[1] * upB[i1] + cf[2] * upB[i2] + cf[3] * upB[i3];
            pminusM = cf[0] * loB[i0] + cf[1] * loB[i1] + cf[2] * loB[i2] + cf[3] * loB[i3];
        }
        const double lsh = lb0L * pplusEnd + lb1L * lsh_x1L - la1L * lsh_y1L;
        lsh_x1L = pplusEnd; lsh_y1L = lsh;
        const double dcb = lsh - dc_x1L + 0.99995 * dc_y1L;
        dc_x1L = lsh; dc_y1L = dcb;
        /* open <-> stopped allpass. For an open top its coefficient is 1 - 2e-9: the identity, but with its pole at -1
           a Nyquist residue in its state (left by the morph moving) never died away, and the top radiated it (P5: found
           at -9 dBFS 25 s after a release). There it is the identity outright, with its state kept as that of one. */
        double apm;
        if (amL > 0.9999) { apm = dcb; apm_x1L = dcb; apm_y1L = dcb; }
        else { apm = amL * dcb + apm_x1L - amL * apm_y1L; apm_x1L = dcb; apm_y1L = apm; }
        const double rTop = -apm;

        /* P3: after the pallet has closed the vortex loss -(rho/2)(v/0.6)^2 sgn v is solved implicitly
           with the mouth. The labium_core.c form (previous sample's v, kept while the key is held so
           the sound stays that of Phase 1) can feed a parasitic oscillation near Nyquist at extreme
           settings, which then never dies away.
           v = A + C v|v|, C <= 0  ->  v = sgn(A) 2|A| / (1 + sqrt(1 + 4|C||A|)) */
        if (! gate_on)
        {
            const double G = SpSmL * invZcL * invKm1L;
            const double Av = G * ((KmL - 1.0) * mq_prevL + mu_prevL + ps - 2.0 * pminusM);
            const double Cv = -G * pvKL;                         // |C|
            const double aA = std::abs (Av);
            const double v = 2.0 * aA / (1.0 + std::sqrt (1.0 + 4.0 * Cv * aA));
            const double vNew = Av < 0 ? -v : v;
            ps += pvKL * vNew * std::abs (vNew);
        }
        const double uin = ps - 2.0 * pminusM;
        const double mq = ((KmL - 1.0) * mq_prevL + uin + mu_prevL) * invKm1L;
        mq_prevL = mq;
        mu_prevL = uin;
        const double pplusNew = mq + pminusM;

        loB[dw] = rTop;
        upB[dw] = pplusNew;
        if (malOn && mQ[(size_t) (t / osM)] != 0.0)
        {
            // the volume pushed in at the strike point starts up and down the bore from there
            const double dp = malK * mQ[(size_t) (t / osM)];
            auto add = [&] (double* buf, double back) {
                const double pos = (double) dw - back;
                int i0 = (int) pos;
                if ((double) i0 > pos) --i0;
                const double fr = pos - (double) i0;
                buf[i0 & dmask] += (1.0 - fr) * dp;
                buf[(i0 + 1) & dmask] += fr * dp;
            };
            add (upB, malDs);
            add (loB, std::max (1.0, malDl - malDs));
        }
        double oh = 0.0;
        if (holeOnL)
        {
            /* P5: the side-hole junction: the waves meeting at the hole (an integer number of samples from the
               mouth) share one pressure; flow conservation with the hole's flow (trapezoidal inertance + resistance)
               U' (1 + B + A / 2Y0) = U (1 - B) + A (p + P),  p_j = P - U' / 2Y0,  P = p1+ + p2- */
            const int Dl = (int) std::lround (dRead);
            if (Dl >= 9)
            {
                /* 4 samples clear of both ends: the ends' 4-point reads then only ever see scattered samples. When the
                   hole or the bore end moves on by a sample, one wave sample would meet the junction twice (and that
                   pumped energy into a pipe left alone): both waves pass unscattered for that one sample instead
                   (lossless, so passive). */
                const int Dh = std::clamp (holeDhL, 4, Dl - 4), Dt = Dl - Dh;
                const bool again = holePrevDhL >= 0 && (Dh > holePrevDhL || Dt > holePrevDtL);
                holePrevDhL = Dh; holePrevDtL = Dt;
                if (! again)
                {
                    const int iu = (dw - Dh) & dmask, il = (dw - Dt) & dmask;
                    const double p1 = upB[iu], p2 = loB[il], P = p1 + p2;
                    const double Un = (holeU * (1.0 - holeBL) + holeAL * (holeP + P)) / (1.0 + holeBL + holeAL * holeI2Y0L);
                    const double pj = P - Un * holeI2Y0L;
                    upB[iu] = pj - p2;                                    // on up the bore
                    loB[il] = (pj - p1) * holeGLowL;                      // back down towards the mouth (lower segment's wall loss)
                    oh = radKL * (Un - holeU);                            // the hole radiates its flow (monopole, 1 m)
                    holeU = Un;
                    holeP = pj;
                }
            }
        }
        dw = (dw + 1) & dmask;

        const double um = (pplusNew - pminusM) * invZcL;
        vmL = um * SpSmL;
        const double vmIn = vmL + edgeKL * (qin - bHL * UjFlow) + (crossInL ? crossKL * crossInL[t] : 0.0);   // P5: cross drive
        if (jitL > 0.0)
        {
            jl1L += (vmIn - jl1L) * jitAL;
            jl2L += (jl1L - jl2L) * jitAL;
            vmB[vw] = jl2L;
        }
        else vmB[vw] = vmIn;
        if (vmOutL) vmOutL[t] = vmL;
        vw = (vw + 1) & vmask;
        const double ut = (pplusEnd - rTop) * invZcL;

        /* ---- radiation (1 m, monopole) ---- */
        const double Um = um * SpipeL, Ut = ut * SpipeL;
        om = -radKL * (Um - um_prevL);
        ot = radKL * (Ut - ut_prevL);
        um_prevL = Um;
        ut_prevL = Ut;

        /* ---- stability guard ---- */
        if (! (std::abs (pplusNew) <= 1e6) || ! (std::abs (pfL) <= 1e12))     // (std::isfinite is a CRT call in MSVC)
        {
            std::fill (upB, upB + dlen, 0.0);
            std::fill (loB, loB + dlen, 0.0);
            std::fill (vmB, vmB + vlen, 0.0);
            apm_x1L = apm_y1L = mq_prevL = mu_prevL = vjL = vj2L = jl1L = jl2L = 0.0;
            etaMeanL = etaVarL = 0.0;
            lsh_x1L = lsh_y1L = dc_x1L = dc_y1L = 0.0;
            vmL = dq_lpL = 0.0;
            qin_prevL = 0.0;
            if (! (std::abs (pfL) <= 1e12)) pfL = 0.0;
            if (! (std::abs (pgL) <= 1e12)) pgL = 0.0;
            if (! (std::abs (tauL) <= 1e12)) tauL = -1.0;
            if (! (std::abs (UjL) <= 1e12)) UjL = 1e-3;
            om = ot = 0.0;
            s1x1 = s1x2 = s1y1 = s1y2 = s2x1 = s2x2 = s2y1 = s2y2 = zEnv = zPrev = 0.0;
            holeU = holeP = 0.0;
            ++resets;
        }

        double otH = ot;
        if (topPath)
        {
            // v1.0: the top's sound as it reaches the listener (linear read; the delay glides with the pipe's length)
            topB[topW] = ot;
            topDL += topDStepL;
            const double pos = (double) topW - topDL;
            int i = (int) pos;
            if ((double) i > pos) --i;
            const double fr = pos - (double) i;
            otH = topB[i & tmask] + (topB[(i + 1) & tmask] - topB[i & tmask]) * fr;
            topW = (topW + 1) & tmask;
        }
        mixL[t] += 0.85 * om + 0.35 * otH + 0.6 * oh;
        mixR[t] += 0.35 * om + 0.85 * otH + 0.6 * oh;
        peak = std::max (peak, std::abs (om + oh));
        if (sig) sig[t] = om + oh;
        if (malOn && ! hostMix)
        {
            // (a voice rendered on its own: the wall's sound held at the internal rate)
            const double sh = mShell[(size_t) (t / osM)];
            mixL[t] += 0.6 * sh;
            mixR[t] += 0.6 * sh;
            if (sig) sig[t] = om + sh;
        }

        /* ---- P3: pitch servo: zero crossings of the band-passed mouth radiation (what is heard) ---- */
        {
            const double xr = om - rawMean;
            rawMean += (om - rawMean) * kDc;
            const double y1 = bq0 * xr - bq0 * s1x2 - ba1 * s1y1 - ba2 * s1y2;
            s1x2 = s1x1; s1x1 = xr; s1y2 = s1y1; s1y1 = y1;
            const double z4 = bq0 * y1 - bq0 * s2x2 - ba1 * s2y1 - ba2 * s2y2;
            s2x2 = s2x1; s2x1 = y1; s2y2 = s2y1; s2y1 = z4;
            if (settle > 0) settle -= 1.0;
            else
            {
                // P5: the reference energy without the breath above 6 x the band (the radiated pressure weights it by f,
                // and a breathy low pipe looked as if the band missed its partial: the servo gave up 3 c short)
                rawLp += (xr - rawLp) * kRawLp;
                eRaw += rawLp * rawLp; eBp += z4 * z4;
                if (++eN >= evalLen) servoEval();
            }
            zEnv = std::max (std::abs (z4), zEnv * zEnvDecay);
            if (z4 < -0.3 * zEnv) zArmed = true;
            if (zArmed && zPrev < 0.0 && z4 >= 0.0)
            {
                zArmed = false;
                const double tc = sampleClock - 1.0 + zPrev / (zPrev - z4);
                if (zLast >= 0.0)
                {
                    const double I = tc - zLast;
                    zMinI = std::min (zMinI, I);
                    zMaxI = std::max (zMaxI, I);
                }
                else zFirst = tc;
                zLast = tc;
                ++zCount;
                const double winMin = std::max (0.012 * fsL, 3.0 * fsL / std::max (regime * fb, 1.0));
                if (zCount >= 4 && zLast - zFirst >= winMin)
                {
                    if (zMaxI < 1.3 * zMinI) { irregular = 0; servoMeasure ((zCount - 1) * fsL / (zLast - zFirst), (zLast - zFirst) / fsL); }
                    else { ++irregular; fPrevMeas = 0; measuredLocked = false; }
                    zFirst = zLast; zCount = 1; zMinI = 1e30; zMaxI = 0;
                }
                else if (zLast - zFirst > 0.5 * fsL)
                {
                    zFirst = zLast; zCount = 1; zMinI = 1e30; zMaxI = 0;   // too sparse: start over
                }
            }
            zPrev = z4;
            sampleClock += 1.0;
        }

        /* ---- period capture (focus voice only) ---- */
        if (capOn && capState < 3)
        {
            up.setWritePos (dw); lo.setWritePos (dw);
            capStep (eta, y0cL, b, qin, H, UjL, vmL, dlineL, rho, c);
        }

        if (++fposL == CTRL) fposL = 0;
    }

    up.setWritePos (dw); lo.setWritePos (dw); vml.setWritePos (vw);
    holePrevDh = holePrevDhL; holePrevDt = holePrevDtL;
    holeA = holeAL; holeB = holeBL; holeI2Y0 = holeI2Y0L; holeGLow = holeGLowL; holeDh = holeDhL;
    jitA = jitAL; jitGd = jitGdL;
    pg = pgL; pf = pfL; Uj = UjL; vm = vmL;
    qin_prev = qin_prevL; dq_lp = dq_lpL; lsh_x1 = lsh_x1L; lsh_y1 = lsh_y1L; dc_x1 = dc_x1L; dc_y1 = dc_y1L;
    apm_x1 = apm_x1L; apm_y1 = apm_y1L; mq_prev = mq_prevL; mu_prev = mu_prevL; vj = vjL; vj2 = vj2L; jl1 = jl1L; jl2 = jl2L;
    kick_t = kick_tL; um_prev = um_prevL; ut_prev = ut_prevL; nz = nzL; phi = phiL; tau = tauL;
    dline = dlineL; dlineStep = dlineStepL; tauStep = tauStepL;
    rng = rngL;
    fpos = fposL; footSteps = footStepsL;
    Spipe = SpipeL; SpSm = SpSmL; Sflue = SflueL; Stoe = StoeL; k2r = k2rL; kp = kpL; target = targetL; kfStep = kfStepL;
    fmd = fmdL; dphi = dphiL; kj = kjL; hG = hGL; invb = invbL; bH = bHL; noiseA = noiseAL; y0c = y0cL;
    etaMean = etaMeanL; etaVar = etaVarL; kEtaTrack = kEtaL; topD = topDL; topDStep = topDStepL; jetSatF = satL;
    topl.setWritePos (topW);
    pjK = pjKL; pvK = pvKL; kick = kickL; kickArm = kickArmL; kickStep = kickStepL; edgeK = edgeKL; radK = radKL;
    Km = KmL; invKm1 = invKm1L; invZc = invZcL; lb0 = lb0L; lb1 = lb1L; la1 = la1L; am = amL;

    etaN = (eta - y0c) * invb;
    inflow = Uj > 0 ? qin / (2.0 * bH * Uj) : 0;
}

/* MALLET */
StrikeParams Voice::strikeParams (const ParamVec& p) const
{
    StrikeParams s;
    s.L = p[P_LPHYS];
    s.d = p[P_D];
    s.wall = p[P_WALL];
    s.metal = (int) std::lround (p[P_METAL]);
    s.head = (int) std::lround (p[P_HEAD]);
    s.headD = p[P_HEADD] * 1e-3;
    s.strike = p[P_STRIKE];
    s.damper = p[P_DAMPER];
    s.morph = std::clamp (p[P_MORPH], 0.0, 1.0);
    s.c0 = p[P_C];
    s.rho0 = p[P_RHO];
    s.tempK = p[P_TEMPK];
    return s;
}

void Voice::strike (double v0)
{
    // the target frame: a stroke right after a knob moved hits the pipe as it is now
    mallet.setParams (strikeParams (T));
    mallet.setDamper (false);
    mallet.strike (v0);
}

namespace
{
constexpr double kRegimes[] { 0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 4.0, 5.0, 6.0 };
constexpr int kNumRegimes = (int) (sizeof (kRegimes) / sizeof (kRegimes[0]));
}

/* P3: band-pass (RBJ, 0 dB peak, Q 1.2) at regime x nominal resonance */
void Voice::servoDesign()
{
    fbDesigned = fb;
    const double fc = std::clamp (regime * fb, 5.0, 0.4 * fs);
    const double w0 = kTwoPi * fc / fs, alpha = std::sin (w0) / (2.0 * 1.2), a0 = 1.0 + alpha;
    bq0 = alpha / a0;
    ba1 = -2.0 * std::cos (w0) / a0;
    ba2 = (1.0 - alpha) / a0;
    evalLen = std::max (0.03 * fs, 5.0 * fs / fc);
    kRawLp = 1.0 - std::exp (-kTwoPi * std::min (0.4 * fs, 6.0 * fc) / fs);
}

/* P3: is the band-pass on the component the pipe actually sounds? (energy share; rescan if not) */
void Voice::servoEval()
{
    const double share = eRaw > 1e-30 ? eBp / eRaw : 0.0;
    lastShare = share;
    const bool sounding = gate_on && onset_t >= 8.0 && eRaw > 1e-24;
    eRaw = eBp = eN = 0;
    if (! sounding) { lowShare = 0; return; }
    if (scanIdx >= 0)
    {
        if (share > scanBestShare) { scanBestShare = share; scanBest = scanIdx; }
        if (++scanIdx >= kNumRegimes) { scanIdx = -1; regime = kRegimes[scanBest]; }
        else regime = kRegimes[scanIdx];
        servoDesign();
        settle = 0.5 * evalLen;
        zFirst = zLast = -1; zCount = 0; zMinI = 1e30; zMaxI = 0; fPrevMeas = 0;
        return;
    }
    // wrong component: little energy in the band, or crossings that never come out regular
    if (share < 0.25 || irregular >= 4) ++lowShare; else lowShare = 0;
    if (lowShare >= 2)
    {
        lowShare = 0; irregular = 0; servoV = 0;
        scanIdx = 0; scanBest = 0; scanBestShare = 0;
        regime = kRegimes[0];
        servoDesign();
        settle = 0.5 * evalLen;
        zFirst = zLast = -1; zCount = 0; zMinI = 1e30; zMaxI = 0; fPrevMeas = 0;
    }
}

/* P3: one pitch measurement of the sounding voice -> running estimate of the jet's pitch offset */
void Voice::servoMeasure (double fMeas, double windowSec)
{
    measuredHz = fMeas;
    if (fb <= 0 || ! gate_on || onset_t < 8.0 || scanIdx >= 0)      // attack transient / regime scan
    {
        fPrevMeas = fMeas;
        measuredLocked = false;
        return;
    }
    const double r = fMeas / fb;
    const int k = std::clamp ((int) std::lround (2.0 * r), 1, 16);
    const double cand = 0.5 * k;
    const double dev = 1200.0 * std::log2 (r / cand);
    // capture range: well inside half the distance to the neighbouring k/2 candidates
    const double cUp = 1200.0 * std::log2 ((cand + 0.5) / cand);
    const double cDn = cand > 0.5 ? 1200.0 * std::log2 (cand / (cand - 0.5)) : 1e9;
    const double window = std::min (250.0, 0.45 * std::min (cUp, cDn));
    const bool consistent = fPrevMeas > 0 && std::abs (fMeas / fPrevMeas - 1.0) < 0.01;
    fPrevMeas = fMeas;
    measuredRatio = cand;
    measuredCents = 1200.0 * std::log2 (fMeas / (cand * A[P_FTARGET]));
    measuredLocked = std::abs (dev) < window && consistent;
    if (cand != regime && std::abs (dev) < window)
    {
        // the pipe sounds another regime than the band-pass assumed: re-centre on it
        regime = cand;
        servoDesign();
        settle = 0.5 * evalLen;
        measuredLocked = false;
        fPrevMeas = 0;
        servoV = 0;
        return;
    }
    // correct only from the dominant partial, in limited steps (a large correction can itself
    // change the regime; the next measurements then see the new state)
    if (! measuredLocked || ! servoEnabled || lastShare < 0.3) { servoV *= 0.5; return; }
    // alpha-beta tracker: follows a knob that keeps moving (ramp) without lag, settles when it stops
    const double a = 1.0 - std::exp (-windowSec / 0.05);
    const double bta = 0.5 * a * a / (2.0 - a);
    const double pred = servoD + servoV * windowSec;
    const double resid = dev - pred;
    servoD = pred + std::clamp (a * resid, -20.0, 20.0);
    servoV = std::clamp (0.95 * servoV + bta * resid / windowSec, -150.0, 150.0);
    servoD = std::clamp (servoD, -300.0, 300.0);
}

/* ---------------- period snapshots: K frames over one period ---------------- */
void Voice::requestCapture (double periodSamples)
{
    if (capState) return;
    capT = periodSamples;
    capState = 1;
    capWait = 0;
    capPrev = etaN;
}

void Voice::capStep (double etaV, double y0, double bb, double q, double HH, double U, double v, double dl, double r, double cc)
{
    const double en = (etaV - y0) / bb;
    if (capState == 1)
    {
        ++capWait;
        // start on an upward crossing of the jet deflection (phase-aligned captures)
        if ((capPrev < 0 && en >= 0) || capWait > fs * 0.08) { capState = 2; capN = 0; capK = 0; }
        capPrev = en;
        if (capState != 2) return;
    }
    const int k = capK;
    if (capN >= std::round (k * capT / K_SNAP))
    {
        const double z = r * cc;
        const int o = k * NX_SNAP;
        for (int i = 0; i < NX_SNAP; ++i)
        {
            const double xf = i / (double) (NX_SNAP - 1);
            const double pp = up.readAfter (xf * dl);
            const double pm = lo.readAfter ((1.0 - xf) * dl);
            cap.p[o + i] = (float) (pp + pm);
            cap.u[o + i] = (float) ((pp - pm) / z);
        }
        cap.eta[k] = (float) en;
        cap.inflow[k] = (float) (U > 0 ? q / (2.0 * bb * HH * U) : 0);
        cap.vm[k] = (float) v;
        capK = k + 1;
        if (capK >= K_SNAP)
        {
            capState = 3;           // done: the engine collects it
            cap.midi = midi; cap.periodSamples = capT; cap.fs = fs; cap.dline = dl; cap.Leff = dv0->Leff;
        }
    }
    ++capN;
}

} // namespace okl
