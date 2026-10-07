/*
 * OkumuLab 1 — Labium DSP core
 *
 * C++ port of the Labium Phase 1 core (proto/labium_core.c: jet-drive flue pipe,
 * bidirectional waveguide with Lagrange interpolation, toe hole + foot cavity)
 * and of the Prinzipal 8' design rules (proto/labium.py), following the
 * real-time structure of the Phase 2 engine (OkumuLab1/js/engine-core.js):
 * control frames of 32 samples, targets glided over ~12 ms, derive() only when
 * something moved.
 *
 * Phase 3 additions are marked "P3:" — the pitch-lock servo (closed-loop tuning
 * that keeps pitch lock 100 % on the key under every modulation), delay lines
 * sized for the internal sample rate, and the voice's mouth-signal tap.
 * Mallet mode ("MALLET:"): the pipe struck instead of blown (Mallet.h); its air
 * column is the same waveguide, driven by the wall's breathing under the stroke.
 *
 * No JUCE in here: the same code is linked into the plugin and into the
 * offline checks (tests/labium_check.cpp).
 */
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "Mallet.h"

namespace okl
{

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;
constexpr double MMWS = 9.80665;      // Pa per mm water column
constexpr int CTRL = 32;              // control-rate block (internal samples)

enum Par
{
    P_GATE, P_PCHEST, P_PALLET_TAU, P_D, P_LPHYS, P_MOUTHW, P_CUTUP, P_FLUE, P_TOE, P_FOOTVOL,
    P_Y0, P_NOISE, P_KAPPA, P_JETGAIN, P_MORPH, P_C, P_RHO, P_GAMMA, P_MU, P_LOSS,
    P_FMF, P_FMD, P_PLOCK, P_FTARGET, P_KCAL, P_AMPCAP, P_EDGE, P_JETBW, P_ONSET_B, P_ONSET_T,
    P_KICK, P_VFOLLOW, P_FDESIGN,
    // MALLET: excitation (0 wind, 1 mallet), pipe metal, wall factor, mallet head, head diameter [mm],
    // strike point (fraction of the body), damper strength, temperature [K]
    P_EXCITE, P_METAL, P_WALL, P_HEAD, P_HEADD, P_STRIKE, P_DAMPER, P_TEMPK,
    // P5: nicking 0..1, FM target (0 wind, 1 length, 2 upper lip), cross drive (other voices shake the jet), side hole (0 off, else position)
    P_NICK, P_FMTGT, P_CROSS, P_HOLE,
    NPAR
};

using ParamVec = std::array<double, NPAR>;

/* ------------------------------------------------------------------ gases */
struct GasProps { double c, rho, gamma, mu; };
GasProps gasProps (double air, double he, double co2, double tempC);
/* gas knob: -1 = CO2, 0 = air, +1 = helium */
GasProps gasForKnob (double gas, double tempC);

double midiToHz (double m);
/* organ (Helmholtz) names: MIDI 36 = C, 48 = c, 60 = c1 ... (writes into buf, >= 8 chars) */
void organName (int midi, char* buf);

/* ---------------------------------------------------------- Prinzipal 8' */
struct PipeDesign
{
    int midi = 60;
    double f0 = 0, d = 0, mouthW = 0, cutup = 0, flue = 0, toe = 0, toeConst = 0, footVol = 0, footLen = 0;
    double lPhys = 0, Leff = 0, M = 0, pChest = 0, pFoot = 0, Uj = 0, ising = 0;
    double c = 0, rho = 0, gamma = 0, mu = 0;
};

/* the canonical pipe of a key: Normalmensur -2 HT, mouth 1/4, air 20 °C (labium.py); cut up for the Ising number at
   the windchest pressure it is voiced for (v1.0: the reference, 500 Pa; Phase 1: 75 mmWS) */
PipeDesign designPrinzipal (int midi, double voicingPa = 500.0);

/* pitch-lock calibration factor of a key (Phase 1 calibrate.py), interpolated / clamped */
double kcalFor (double midi);

/* ------------------------------------------------- performance parameters */
/* what the knobs move (same names and defaults as DEFAULT_G of the Phase 2 engine) */
struct Globals
{
    double wind = 500.0 / MMWS;   // the windchest's reference pressure, 500 Pa [mmWS] (no knob: the checks and pad A1 move it)
    double bellows = 1.0;      // the Wind pressure knob (the former Bellows): x reference, 0 .. 5
    double trem = 0.0;         // tremulant depth 0..1 (±15 % wind)
    double tremRate = 5.6;     // [Hz]
    double velSens = 0.7;      // velocity -> pallet speed
    double cutup = 1.0;        // cut-up factor
    double y0b = 0.5;          // labium offset in jet half-widths b
    double mouthFrac = 0.25;   // mouth width / circumference
    double noise = 0.008;      // jet turbulence
    double toe = 1.0;          // toe-hole diameter factor
    double scaleHT = -2.0;     // scale (Normalmensur half-tones)
    double morph = 0.0;        // 0 open .. 1 stopped
    double glide = 0.0;        // length glide [semitones]
    double pitchLock = 1.0;
    double vFollow = 1.0;      // voicing follow (cut-up tracks the resonance)
    double gas = 0.0;          // -1 CO2 .. 0 air .. +1 He
    double tempC = 20.0;
    double cMult = 1.0;        // speed of sound multiplier (non-physical)
    double rhoMult = 1.0;      // density multiplier (non-physical)
    double fmDepth = 0.0;      // wind FM depth (fraction of pressure)
    double fmRatio = 1.0;      // wind FM frequency / key frequency
    double jetGain = 1.0;      // jet <- resonator feedback gain
    double edge = 0.0;         // edge-tone feedback
    double kick = -0.5;        // starting-vortex pulse
    double loss = 2.5;         // wall-loss multiplier
    double kappa = 1.36;       // jet wave-speed coefficient
    double reverb = 0.25;      // reverb mix 0..1
    double outDb = 0.0;        // output level [dB]
    // MALLET
    double excite = 0.0;       // 0 wind, 1 mallet
    double metal = 1.0;        // pipe metal (index into the material table)
    double wallMult = 1.0;     // wall thickness factor
    double head = 1.0;         // mallet head material (index)
    double headD = 25.0;       // mallet head diameter [mm]
    double strikePos = 0.3;    // strike point, fraction of the body from the mouth
    double damper = 0.6;       // felt damper when the key is up
    // P5: Lab
    double nicking = 0.0;      // nicks on the languid: 0 none .. 1 heavy
    double fmTarget = 0.0;     // physical FM of 0 wind pressure, 1 pipe length, 2 upper-lip position
    double crossDrive = 0.0;   // the other sounding pipes' mouth flow shakes this jet
    double sideHole = 0.0;     // open side hole: 0 closed, else its position along the body
    // P5: modulation matrix (4 slots: source, destination, amount) and its LFOs and envelope
    double modSrc1 = 0, modSrc2 = 0, modSrc3 = 0, modSrc4 = 0;
    double modDst1 = 0, modDst2 = 0, modDst3 = 0, modDst4 = 0;
    double modAmt1 = 0, modAmt2 = 0, modAmt3 = 0, modAmt4 = 0;
    double lfo1Rate = 0.5, lfo2Rate = 3.0;    // [Hz]
    double envAttack = 0.05, envDecay = 1.0;  // [s]
};

/* per-voice overrides: wind (mmWS, pad pipes; < 0 = none), pallet time constant (0 = default) */
struct VoiceOverride
{
    double wind = -1.0;
    double tau = 0.0;
};

/* control vector of one voice: the canonical pipe of its key, deformed by the knobs */
void voiceParams (int midi, const Globals& G, const VoiceOverride& vo, const PipeDesign& pd, ParamVec& v);

/* ------------------------------------------------------------- derived */
struct Derived
{
    double dline = 3, lb0 = 1, lb1 = 0, la1 = 0, lpDelay = 0, taum = 0, aMorph = 0, M = 0, Leff = 0, fres = 0, W = 0;
    double holeR = 1, holeX = 0;         // P5: first resonance with the open side hole / without; its place on the bore (m)
    double morphFor = -1, fsFor = 0;     // aMorph cache key
};
void derive (const double* p, double fs, Derived& dv);

/* ----------------------------------------------------------- delay line */
class DelayLine
{
public:
    void allocate (int minLen);
    void clear();
    int length() const { return mask + 1; }
    double* data() { return buf.data(); }
    int maskBits() const { return mask; }
    int writePos() const { return w; }
    void setWritePos (int p) { w = p & mask; }

