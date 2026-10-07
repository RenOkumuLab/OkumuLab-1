/*
 * OkumuLab 1 — engine: voices, shared wind (tremulant), oversampling, output chain
 *
 * Same structure as the Phase 2 engine (engine-core.js, class Engine): every
 * voice is the canonical Prinzipal 8' pipe of its key deformed by the knobs,
 * voices run at 2x the host rate below 70 kHz (96 kHz internal at 48 kHz, as in
 * Phase 1) and are decimated with a halfband filter.
 *
 * Output chain: 18 Hz high-pass -> reverb (P5: the impulse response of a stone church
 * nave computed from its geometry, surfaces and air, convolved; Reverb.h) -> output level ->
 * limiter (-1 dBFS, instant attack).
 */
#pragma once

#include "LabiumCore.h"
#include "Params.h"
#include "Reverb.h"

#include <array>
#include <atomic>
#include <vector>

namespace okl
{

/* halfband decimator 2:1 (windowed sinc, Blackman, odd length) */
class Decimator
{
public:
    void prepare (int taps = 47);
    void reset();
    /* in: 2n samples -> out: n samples */
    void process (const double* in, double* out, int n);

private:
    std::vector<double> h, buf;
    std::vector<int> nz;       // indices of the non-zero taps
    int N = 0, pos = 0;
};

/* 2nd-order Butterworth high-pass */
struct HP2
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    void prepare (double fc, double fs);
    void reset() { x1 = x2 = y1 = y2 = 0; }
    inline double run (double x)
    {
        const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x; y2 = y1; y1 = y;
        return y;
    }
};

/* what the editor (and the Phase 4 screen) reads; written once per block */
struct Telemetry
{
    int nActive = 0, maxVoices = 0, osFactor = 2;
    float gr = 1.0f;
    double hostFs = 48000;
    // focus pipe (the last key pressed)
    int focusMidi = -1, focusGate = 0, resets = 0;
    float fTarget = 0, fMeas = 0, cents = 0, ratio = 0, servoCents = 0, lock = 1;
    int locked = 0;
    float Leff = 0, d = 0, W = 0, H = 0, h = 0, toe = 0, pf = 0, pchest = 0, Uj = 0, fres = 0, morph = 0, ising = 0, c = 0, rho = 0;
    float etaN = 0, inflow = 0, level = 0;
    // standing wave of the last period capture: |p| envelope along the bore (mouth -> top), normalised
    int capSerial = 0;
    float boreP[NX_SNAP] {};
    float boreSigned[NX_SNAP] {};
    // Phase 4 screen: every sounding voice, and the focus values the Phase 2 telemetry carried
    struct Vox { int id, midi; float gate, peak, pf; uint32_t age; };
    int nVox = 0;
    Vox vox[32] {};
    int focusId = -1;
    float pg = 0, vm = 0, M = 0, dline = 0, kappa = 0, y0 = 0, ampcap = 0;
    // MALLET: the struck pipe in focus
    int excite = 0;
    MalletTelemetry mallet;
    // P5: the modulation sources as the focus voice sees them, and what each matrix slot adds (normalised)
    float modSrc[MS_COUNT] {};
    float modOut[kModSlots] {};
};

class Engine
{
public:
    static constexpr int kMaxVoices = 32;
    static constexpr int kMinNote = 0, kMaxNote = 127;     // P5: the whole MIDI range (pipes beyond the 8' compass)
    static constexpr double kGain = 0.9;            // full scale per Pa at 1 m (labium.py)
    static constexpr double kJetJitter = 0.35;      // P5: turbulent spread of the jet's convection time (std / delay)
    double jetJitter = kJetJitter;                  // (set before prepare; the checks set 0 for the sharp jet of Phase 1)
    /* v1.0: the pipe as its dimensions make it (Voice::jetSpread / jetSat / jetLipVoice / topSinElev; set before prepare).
       The checks set jetSpread = jetSat = jetLipVoice = 0, topPath = false and voicingPa = 75 mmWS for Phase 1's pipe. */
    static constexpr double kJetSpread = 0.04;      // the turbulent jet's half-width grows by 0.04 x across the cut-up
    static constexpr double kJetSat = 2.5;          // its swing at the lip saturates at about its own width (2.5 half-widths)
    static constexpr double kJetLipVoice = 0.5;     // the voiced lip sits half the jet's spread off its axis (with the default
                                                    // labium offset 0.5 b0: half the jet's half-width at the lip)
    static constexpr double kVoicingPa = 500.0;     // the windchest pressure the pipes are voiced for (the reference, 1.00)
    double jetSpread = kJetSpread, jetSat = kJetSat, jetLipVoice = kJetLipVoice, voicingPa = kVoicingPa;
    bool topPath = true;                            // the top's sound arrives over its own path (the room's listener)
    /* a voice's jet and mix as this engine runs them (prepare does this for its own voices; the checks for theirs) */
    void setupVoice (Voice& v) const;

