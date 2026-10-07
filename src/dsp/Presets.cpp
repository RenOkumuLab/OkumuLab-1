/*
 * OkumuLab 1 — factory presets (see Presets.h)
 */
#include "Presets.h"

#include <algorithm>

namespace okl
{

const std::vector<Preset>& factoryPresets()
{
    // matrix sources: 1 velocity, 2 key, 3 LFO 1, 4 LFO 2, 5 envelope, 6 wheel, 7 pressure, 8 level, 9 overblow, 10 pitch, 11 random
    // matrix targets: 1 wind pressure, 2 cut-up, 3 labium offset, 4 scale, 5 open/stopped, 6 gas, 7 temperature, 8 tremulant, ... 13 side hole
    static const std::vector<Preset> p {
        { "Prinzipal 8'",
          "The canonical pipe at the reference wind it is voiced for (1.00 = 500 Pa); mouth 1/4 of the circumference, cut up for an intonation number of 2.0 (bass) to 2.4 (treble), air at 20 C.",
          {} },
        { "At the edge of speech",
          "Too little wind (0.43 = 216 Pa), pitch lock off: the pipe speaks late, weak and flat, breath around a thin tone.",
          { { "bellows", 0.43 }, { "pitchLock", 0 } } },
        { "Overblown octave",
          "Full wind (5.00 = 2500 Pa) on a mouth cut low (cut-up x0.65): the fast jet no longer fits the fundamental and the pipe jumps to its second mode, an octave up.",
          { { "bellows", 5 }, { "cutup", 0.65 }, { "pitchLock", 0 } } },
        { "Breathing crescendo",
          "The envelope swells the wind of every note from 1.00 to 5.00 (500 to 2500 Pa): louder, brighter, and sharper (pitch lock off).",
          { { "pitchLock", 0 }, { "modSrc1", 5 }, { "modDst1", 1 }, { "modAmt1", 0.34 }, { "envAttack", 1.6 }, { "envDecay", 6 } } },
        { "Helium Prinzipal",
          "Helium in the wind: sound travels almost three times faster, so the same pipe sounds far higher (pitch lock off).",
          { { "gas", 1 }, { "pitchLock", 0 } } },
        { "Cold church, hot church",
          "A slow LFO swings the air from about -10 to 50 C: the pitch drifts with the speed of sound (pitch lock off).",
          { { "pitchLock", 0 }, { "modSrc1", 3 }, { "modDst1", 7 }, { "modAmt1", 0.27 }, { "lfo1Rate", 0.07 } } },
        { "Gedackt",
          "A stopped pipe, scaled wider: an octave lower, the odd harmonics only, the soft hollow tone of a stopped flute.",
          { { "morph", 1 }, { "scaleHT", 6 }, { "cutup", 0.85 } } },
        { "Impossible morph",
          "LFO 1 slides the top between open and stopped: a pipe no organ builder could make.",
          { { "modSrc1", 3 }, { "modDst1", 5 }, { "modAmt1", 1.0 }, { "lfo1Rate", 0.25 } } },
        { "Wind FM bell",
          "The wind pressure modulated at 1.41 times the key frequency: physical FM sidebands from the jet itself.",
          { { "fmTarget", 0 }, { "fmDepth", 0.35 }, { "fmRatio", 1.41 } } },
        { "Telescoping pipe",
          "The pipe length itself modulated at half the key frequency: the resonator breathes in and out.",
          { { "fmTarget", 1 }, { "fmDepth", 0.25 }, { "fmRatio", 0.5 } } },
        { "Lip FM",
          "The upper lip moved across the jet at twice the key frequency: the balance of even and odd harmonics flickers.",
          { { "fmTarget", 2 }, { "fmDepth", 0.6 }, { "fmRatio", 2.0 } } },
        { "Wandering tone hole",
          "An open side hole that LFO 1 slides up and down the body: the pitch follows the hole's acoustic interval, largest near the middle of the bore.",
          { { "sideHole", 0.5 }, { "modSrc1", 3 }, { "modDst1", 13 }, { "modAmt1", 0.45 }, { "lfo1Rate", 0.2 } } },
        { "Overblow closes the pipe",
          "Play c or below. Full wind (5.00) on a mouth cut low (x0.65) makes the pipe overblow; the moment it does, the matrix closes its top, which drops it back down.",
          { { "bellows", 5 }, { "cutup", 0.65 }, { "pitchLock", 0 }, { "modSrc1", 9 }, { "modDst1", 5 }, { "modAmt1", 1.0 } } },
        { "Choir of jets",
          "Every jet is also shaken by the other sounding pipes: chords beat, lock and pull each other.",
          { { "crossDrive", 0.6 }, { "trem", 0.25 } } },
        { "Unnicked chiff",
          "No nicks, a strong starting vortex and turbulent wind: the spitting attack of an unnicked pipe.",
          { { "nicking", 0 }, { "kick", -1.3 }, { "noise", 0.03 } } },
        { "Heavily nicked",
          "Deep nicks on the languid: a calm, slow speech without chiff, a rounder tone.",
          { { "nicking", 1 } } },
        { "Zinc chimes (mallet)",
          "The pipes struck with a brass mallet, made of zinc: bright, long-ringing bending and ovalling modes.",
          { { "excite", 1 }, { "metal", 3 }, { "head", 3 }, { "damper", 0.3 }, { "reverb", 0.35 } } },
        { "Dead lead (mallet)",
          "Lead-rich organ metal struck with hard rubber: a short, dull knock, as tin-lead pipe metal sounds.",
          { { "excite", 1 }, { "metal", 0 }, { "head", 0 } } },
        { "Beyond the compass",
          "The scale extrapolated 12 half-tones wider: play the lowest keys for pipes larger than any 32' stop.",
          { { "scaleHT", 12 }, { "bellows", 2.16 } } },
    };
    return p;
}

void presetValues (int index, double* out)
{
    for (int i = 0; i < kNumParams; ++i) out[i] = kParamDefs[i].def;
    const auto& ps = factoryPresets();
    if (index < 0 || index >= (int) ps.size()) return;
    for (const auto& v : ps[(size_t) index].values)
    {
        const int i = paramIndex (v.id);
        if (i >= 0) out[i] = std::clamp (v.v, kParamDefs[i].min, kParamDefs[i].max);
    }
}

} // namespace okl
