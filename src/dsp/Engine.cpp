/*
 * OkumuLab 1 — engine (see Engine.h)
 */
#include "Engine.h"
#include "FpEnv.h"
#include "Simd.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace okl
{

/* --------------------------------------------------------- decimator */
void Decimator::prepare (int taps)
{
    N = taps | 1;
    const int M = (N - 1) / 2;
    h.assign ((size_t) N, 0.0);
    double sum = 0;
    for (int i = 0; i < N; ++i)
    {
        const int x = i - M;
        const double s = x == 0 ? 0.5 : std::sin (kPi * x / 2.0) / (kPi * x);
        const double w = 0.42 - 0.5 * std::cos (kTwoPi * i / (N - 1)) + 0.08 * std::cos (4.0 * kPi * i / (N - 1));
        h[(size_t) i] = s * w;
        sum += h[(size_t) i];
    }
    nz.clear();
    for (int i = 0; i < N; ++i)
    {
        h[(size_t) i] /= sum;
        if (std::abs (h[(size_t) i]) > 1e-18) nz.push_back (i);
    }
    buf.assign ((size_t) (2 * N), 0.0);
    pos = 0;
}

void Decimator::reset()
{
    std::fill (buf.begin(), buf.end(), 0.0);
    pos = 0;
}

void Decimator::process (const double* in, double* out, int n)
{
    for (int i = 0; i < n; ++i)
    {
        for (int k = 0; k < 2; ++k)
        {
            const double x = in[2 * i + k];
            buf[(size_t) pos] = x;
            buf[(size_t) (pos + N)] = x;
            pos = (pos + 1) % N;
        }
        // buf[pos .. pos+N-1] holds the last N samples, oldest -> newest
        const double* b = buf.data() + pos;
        double acc = 0;
        for (int j : nz) acc += h[(size_t) j] * b[j];
        out[i] = acc;
    }
}

/* ---------------------------------------------------------------- HP2 */
void HP2::prepare (double fc, double fs)
{
    const double w = std::tan (kPi * fc / fs), k = std::sqrt (2.0);
    const double n = 1.0 / (1.0 + k * w + w * w);
    b0 = n; b1 = -2.0 * n; b2 = n;
    a1 = 2.0 * (w * w - 1.0) * n;
    a2 = (1.0 - k * w + w * w) * n;
    reset();
}

/* ------------------------------------------------------------- engine */
namespace
{
/* the floating-point mode the engine computes in, whatever the calling thread uses (a host's thread may round
   otherwise or unmask exceptions): round to nearest, exceptions masked, as on a default thread such as the
   standalone's; audio also flushes denormals. The caller's mode comes back afterwards. (FpEnv.h: x86-64 and ARM64) */
using ScopedFpMode = fpenv::ScopedEngineMode;
} // namespace

Engine::Engine()
{
    const ScopedFpMode fp (false);
    for (int m = 0; m < 128; ++m)
    {
        designs[(size_t) m] = designPrinzipal (m);
        learned[(size_t) m] = 1200.0 * std::log2 (kcalFor (m));
    }
}

void Engine::setupVoice (Voice& v) const
{
    v.jetJitter = jetJitter;
    v.jetSpread = jetSpread;
    v.jetSat = jetSat;
    v.jetLipVoice = jetLipVoice;
    v.jetUjRef = designPrinzipal (60, voicingPa).Uj;       // (every key is voiced at the same foot pressure)
    // v1.0: the listener's elevation (the room's) for the tops' path
    double sinElev = 0.0;
    if (topPath)
    {
        const RoomSpec rs = defaultRoom();
        const double dx = rs.mic[0] - rs.src[0], dy = rs.mic[1] - rs.src[1], dz = rs.src[2] - rs.mic[2];
        sinElev = std::max (0.0, dz / std::sqrt (dx * dx + dy * dy + dz * dz));
    }
    v.topSinElev = sinElev;
}

void Engine::prepare (double hostFs, int maxBlock)
{
    const ScopedFpMode fp (false);
    (void) maxBlock;
    (void) simdLevel();          // P5: the instruction set is chosen here, not on the audio thread
    fsOut = hostFs;
    os = hostFs < 70000.0 ? 2 : 1;
    fs = os * hostFs;
    // v1.0: the canonical pipes voiced for this pressure
    for (int m = 0; m < 128; ++m) designs[(size_t) m] = designPrinzipal (m, voicingPa);
    for (int i = 0; i < kMaxVoices; ++i)
    {
        voices[(size_t) i].prepare (fs, (uint32_t) (17 + i * 7919));
        voices[(size_t) i].setHostRate (fsOut);
        setupVoice (voices[(size_t) i]);
        voices[(size_t) i].active = false;
        voices[(size_t) i].id = -1;
    }
    constexpr int kChunk = 64;
    mixL.assign ((size_t) (2 * kChunk), 0.0); mixR.assign ((size_t) (2 * kChunk), 0.0); sig.assign ((size_t) (2 * kChunk), 0.0);
    dL.assign ((size_t) kChunk, 0.0); dR.assign ((size_t) kChunk, 0.0); dS.assign ((size_t) kChunk, 0.0);
    wL.assign ((size_t) kChunk, 0.0); wR.assign ((size_t) kChunk, 0.0);
    hM.assign ((size_t) kChunk, 0.0); hS.assign ((size_t) kChunk, 0.0);
    crossSum.assign ((size_t) (2 * kChunk), 0.0); crossNext.assign ((size_t) (2 * kChunk), 0.0); crossIn.assign ((size_t) (2 * kChunk), 0.0);
    for (int i = 0; i < kMaxVoices; ++i) { vmPrev[(size_t) i].assign ((size_t) (2 * kChunk), 0.0); vmCur[(size_t) i].assign ((size_t) (2 * kChunk), 0.0); }
    decL.prepare (47); decR.prepare (47); decS.prepare (47);
    hpL.prepare (18.0, fsOut); hpR.prepare (18.0, fsOut);
    reverb.prepare (fsOut);
    scope.assign (4096, 0.0f);
    reset();
}

void Engine::reset()
{
    for (auto& v : voices) { v.active = false; v.id = -1; v.held = false; }
    decL.reset(); decR.reset(); decS.reset();
    hpL.reset(); hpR.reset();
    reverb.clear();
    env = 0; grMin = 1;
    tremPhase = 0;
    focusId = -1;
    nActive = 0;
    sustain = false;
    std::fill (scope.begin(), scope.end(), 0.0f);
    scopePos = 0;
}

void Engine::resetLearnedTuning()
{
    for (int m = 0; m < 128; ++m) learned[(size_t) m] = 1200.0 * std::log2 (kcalFor (m));
}

void Engine::setGlobals (const Globals& g)
{
    G = g;
    gDirty = true;
}

/* ------------------------------------------------------------- P5: modulation matrix */
bool Engine::matrixOn() const
{
    const double s[kModSlots] { G.modSrc1, G.modSrc2, G.modSrc3, G.modSrc4 }, d[kModSlots] { G.modDst1, G.modDst2, G.modDst3, G.modDst4 };
    const double a[kModSlots] { G.modAmt1, G.modAmt2, G.modAmt3, G.modAmt4 };
    for (int k = 0; k < kModSlots; ++k) if (s[k] >= 0.5 && d[k] >= 0.5 && a[k] != 0.0) return true;
    return false;
}

/* a source as one voice sees it: unipolar 0..1 or bipolar -1..1 */
double Engine::modSource (const Voice& v, int src) const
{
    switch (src)
    {
        case MS_VEL: return v.modVel;
        case MS_KEY: return std::clamp ((v.midi - 60) / 48.0, -1.5, 1.5);
        case MS_LFO1: return std::sin (lfoPh[0]);
        case MS_LFO2: return std::sin (lfoPh[1]);
        case MS_ENV:
        {
            const double at = std::max (1e-4, G.envAttack), dc = std::max (1e-3, G.envDecay);
            return v.modEnvT < at ? v.modEnvT / at : std::exp (-(v.modEnvT - at) / dc);
        }
        case MS_WHEEL: return modWheel;
        case MS_PRESSURE: return std::max (chanAT, polyAT[(size_t) (v.midi & 127)]);
        case MS_LEVEL: return std::clamp ((20.0 * std::log10 (std::max (1e-9, v.peakLevel) / 2e-5) - 40.0) / 60.0, 0.0, 1.0);   // 40 .. 100 dB SPL
        case MS_OVERBLOW: return v.modOver;
        case MS_PITCH: return v.measuredHz > 0 ? std::clamp (v.measuredCents / 50.0, -1.0, 1.0) : 0.0;
        case MS_RANDOM: return v.modRnd;
        default: return 0.0;
    }
}

/* the knobs as this voice feels them: each slot moves its destination by amount x source, in the knob's own travel */
void Engine::modulated (const Voice& v, Globals& g) const
{
    g = G;
    const double s[kModSlots] { G.modSrc1, G.modSrc2, G.modSrc3, G.modSrc4 }, d[kModSlots] { G.modDst1, G.modDst2, G.modDst3, G.modDst4 };
    const double a[kModSlots] { G.modAmt1, G.modAmt2, G.modAmt3, G.modAmt4 };
    for (int k = 0; k < kModSlots; ++k)
    {
        const int src = (int) std::lround (s[k]), pi = modDstParam ((int) std::lround (d[k]));
        if (src <= 0 || pi < 0 || a[k] == 0.0) continue;
        const ParamDef& pd = kParamDefs[pi];
        double& f = g.*(pd.field);
        f = fromNorm (pd, std::clamp (toNorm (pd, f) + a[k] * modSource (v, src), 0.0, 1.0));
    }
}

void Engine::target (Voice& v, ParamVec& p) const
{
    if (matrixOn())
    {
        Globals g;
        modulated (v, g);
        voiceParams (v.midi, g, v.vo, designs[(size_t) v.midi], p);
    }
    else voiceParams (v.midi, G, v.vo, designs[(size_t) v.midi], p);
    p[P_GATE] = G.excite >= 0.5 ? 0.0 : v.gate;          // MALLET: no wind; the key only lifts the damper
    /* P5: the physical FM is the player's experiment while the pallet is open: it fades with the release (and a struck
       pipe has none). A bore whose length keeps moving at audio rate pumps its own resonance and would go on sounding
       with the wind gone (the release check found such pipes at -23 dBFS 25 s after the key came up). */
    if (p[P_GATE] <= 0.0) p[P_FMD] = 0.0;
}

/* MALLET: key velocity -> mallet velocity, 0.1 .. 3 m/s (exponential) */
double Engine::malletSpeed (int velocity)
{
    return 0.1 * std::pow (30.0, std::clamp (velocity, 1, 127) / 127.0);
}

Voice* Engine::findVoice (int id)
{
    for (auto& v : voices) if (v.active && v.id == id) return &v;
    return nullptr;
}

Voice* Engine::focusVoice() { return focusId < 0 ? nullptr : findVoice (focusId); }

const Voice* Engine::focusVoice() const
{
    if (focusId < 0) return nullptr;
    for (auto& v : voices) if (v.active && v.id == focusId) return &v;
    return nullptr;
}

void Engine::noteOn (int id, int midi, int vel, const VoiceOverride& voIn)
{
    midi = std::clamp (midi, kMinNote, kMaxNote);
    VoiceOverride vo = voIn;
    if (vo.tau <= 0.0)
    {
        // velocity -> pallet speed: 12 ms at mid velocity, ~4..40 ms range (pad pipes: their pressure is the wind)
        const double x = (vo.wind >= 0.0 ? 100 : vel) / 127.0 - 0.6;
        vo.tau = 0.012 * std::pow (10.0, -x * 1.3 * G.velSens);
    }
    Voice* v = findVoice (id);
    // P5: the note's own modulation sources
    modRng ^= modRng << 13; modRng ^= modRng >> 17; modRng ^= modRng << 5;
    const double rnd = 2.0 * (modRng * (1.0 / 4294967296.0)) - 1.0;
    if (v && v->midi == midi)
    {
        v->gate = 1; v->vo = vo; v->held = false; v->age = ++ageCounter;
        v->modVel = vel / 127.0; v->modRnd = rnd; v->modEnvT = 0; v->modOver = 0;
        target (*v, tmp);
        v->setTarget (tmp);
        if (G.excite >= 0.5) strikeOrQueue (*v, malletSpeed (vel));       // MALLET: the same pipe again
        focusId = id;
        return;
    }
    if (v) { v->active = false; v->id = -1; }
    v = nullptr;
    // a free voice that last played this key first (MALLET: its modes are already worked out)
    for (auto& x : voices) if (! x.active && x.midi == midi) { v = &x; break; }
    if (! v) for (auto& x : voices) if (! x.active) { v = &x; break; }
    if (! v)
    {
        // steal: released voices first (quietest), otherwise the oldest
        double bestScore = 1e300;
        for (auto& x : voices)
        {
            const double score = (x.gate > 0 ? 1e9 + (double) x.age : x.peakLevel);
            if (score < bestScore) { bestScore = score; v = &x; }
        }
    }
    v->id = id; v->midi = midi; v->vo = vo; v->gate = 1; v->held = false; v->age = ++ageCounter;
    v->modVel = vel / 127.0; v->modRnd = rnd; v->modEnvT = 0; v->modOver = 0;
    v->servoEnabled = servoEnabled;
    target (*v, tmp);
    v->start (tmp, learned[(size_t) midi]);
    if (G.excite >= 0.5) strikeOrQueue (*v, malletSpeed (vel));
    focusId = id;
}

/* MALLET: working out a new pipe's modes takes a few hundred microseconds; a chord of new pipes is spread over
   consecutive 64-sample chunks so that no audio block carries more than a quarter of its time in it */
void Engine::strikeOrQueue (Voice& v, double v0)
{
    const double budget = 0.25 * 64.0 / fsOut;
    if (strikeTime < budget || nPend >= (int) pend.size())
    {
        const auto t0 = std::chrono::steady_clock::now();
        v.strike (v0);
        strikeTime += std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
        return;
    }
    pend[(size_t) nPend++] = { v.id, v0 };
}

void Engine::runPendingStrikes()
{
    strikeTime = 0;
    int k = 0;
    const double budget = 0.25 * 64.0 / fsOut;
    for (; k < nPend && strikeTime < budget; ++k)
    {
        Voice* v = findVoice (pend[(size_t) k].id);
        if (! v) continue;
        const auto t0 = std::chrono::steady_clock::now();
        v->strike (pend[(size_t) k].v0);
        strikeTime += std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
    }
    for (int j = k; j < nPend; ++j) pend[(size_t) (j - k)] = pend[(size_t) j];
    nPend -= k;
}

void Engine::noteOff (int id)
{
    Voice* v = findVoice (id);
    if (! v) return;
    if (sustain) { v->held = true; return; }
    v->gate = 0;
    v->setDamper (true);
    target (*v, tmp);
    v->setTarget (tmp);
}

void Engine::voiceWind (int id, double windMMWS)
{
    Voice* v = findVoice (id);
    if (! v) return;
    v->vo.wind = windMMWS;
    target (*v, tmp);
    v->setTarget (tmp);
}

void Engine::setSustain (bool on)
{
    sustain = on;
    if (on) return;
    for (auto& v : voices)
        if (v.active && v.held)
        {
            v.held = false; v.gate = 0;
            v.setDamper (true);
            target (v, tmp);
            v.setTarget (tmp);
        }
}

void Engine::allOff (bool hard)
{
    for (auto& v : voices)
    {
        if (! v.active) continue;
        if (hard) { v.active = false; v.id = -1; }
        else
        {
            v.gate = 0; v.held = false;
            v.setDamper (true);
            target (v, tmp);
            v.setTarget (tmp);
        }
    }
    if (hard) { reverb.clear(); env = 0; nPend = 0; }
}

void Engine::process (float* outL, float* outR, int n)
{
    // flush denormals to zero for the engine's own work, whoever calls it (decaying tails in the room's float
    // convolution and the voices' filters would otherwise crawl), round to nearest whatever the caller's thread uses
    const ScopedFpMode fp (true);
    processFtz (outL, outR, n);
}

void Engine::processFtz (float* outL, float* outR, int n)
{
    if (gDirty)
    {
        gDirty = false;
        for (auto& v : voices)
            if (v.active) { target (v, tmp); v.setTarget (tmp); }
    }
    int done = 0;
    while (done < n)
    {
        const int m = std::min (64, n - done);
        renderChunk (outL + done, outR + done, m);
        done += m;
    }
}

void Engine::renderChunk (float* outL, float* outR, int n)
{
    runPendingStrikes();
    // P5: the matrix moves every voice's knobs once per chunk (the voice glides them over ~12 ms)
    {
        lfoPh[0] = std::fmod (lfoPh[0] + kTwoPi * G.lfo1Rate * n / fsOut, kTwoPi);
        lfoPh[1] = std::fmod (lfoPh[1] + kTwoPi * G.lfo2Rate * n / fsOut, kTwoPi);
        const bool mat = matrixOn();
        const double dt = n / fsOut, kA = 1.0 - std::exp (-dt / 0.01), kR = 1.0 - std::exp (-dt / 0.15);
        for (auto& v : voices)
        {
            if (! v.active) continue;
            v.modEnvT += dt;
            // overblowing: the pipe sounds an upper regime of its resonance (stopped pipes: their 1/2 is the base)
            const double base = v.frame()[P_MORPH] > 0.5 ? 0.5 : 1.0;
            const double reg = v.measuredRatio > 0 ? v.measuredRatio : v.servoRegime();
            const double ob = v.peakLevel > 1e-4 && reg >= 1.45 * base ? 1.0 : 0.0;
            v.modOver += (ob - v.modOver) * (ob > v.modOver ? kA : kR);
            /* the matrix moves a pipe while its key (or the pedal) holds it: after the release the pipe keeps the shape it
               had and dies away. (A bore whose length or sound speed the matrix kept swinging after the wind had gone
               pumped its own resonance, with the side hole open especially, and never fell silent.) */
            if ((mat || matWasOn) && v.gate > 0) { target (v, tmp); v.setTarget (tmp); }
        }
        matWasOn = mat;
    }
    const int ni = os * n;
    std::fill (mixL.begin(), mixL.begin() + ni, 0.0);
    std::fill (mixR.begin(), mixR.begin() + ni, 0.0);
    std::fill (hM.begin(), hM.begin() + n, 0.0);
    std::fill (hS.begin(), hS.begin() + n, 0.0);

    // tremulant (shared wind)
    tremPhase += kTwoPi * G.tremRate * n / fsOut;
    if (tremPhase > kTwoPi) tremPhase -= kTwoPi;
    const double trem = 1.0 + 0.15 * G.trem * std::sin (tremPhase);

    Voice* fv = focusVoice();
    int na = 0;
    // P5: cross drive: each jet also feels the other pipes' mouth flow of the previous chunk
    const bool cross = G.crossDrive > 0.0;
    if (cross) std::fill (crossNext.begin(), crossNext.begin() + ni, 0.0);
    for (int vi = 0; vi < kMaxVoices; ++vi)
    {
        Voice& v = voices[(size_t) vi];
        if (! v.active) continue;
        ++na;
        v.trem = trem;
        v.peak = 0;
        if (cross)
        {
            const double* prev = vmPrev[(size_t) vi].data();
            for (int t = 0; t < ni; ++t) crossIn[(size_t) t] = crossSum[(size_t) t] - prev[t];
            v.crossIn = crossIn.data();
            v.vmOut = vmCur[(size_t) vi].data();
        }
        else { v.crossIn = nullptr; v.vmOut = nullptr; }
        v.render (ni, mixL.data(), mixR.data(), &v == fv ? sig.data() : nullptr, hM.data(), &v == fv ? hS.data() : nullptr);
        if (cross)
        {
            for (int t = 0; t < ni; ++t) crossNext[(size_t) t] += vmCur[(size_t) vi][(size_t) t];
            std::swap (vmPrev[(size_t) vi], vmCur[(size_t) vi]);
        }
        // free silent released voices (MALLET: also a struck pipe that has died away under the held key)
        if (v.gate <= 0 || (G.excite >= 0.5 && ! v.malletRinging()))
        {
            if (v.pg < 0.02 * MMWS && v.peak < 5e-5) v.quiet += ni; else v.quiet = 0;     // -88 dBFS
            if (v.quiet > fs * 0.15) { v.active = false; v.id = -1; }
        }
        else if (v.vo.wind < 0 && v.measuredLocked)
            learned[(size_t) v.midi] = v.servoCents();      // P3: carry the tuning to the next note of this key
        v.peakLevel = v.peak;
    }
    nActive = na;
    if (cross) std::swap (crossSum, crossNext);
    else std::fill (crossSum.begin(), crossSum.end(), 0.0);

    if (os == 2)
    {
        decL.process (mixL.data(), dL.data(), n);
        decR.process (mixR.data(), dR.data(), n);
    }
    else
    {
        std::copy (mixL.begin(), mixL.begin() + n, dL.begin());
        std::copy (mixR.begin(), mixR.begin() + n, dR.begin());
    }
    // MALLET: the struck walls (host rate)
    for (int i = 0; i < n; ++i) { dL[(size_t) i] += 0.6 * hM[(size_t) i]; dR[(size_t) i] += 0.6 * hM[(size_t) i]; }

    // focus signal -> scope, period captures
    if (fv)
    {
        if (os == 2) decS.process (sig.data(), dS.data(), n);
        else std::copy (sig.begin(), sig.begin() + n, dS.begin());
        for (int i = 0; i < n; ++i) dS[(size_t) i] += hS[(size_t) i];
        const int sz = (int) scope.size();
        for (int i = 0; i < n; ++i) { scope[(size_t) scopePos] = (float) dS[(size_t) i]; scopePos = (scopePos + 1) % sz; }
        scopeWritten += (uint64_t) n;

        if (fv->captureReady())
        {
            const PeriodCapture& cp = fv->capture();
            float mx = 1e-12f;
            for (int x = 0; x < NX_SNAP; ++x)
            {
                float a = 0;
                for (int k = 0; k < K_SNAP; ++k) a = std::max (a, std::abs (cp.p[k * NX_SNAP + x]));
                boreP[(size_t) x] = a;
                boreS[(size_t) x] = cp.p[x];
                mx = std::max (mx, a);
            }
            for (int x = 0; x < NX_SNAP; ++x) { boreP[(size_t) x] /= mx; boreS[(size_t) x] /= mx; }
            ++capSerial;
            capOut = cp;                    // the whole period for the Phase 4 screen
            capOutId = fv->id;
            ++capOutSerial;
            fv->releaseCapture();
            capTimer = 0;
        }
        else if (captureEnabled.load (std::memory_order_relaxed))
        {
            capTimer += n / fsOut;
            if (capTimer >= 0.09)
            {
                capTimer = 0;
                const double f = fv->measuredHz > 0 ? fv->measuredHz : std::max (10.0, fv->derived().fres);
                fv->requestCapture (fs / f);
            }
        }
    }

    // output chain
    const double r = std::clamp (G.reverb, 0.0, 1.0);
    for (int i = 0; i < n; ++i)
    {
        dL[(size_t) i] = hpL.run (dL[(size_t) i]) * kGain;
        dR[(size_t) i] = hpR.run (dR[(size_t) i]) * kGain;
    }
    if (r > 0.0) reverb.process (dL.data(), dR.data(), wL.data(), wR.data(), n);
    else { std::fill (wL.begin(), wL.begin() + n, 0.0); std::fill (wR.begin(), wR.begin() + n, 0.0); }
    const double dry = 1.0 - 0.55 * r, wet = 1.2 * r;
    const double out = 0.8 * std::pow (10.0, G.outDb / 20.0);
    // limiter: instant attack, 150 ms release, -1 dBFS ceiling
    const double rel = std::exp (-1.0 / (0.15 * fsOut));
    const double thr = 0.89;
    double e = env, gm = grMin;
    for (int i = 0; i < n; ++i)
    {
        double l = (dry * dL[(size_t) i] + wet * wL[(size_t) i]) * out;
        double rr = (dry * dR[(size_t) i] + wet * wR[(size_t) i]) * out;
        if (! (std::abs (l) <= 1e6) || ! (std::abs (rr) <= 1e6)) { l = rr = 0.0; reverb.clear(); }
        const double a = std::max (std::abs (l), std::abs (rr));
        e = a > e ? a : e * rel + a * (1.0 - rel);
        const double gr = e > thr ? thr / e : 1.0;
        if (gr < gm) gm = gr;
        l *= gr; rr *= gr;
        outL[i] = (float) std::clamp (l, -1.0, 1.0);
        outR[i] = (float) std::clamp (rr, -1.0, 1.0);
    }
    env = e;
    grMin = gm;
}

void Engine::fillTelemetry (Telemetry& t)
{
    t.nActive = nActive;
    t.maxVoices = kMaxVoices;
    t.osFactor = os;
    t.hostFs = fsOut;
    t.gr = (float) grMin;
    grMin = 1.0;
    t.nVox = 0;
    for (const auto& v : voices)
    {
        if (! v.active || t.nVox >= 32) continue;
        t.vox[t.nVox++] = { v.id, v.midi, (float) v.gate, (float) v.peakLevel, (float) v.pf, (uint32_t) v.age };
    }
    t.excite = G.excite >= 0.5 ? 1 : 0;
    const Voice* fv = focusVoice();
    if (! fv) { t.focusGate = 0; t.focusMidi = -1; t.focusId = -1; t.mallet.nShown = 0; return; }
    fv->malletModel().fillTelemetry (t.mallet);
    for (int k = 0; k < MS_COUNT; ++k) t.modSrc[k] = (float) modSource (*fv, k);
    {
        const double s[kModSlots] { G.modSrc1, G.modSrc2, G.modSrc3, G.modSrc4 }, a[kModSlots] { G.modAmt1, G.modAmt2, G.modAmt3, G.modAmt4 };
        for (int k = 0; k < kModSlots; ++k) t.modOut[k] = (float) (a[k] * modSource (*fv, (int) std::lround (s[k])));
    }
    const ParamVec& p = fv->frame();
    const Derived& dv = fv->derived();
    t.focusMidi = fv->midi;
    t.focusGate = fv->gate > 0 ? 1 : 0;
    t.resets = fv->resets;
    t.fTarget = (float) p[P_FTARGET];
    t.fMeas = (float) fv->measuredHz;
    t.cents = (float) fv->measuredCents;
    t.ratio = (float) fv->measuredRatio;
    t.locked = fv->measuredLocked ? 1 : 0;
    t.servoCents = (float) fv->servoCents();
    t.lock = (float) p[P_PLOCK];
    t.Leff = (float) dv.Leff;
    t.d = (float) p[P_D];
    t.W = (float) dv.W;
    t.H = (float) p[P_MOUTHW];
    t.h = (float) p[P_FLUE];
    t.toe = (float) p[P_TOE];
    t.pf = (float) fv->pf;
    t.pchest = (float) p[P_PCHEST];
    t.Uj = (float) fv->Uj;
    t.fres = (float) dv.fres;
    t.morph = (float) p[P_MORPH];
    t.c = (float) p[P_C];
    t.rho = (float) p[P_RHO];
    t.ising = (float) (dv.fres > 0 && dv.W > 0 ? std::sqrt (2.0 * std::max (0.0, fv->pf) * p[P_FLUE] / (p[P_RHO] * dv.W * dv.W * dv.W)) / dv.fres : 0.0);
    t.etaN = (float) fv->etaN;
    t.inflow = (float) fv->inflow;
    t.level = (float) fv->peakLevel;
    t.capSerial = capSerial;
    std::copy (boreP.begin(), boreP.end(), t.boreP);
    std::copy (boreS.begin(), boreS.end(), t.boreSigned);
    t.focusId = fv->id;
    t.pg = (float) fv->pg;
    t.vm = (float) fv->vm;
    t.M = (float) dv.M;
    t.dline = (float) dv.dline;
    t.kappa = (float) p[P_KAPPA];
    t.y0 = (float) p[P_Y0];
    t.ampcap = (float) p[P_AMPCAP];
}

void Engine::copyScope (float* dst, int n) const
{
    const int sz = (int) scope.size();
    n = std::min (n, sz);
    for (int i = 0; i < n; ++i) dst[i] = scope[(size_t) ((scopePos - n + i + sz) % sz)];
}

} // namespace okl