    Engine();
    void prepare (double hostFs, int maxBlock);
    void reset();

    void setGlobals (const Globals& g);
    const Globals& globals() const { return G; }

    /* id: any int identifying the note (keyboard: MIDI note, pad pipes: 1000 + pad) */
    void noteOn (int id, int midi, int velocity, const VoiceOverride& vo = {});
    void noteOff (int id);
    void voiceWind (int id, double windMMWS);
    void setSustain (bool on);
    /* P5: shared modulation sources (0..1) */
    void setModWheel (double v) { modWheel = v; }
    void setChannelPressure (double v) { chanAT = v; }
    void setPolyPressure (int note, double v) { polyAT[(size_t) (note & 127)] = v; }
    void allOff (bool hard);

    void process (float* outL, float* outR, int n);

    /* also restarts the limiter's gain-reduction peak hold */
    void fillTelemetry (Telemetry& t);
    /* last n samples of the focus pipe's mouth signal at the host rate (oldest first) */
    void copyScope (float* dst, int n) const;
    /* samples written to that signal so far (to stream only the new ones) */
    uint64_t scopeCount() const { return scopeWritten; }
    /* the last complete period snapshot of the focus pipe: serial bumps when a new one is ready */
    const PeriodCapture& lastCapture() const { return capOut; }
    int lastCaptureVoiceId() const { return capOutId; }
    uint32_t lastCaptureSerial() const { return capOutSerial; }

    std::atomic<bool> captureEnabled { false };     // period snapshots of the focus pipe
    bool servoEnabled = true;                       // P3 pitch-lock servo (tests switch it off)
    double hostFs() const { return fsOut; }
    int osFactor() const { return os; }
    const RoomInfo& room() const { return reverb.info(); }
    int activeVoices() const { return nActive; }
    const Voice& voice (int i) const { return voices[(size_t) i]; }
    void resetLearnedTuning();
    static double malletSpeed (int velocity);

private:
    void target (Voice& v, ParamVec& p) const;
    Voice* findVoice (int id);
    Voice* focusVoice();
    const Voice* focusVoice() const;
    void renderChunk (float* outL, float* outR, int n);
    void processFtz (float* outL, float* outR, int n);
    bool matrixOn() const;
    double modSource (const Voice& v, int src) const;
    void modulated (const Voice& v, Globals& g) const;
    void strikeOrQueue (Voice& v, double v0);
    void runPendingStrikes();

    std::array<Voice, kMaxVoices> voices;
    std::array<PipeDesign, 128> designs;
    std::array<double, 128> learned {};             // P3: servo correction per key, carried to the next note
    Globals G;
    bool gDirty = false;
    double fsOut = 48000, fs = 96000;
    int os = 2;
    uint64_t ageCounter = 0;
    int focusId = -1, nActive = 0;
    bool sustain = false;
    double tremPhase = 0;

    std::vector<double> mixL, mixR, sig, dL, dR, dS, wL, wR, hM, hS;
    Decimator decL, decR, decS;
    HP2 hpL, hpR;
    RoomReverb reverb;
    double env = 0, grMin = 1;
    double capTimer = 0;
    int capSerial = 0;
    std::array<float, NX_SNAP> boreP {}, boreS {};
    std::vector<float> scope;
    int scopePos = 0;
    uint64_t scopeWritten = 0;
    PeriodCapture capOut;
    int capOutId = -1;
    uint32_t capOutSerial = 0;
    ParamVec tmp {};
    struct PendingStrike { int id; double v0; };
    std::array<PendingStrike, 64> pend {};
    int nPend = 0;
    double strikeTime = 0;           // seconds spent on strokes in this chunk
    // P5: modulation matrix state, cross drive
    double lfoPh[2] {}, modWheel = 0, chanAT = 0;
    std::array<double, 128> polyAT {};
    uint32_t modRng = 0x2545F491u;
    bool matWasOn = false;
    std::vector<double> crossSum, crossNext, crossIn;
    std::array<std::vector<double>, kMaxVoices> vmPrev, vmCur;
};

} // namespace okl