    /* 3rd-order Lagrange read, D samples ago (before this sample's write), D >= 1 */
    inline double read (double D) const
    {
        int i;
        double c[4];
        lagrange (D, i, c);
        const double* b = buf.data();
        return c[0] * b[(i - 1) & mask] + c[1] * b[i & mask] + c[2] * b[(i + 1) & mask] + c[3] * b[(i + 2) & mask];
    }
    /* the same read from two lines written in lockstep (same length and write position) */
    static inline void read2 (const DelayLine& a, const DelayLine& b, double D, double& ya, double& yb)
    {
        int i;
        double c[4];
        a.lagrange (D, i, c);
        const int m = a.mask;
        const int i0 = (i - 1) & m, i1 = i & m, i2 = (i + 1) & m, i3 = (i + 2) & m;
        const double* p = a.buf.data();
        const double* q = b.buf.data();
        ya = c[0] * p[i0] + c[1] * p[i1] + c[2] * p[i2] + c[3] * p[i3];
        yb = c[0] * q[i0] + c[1] * q[i1] + c[2] * q[i2] + c[3] * q[i3];
    }
    /* linear read relative to the most recent write (snapshots) */
    inline double readAfter (double D) const
    {
        const double pos = (double) (w - 1) - D;
        const double fl = std::floor (pos);
        const int i = (int) fl;
        const double f = pos - fl;
        return buf[(size_t) (i & mask)] * (1.0 - f) + buf[(size_t) ((i + 1) & mask)] * f;
    }
    inline void write (double x) { buf[(size_t) w] = x; w = (w + 1) & mask; }

private:
    inline void lagrange (double D, int& i, double* c) const
    {
        const double pos = (double) w - D;
        i = (int) pos;                       // floor without the library call (pos may be negative)
        if ((double) i > pos) --i;
        const double f = pos - (double) i;
        c[0] = -f * (f - 1.0) * (f - 2.0) / 6.0;
        c[1] = (f + 1.0) * (f - 1.0) * (f - 2.0) / 2.0;
        c[2] = -(f + 1.0) * f * (f - 2.0) / 2.0;
        c[3] = (f + 1.0) * f * (f - 1.0) / 6.0;
    }

