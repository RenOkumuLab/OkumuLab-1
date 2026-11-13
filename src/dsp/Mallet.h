/*
 * OkumuLab 1 — Mallet mode: the pipe body struck with a hard mallet
 *
 * The pipe body (the cylinder from the mouth to the top) is a thin elastic shell:
 *   - modes: Flügge shell theory (Leissa, NASA SP-288) for every circumferential
 *     order n (0 breathing, 1 beam bending, 2.. ovalling) and axial order m, the
 *     axial shape a beam function (Soedel): clamped at the languid; at the top
 *     free (open pipe) or held by the cap (stopped: tip mass for the bending
 *     modes, a spring on the rim for the others; the morph slides between them)
 *   - material: Young's modulus, Poisson ratio, density, internal loss, and the
 *     thermoelastic loss of the wall in bending (Zener); tin-lead alloys from
 *     tin and lead (volume fractions, Hill average)
 *   - the gas in and around the pipe: added mass of the interior and exterior
 *     gas and radiation damping (cylinder with an axial wavenumber, Hankel /
 *     modified Bessel functions)
 *   - the mallet: a sphere with Hertz contact (stiffness from both materials and
 *     both radii) and Hunt-Crossley damping from the coefficient of restitution
 *     (V2: or a yarn-wound head, the measured power law of a felt mallet);
 *     the contact force is solved implicitly every sample against the modal
 *     driving-point response, so bounces and re-strikes on a ringing pipe follow
 *   - sound: far-field pressure 1 m broadside (cylinder in a rigid cylindrical
 *     baffle, Junger & Feit), and the wall's breathing under the force drives
 *     the air column (volume velocity a^2/(E h) dF/dt at the strike point)
 *
 * Time stepping: every mode is an exact complex one-pole for its free ringing,
 * with a first-order-hold input (exact for a force that is linear over a sample).
 *
 * V2: a wooden pipe (the Gedackt) is a box of four boards; the mallet strikes the front
 * board in the middle of its width. That board is an orthotropic plate (grain along the
 * pipe, flat-sawn: the tangential direction across it, the radial through it), simply
 * supported on its four edges (glued to the side boards, the block and the top), modes
 * sin(m pi x / L) sin(n pi y / b) with the closed-form frequencies of Leissa (NASA SP-160);
 * wood constants from the Wood Handbook (FPL-GTR-190, ch. 5, 12 % moisture). It radiates
 * from its outer face as a monopole of its net volume velocity (the odd-odd modes), and
 * pushes the air in the bore with its static volume compliance under the force.
 *
 * No JUCE in here (also linked into the offline checks).
 */
#pragma once

#include <cstdint>

namespace okl
{

struct MetalProps
{
    const char* name;
    double E, nu, rho, eta;      // Young's modulus [Pa], Poisson ratio, density [kg/m^3], internal loss factor
    double alpha, kth, cp;       // thermal expansion [1/K], conductivity [W/(m K)], specific heat [J/(kg K)]
};
struct HeadProps
{
    const char* name;
    double E, nu, rho, e;        // mallet head: Young's modulus, Poisson ratio, density, coefficient of restitution
    // V2: the contact law F = K x^p: p 1.5 and K = 0 for an elastic sphere (Hertz, K from both materials); a wool head
    // (felt, yarn) is a measured power law instead (K [N/m^p] given, the wall's own dent left out: the wool is far softer)
    double p = 1.5, K = 0.0;
};

/* V2: a wood (Wood Handbook, 12 % moisture content) */
struct WoodProps
{
    const char* name;
    double EL, ETL, ERL, GLTL;   // modulus along the grain [Pa] (bending MOE + 10 %), E_T / E_L, E_R / E_L, G_LT / E_L
    double nuLT, nuRT;           // Poisson ratios
    double rho, eta;             // density [kg/m^3], loss factor
};

constexpr int kNumMetals = 5, kNumHeads = 5, kNumWoods = 4;
const MetalProps& metalProps (int i);
const HeadProps& headProps (int i);
const WoodProps& woodProps (int i);

/* default wall thickness of a pipe of diameter d [m] (before the wall factor) */
double wallThickness (double d);
/* V2: of a wooden pipe of inside width w [m] (Rucz et al. 2023: 10-11 mm boards on their 55-70 mm wide 4' C pipes) */
double woodWallThickness (double w);

/* what the model takes from the voice's control frame */
struct StrikeParams
{
    double L = 0.5, d = 0.05, wall = 1.0;     // body length, diameter [m], wall-thickness factor
    int metal = 1, head = 1;
    double headD = 0.025;                     // mallet head diameter [m]
    double strike = 0.3;                      // strike point, fraction of the body from the mouth
    double damper = 0.6;                      // damper (felt) strength when the key is up, 0..1
    double morph = 0.0;                       // 0 open top .. 1 capped
    double c0 = 343, rho0 = 1.2, tempK = 293.15;
    // V2: a wooden box (its front board struck): inside width and depth [m], the wood
    bool box = false;
    double width = 0.05, depth = 0.06;
    int wood = 0;
};

/* telemetry of the struck pipe (focus voice) */
struct MalletTelemetry
{
    static constexpr int kModes = 16, kForce = 64, kRing = 9, kBend = 16;
    int strikes = 0, contacts = 0, nModes = 0, nActive = 0;
    float v0 = 0, fPeak = 0, tContact = 0, h = 0, K = 0, mass = 0;
    float p = 1.5f;                // V2: the contact law's exponent (K in N/m^p)
    float force[kForce] {};        // last strike: contact force, kForce samples over forceSpan seconds
    float forceSpan = 0;
    float ring[kRing] {};          // radial displacement at the strike height by circumferential order n [m]
    float bend[kBend] {};          // bending (n = 1) deflection along the body, mouth -> top [m]
    struct Mode { float f, level, t60; int n, m; };
    Mode modes[kModes] {};         // strongest radiating modes now (level: dB SPL at 1 m)
    int nShown = 0;
};

class MalletModel
{
public:
    static constexpr int kMaxModes = 64;