    std::vector<double> buf;
    int mask = 0, w = 0;
};

/* ---------------------------------------------------------------- voice */
constexpr int K_SNAP = 48, NX_SNAP = 48;    // period snapshots: frames x points along the bore

struct PeriodCapture
{
    int midi = 0;
    double periodSamples = 0, fs = 0, dline = 0, Leff = 0;
    float p[K_SNAP * NX_SNAP] {}, u[K_SNAP * NX_SNAP] {};
    float eta[K_SNAP] {}, inflow[K_SNAP] {}, vm[K_SNAP] {};
};

class Voice
{
public:
    /* allocation happens here (never on the audio thread) */
    void prepare (double fs, uint32_t seed);
    void reset();

    /* start from silence (fresh pallet). servoCents: initial pitch-lock correction (P3) */
    void start (const ParamVec& target, double servoCents);
    void setTarget (const ParamVec& target);

    /* MALLET: the struck wall runs at the host rate (call after prepare) */
    void setHostRate (double hostFs);

    /*
     * Render n internal samples, adding the stereo mix into mixL / mixR.
     * sig (optional) receives the mouth signal (focus voice).
     * MALLET: hostMix / hostSig (n / oversampling samples at the host rate) take the struck wall's radiation;
     * without them it is held into the internal-rate mix.
     */
    void render (int n, double* mixL, double* mixR, double* sig, double* hostMix = nullptr, double* hostSig = nullptr);

    /* period snapshot of the bore (focus voice): request, then poll captureReady() */
    void requestCapture (double periodSamples);
    bool captureReady() const { return capState == 3; }
    const PeriodCapture& capture() const { return cap; }
    void releaseCapture() { capState = 0; }