    /* the model runs at the host rate (the wall modes reach 20 kHz there; the voice holds its bore input) */
    void prepare (double fs);
    void reset();

    /* control rate: re-tunes the modes when something physical moved */
    void setParams (const StrikeParams& p);
    /* a stroke: the mallet arrives with velocity v0 [m/s] */
    void strike (double v0);
    /* the felt damper (key up, pedal not held) */
    void setDamper (bool on) { damperOn = on; }

    bool active() const { return flight || nActive > 0; }

    /*
     * n internal samples: radiated pressure 1 m broadside [Pa] into shell[],
     * volume velocity pushed into the air column at the strike point [m^3/s] into q[]
     */
    void render (int n, double* shell, double* q);

    void fillTelemetry (MalletTelemetry& t) const;
    double strikeFraction() const { return sp.strike; }
    double lastPeakForce() const { return fPeakLast; }
    double lastContactTime() const { return tContactLast; }
    double contactStiffness() const { return Kh; }
    double malletMass() const { return mh; }
    int modeCount() const { return nSel; }
    int candidateCount() const { return nCand; }            // modes below the top of hearing found at the last design
    int activeModes() const { return nActive; }
    double modeFreq (int i) const { return w0[i] / 6.283185307179586; }
    double modeT60 (int i) const { return sigma[i] > 0 ? 6.907755 / sigma[i] : 1e9; }
    int modeN (int i) const { return mn[i]; }
    int modeM (int i) const { return mm[i]; }
    double modeGain (int i) const { return gAbs[i]; }       // |radiated Pa per m/s of modal velocity|

    // the exact Hertz-free reference used by the checks: driving-point compliance of the selected modes
    double residualCompliance() const { return cRes; }

private:
    void design();                 // candidates, selection, everything
    void retune (bool fluid);      // the selected set for the current parameters
    void discretise (int i);       // pole and input weights of mode i (sigma, w)
    void beamRoots();
    void computeFluid (int i);
    // V2: the struck front board of a wooden box (orthotropic plate)
    void designBox();
    void retuneBox();
    void boxPlate();               // board, stiffnesses, mallet contact for the current parameters
    double boxW (int m, int n) const;
    double D11 = 0, D22 = 0, D12 = 0, D66 = 0, bW = 0.05, ePerp = 1e9, nuPerp = 0.4, cVol = 0;

    double fs = 96000, T = 1.0 / 96000;
    StrikeParams sp, spDesigned, spTuned, spFluid;
    bool designed = false;
    int framesSinceFluid = 0, framesSinceBC = 0;

    // geometry / material of the current design
    double a = 0.025, h = 7e-4, Lb = 0.5, rhoS = 8900, E = 3e10, nu = 0.36, etaInt = 0.005, ms = 6, kk = 0;
    double teDelta = 0, teTau = 0, mTip = 0;

    // beam functions for the two end conditions (index 0: bending modes n = 1, 1: all other n)
    static constexpr int kM = 20;
    double lam[2][kM] {}, sShape[2][kM] {}, xS[2][kM] {}, xD[2][kM] {}, xTip[2][kM] {}, nrm[2][kM] {}, intX[2][kM] {};
    double bcMu = -1, bcK = -1, bcXs = -1;

    // selected modes (structure of arrays; the first nActive ring, the rest are silent)
    int nSel = 0, nActive = 0, nCand = 0;
    alignas(32) double zr[kMaxModes] {}, zi[kMaxModes] {};       // (32: the AVX2 ring, Simd.h)
    alignas(32) double pr[kMaxModes] {}, pim[kMaxModes] {};
    alignas(16) double g0r[kMaxModes] {}, g0i[kMaxModes] {}, g1r[kMaxModes] {}, g1i[kMaxModes] {};
    alignas(16) double wq[kMaxModes] {};               // strike-point displacement per Im z
    alignas(32) double oa[kMaxModes] {}, ob[kMaxModes] {};   // radiated pressure = oa Re z + ob Im z (+ oF F while pushed)
    double oF[kMaxModes] {};       // v2.0: the force's own share of the wall's acceleration, -|G| sin(arg G) / w x phi / M
    int mn[kMaxModes] {}, mm[kMaxModes] {}, mbc[kMaxModes] {};
    double w0[kMaxModes] {}, wS[kMaxModes] {}, sigma[kMaxModes] {}, sigD[kMaxModes] {}, mass[kMaxModes] {}, phiS[kMaxModes] {};
    double radial[kMaxModes] {}, fluidRatio[kMaxModes] {}, etaRad[kMaxModes] {}, fBend[kMaxModes] {};
    double gAbs[kMaxModes] {}, gArg[kMaxModes] {}, ax[kMaxModes] {};
    double cModes = 0, cRes = 0;

    // mallet
    double mh = 0.01, Kh = 1e7, pC = 1.5, eRest = 0.8, lamHC = 0;    // (V2: pC the contact exponent)
    bool flight = false, inContact = false;
    double y = 0, yd = 0, F = 0, delta = 0, flightT = 0, vIn = 0;
    double damperAmt = 0;
    bool damperOn = false;
    double prevF = 0;

    // last strike (telemetry)
    int strikes = 0, contacts = 0;
    double v0Last = 0, fPeakLast = 0, tContactLast = 0, contactStart = -1, clock = 0;
    float forceRec[256] {};
    int forceN = 0;
};

} // namespace okl