    /* MALLET: a stroke (mallet velocity [m/s]) and the felt damper (key up) */
    void strike (double v0);
    void setDamper (bool on) { mallet.setDamper (on); }
    bool malletRinging() const { return mallet.active(); }
    const MalletModel& malletModel() const { return mallet; }

    /* P5: turbulent jitter of the jet's convection time (std / mean delay; 0 = the sharp delay of Phase 1) */
    double jetJitter = 0.0;
    /* v1.0: the jet as an organ pipe's mouth makes it (all 0 = Phase 1's jet, as for a recorder's short mouth).
       jetSpread: rate at which the jet's half-width grows across the cut-up when the jet is turbulent, b = b0 + s x (it is
       scaled down to 0 for a laminar jet, Reynolds number 500); jetSat: its swing at the lip saturates at that many
       half-widths there; jetLipVoice: the voiced upper lip sits that share of the jet's spread off its axis (at the
       reference jet velocity jetUjRef [m/s], the pipe as voiced); topSinElev: sine of the listener's elevation, so the open
       top's sound arrives later by the pipe's length x that (0 = no path difference) */
    double jetSpread = 0.0, jetSat = 0.0, jetLipVoice = 0.0, jetUjRef = 0.0, topSinElev = 0.0;
    /* P5: cross drive: the other voices' mouth velocity for this block (internal rate, set by the engine; null = none),
       and where this voice leaves its own mouth velocity */
    const double* crossIn = nullptr;
    double* vmOut = nullptr;
    /* P5: modulation-matrix state of the note (engine bookkeeping) */
    double modVel = 0, modRnd = 0, modEnvT = 0, modOver = 0;

    /* P3: pitch-lock servo */
    bool servoEnabled = true;
    double servoCents() const { return servoD; }
    double servoRegime() const { return regime; }
    bool servoScanning() const { return scanIdx >= 0; }
    double servoShare() const { return lastShare; }

    // identity / bookkeeping (engine)
    bool active = false, held = false;
    int id = -1, midi = 60;
    double gate = 0;
    uint64_t age = 0;
    VoiceOverride vo;
    double peakLevel = 0, quiet = 0;
    double trem = 1.0;          // shared tremulant factor on the wind

    // telemetry (last sample / last frame)
    double fs = 96000;
    double pg = 0, pf = 0, Uj = 0, vm = 0, etaN = 0, inflow = 0.5, peak = 0;
    int resets = 0;
    const ParamVec& frame() const { return A; }
    double jetSwing() const { return std::sqrt (2.0 * etaVar); }      // v1.0: the jet's swing amplitude at the lip [m]
    double jetHalfWidth() const { return b; }                         // and its half-width there [m]
    double jetOffset() const { return y0c; }                          // the lip's offset from the jet's axis [m]
    const Derived& derived() const { return *dv0; }
    double measuredHz = 0;      // P3: last servo pitch measurement (0 = none)
    double measuredRatio = 0;   // harmonic candidate it was locked to (k / 2 of the nominal resonance)
    double measuredCents = 0;   // sounding pitch - (candidate x key target) [cents]
    bool measuredLocked = false;

private:
    void nextFrame();
    void capStep (double eta, double y0c, double b, double qin, double H, double Uj, double vm, double dline, double rho, double c);
    void servoMeasure (double fMeas, double windowSec);
    StrikeParams strikeParams (const ParamVec& p) const;

    DelayLine up, lo, vml;
    MalletModel mallet;
    int osM = 1;                          // internal samples per host sample
    std::vector<double> mShell, mQ;      // MALLET: per block, wall radiation [Pa] and volume velocity into the bore
    double maxTau = 0, maxDline = 0;
    ParamVec A {}, B {}, T {};
    Derived dvA, dvB;
    Derived* dv0 = &dvA;
    Derived* dv1 = &dvB;
    uint32_t rng = 1, seed0 = 1;
    double k_dq = 0, k_nz = 0, smooth = 0;

    // state (labium_core.c names)
    double qin_prev = 0, dq_lp = 0, lsh_x1 = 0, lsh_y1 = 0, dc_x1 = 0, dc_y1 = 0;
    double apm_x1 = 0, apm_y1 = 0, mq_prev = 0, mu_prev = 0, vj = 0, vj2 = 0;
    double jl1 = 0, jl2 = 0;            // P5: the low-pass ahead of the jitter taps (no grating lobes)
    double jitA = 1.0, jitGd = 0.0;     // its coefficient and group delay (frame constants: a control frame can span two renders)
    int gate_on = 0;
    double onset_t = 0, kick_t = -1, um_prev = 0, ut_prev = 0, nz = 0, phi = 0;
    int fpos = 0;
    double tau = -1;

    // frame constants
    double c = 343, rho = 1.2, zc = 412, Spipe = 0, SpSm = 0, Sflue = 0, Stoe = 0, kf = 0, k2r = 0, kp = 0, target = 0;
    double fmd = 0, dphi = 0, kj = 0, hG = 0, b = 1e-4, invb = 1e4, bH = 0, noiseA = 0, y0c = 0, H = 0, pjK = 0, pvK = 0;
    double kick = 0, kickArm = 0, kickStep = 0, edgeK = 0, radK = 0, Km = 1, lb0 = 1, lb1 = 0, la1 = 0, am = 0;
    double dline = 3, dlineStep = 0, tauStep = 0;
    double invKm1 = 1, invZc = 1, kfStep = 0;
    int footSteps = 4;

    // P3: pitch-lock servo. The mouth velocity goes through a band-pass centred
    // on the regime the pipe sounds in (m x the nominal resonance c / 2 L_eff,
    // m = 1/2, 1, 3/2, 2 ...; found by the band-pass energy share, rescanned when
    // the pipe changes regime), its upward zero crossings are timed, and the
    // measured pitch is compared with the nearest k/2 multiple of the nominal
    // resonance. The difference is the pitch offset the jet / mouth adds (what
    // k_cal calibrates for the reference state); its running estimate replaces
    // k_cal in the pitch-lock length.
    void servoDesign();
    void servoEval();
    double servoD = 0, servoApplied = 0, servoV = 0;      // offset [cents] and its rate [cents/s] (alpha-beta tracker)
    double fb = 0, fbDesigned = 0, regime = 1.0;
    double rawLp = 0, kRawLp = 1;              // P5: the servo's reference energy, low-passed at 6 x the band
    double bq0 = 0, bq2 = 0, ba1 = 0, ba2 = 0;               // band-pass biquad (b1 = 0), applied twice
    double s1x1 = 0, s1x2 = 0, s1y1 = 0, s1y2 = 0, s2x1 = 0, s2x2 = 0, s2y1 = 0, s2y2 = 0;
    double rawMean = 0, kDc = 0, eRaw = 0, eBp = 0, eN = 0, evalLen = 0, settle = 0;
    int lowShare = 0, scanIdx = -1, scanBest = 0, irregular = 0;
    double scanBestShare = 0, lastShare = 0;
    double zEnv = 0, zPrev = 0, zEnvDecay = 0;
    bool zArmed = false;
    double sampleClock = 0, zFirst = -1, zLast = -1, zMinI = 1e30, zMaxI = 0;
    int zCount = 0;
    double fPrevMeas = 0;

    // P5: side hole (junction state: hole volume flow and junction pressure, opening 0..1)
    double holeU = 0, holeP = 0, holeOpen = 0, holeUprev = 0;
    int holePrevDh = -1, holePrevDt = -1;     // the junction's place last sample (a step there passes unscattered)
    double holeA = 0, holeB = 0, holeI2Y0 = 0, holeGLow = 1.0;     // the junction's frame constants
    int holeDh = 2;

    // v1.0: the jet's swing at the lip (running mean and variance of the deflection), and the open top's sound on its
    // way to the listener (delay line, read the pipe's length x topSinElev later)
    double etaMean = 0, etaVar = 0, kEtaTrack = 0, topD = 0, topDStep = 0, jetSatF = 1.0;
    DelayLine topl;

    // period capture
    int capState = 0, capK = 0;
    double capT = 0, capWait = 0, capPrev = 0, capN = 0;
    PeriodCapture cap;
};

} // namespace okl
