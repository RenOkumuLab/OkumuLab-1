/*
 * OkumuLab 1 — parameter table (see Params.h)
 */
#include "Params.h"
#include "FpEnv.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace okl
{

namespace
{
const char* const kExciteNames[] { "Wind", "Mallet" };
const char* const kMetalNames[] { "Common metal 30% Sn", "Spotted metal 50% Sn", "Tin 75% Sn", "Zinc", "Copper" };
const char* const kHeadNames[] { "Hard rubber", "Acrylic", "Boxwood", "Brass" };
const char* const kFmTargetNames[] { "Wind", "Length", "Upper lip" };
const char* const kModDstNames[] { "Off", "Wind pressure", "Cut-up", "Labium offset", "Scale", "Open/Stopped", "Gas", "Temperature", "Tremulant",
                                   "Mouth width", "Turbulence", "Toe hole", "Length glide", "Side hole", "Sound speed", "Density", "FM depth",
                                   "FM ratio", "Jet feedback", "Cross drive", "Wave speed", "Edge tone", "Start pulse", "Nicking", "Wall loss",
                                   "Strike point", "Damper", "Wall" };
}

const char* const kModSrcNames[MS_COUNT] { "Off", "Velocity", "Key", "LFO 1", "LFO 2", "Envelope", "Mod wheel", "Pressure",
                                            "Pipe level", "Overblow", "Pitch deviation", "Random" };
const char* const kModDstIds[] { "off", "bellows", "cutup", "y0b", "scaleHT", "morph", "gas", "tempC", "trem", "mouthFrac", "noise", "toe",
                                 "glide", "sideHole", "cMult", "rhoMult", "fmDepth", "fmRatio", "jetGain", "crossDrive", "kappa", "edge",
                                 "kick", "nicking", "loss", "strikePos", "damper", "wallMult" };
const int kModDstCount = (int) (sizeof (kModDstIds) / sizeof (kModDstIds[0]));

#define F(x) &Globals::x
const ParamDef kParamDefs[] = {
    // ---- 01 MIDI CONTROLLER (knobs 1..8, CC 74 71 76 77 93 18 19 16)
    { "cutup",     "Cut-up",        "W",       Group::Midi, 1, 0.4, 2.5, 1.0, true, false, Fmt::Mult, 2, "", 74, F (cutup) },
    { "y0b",       "Labium offset", "y0",      Group::Midi, 2, -2.5, 2.5, 0.5, false, true, Fmt::Signed, 2, "b", 71, F (y0b) },
    { "scaleHT",   "Scale",         "HT",      Group::Midi, 3, -20, 16, -2, false, false, Fmt::Signed, 1, "HT", 76, F (scaleHT) },
    { "morph",     "Open/Stopped",  "TOP",     Group::Midi, 4, 0, 1, 0, false, false, Fmt::Morph, 0, "%", 77, F (morph) },
    // knob 5: the former Bellows, x 500 Pa (the windchest's reference pressure, Globals::wind)
    { "bellows",   "Wind pressure", "p chest", Group::Midi, 5, 0, 5, 1, 2, false, Fmt::Wind, 2, "", 93, F (bellows) },
    { "gas",       "Gas",           "MIX",     Group::Midi, 6, -1, 1, 0, false, true, Fmt::Gas, 0, "", 18, F (gas) },
    { "tempC",     "Temperature",   "TEMP",    Group::Midi, 7, -30, 80, 20, false, false, Fmt::Num, 0, "\xC2\xB0" "C", 19, F (tempC) },
    { "trem",      "Tremulant",     "DEPTH",   Group::Midi, 8, 0, 1, 0, false, false, Fmt::Pct, 0, "%", 16, F (trem) },
    // ---- 02 WIND
    { "tremRate",  "Trem rate",     "RATE",    Group::Wind, 0, 2, 12, 5.6, false, false, Fmt::Num, 1, "Hz", -1, F (tremRate) },
    { "velSens",   "Pallet speed",  "VELOCITY",Group::Wind, 0, 0, 1, 0.7, false, false, Fmt::Pct, 0, "%", -1, F (velSens) },
    // ---- 03 VOICING
    { "mouthFrac", "Mouth width",   "H",       Group::Voice, 0, 0.1, 0.31, 0.25, false, false, Fmt::Inv, 2, "circ", -1, F (mouthFrac) },
    { "noise",     "Turbulence",    "NOISE",   Group::Voice, 0, 0, 0.06, 0.008, false, false, Fmt::Pct, 1, "%h", -1, F (noise) },
    { "toe",       "Toe hole",      "TOE",     Group::Voice, 0, 0.4, 1.8, 1, true, false, Fmt::Mult, 2, "", -1, F (toe) },
    // ---- 04 GEOMETRY
    { "glide",     "Length glide",  "GLIDE",   Group::Shape, 0, -12, 12, 0, false, true, Fmt::Signed, 2, "st", kPitchBend, F (glide) },
    { "pitchLock", "Pitch lock",    "LOCK",    Group::Shape, 0, 0, 1, 1, false, false, Fmt::Lock, 0, "%", 85, F (pitchLock) },
    // ---- 05 ATMOSPHERE
    { "cMult",     "Sound speed",   "c ONLY",  Group::Atmos, 0, 0.5, 2, 1, true, true, Fmt::Mult, 2, "", -1, F (cMult) },
    { "rhoMult",   "Density",       "rho ONLY",Group::Atmos, 0, 0.2, 5, 1, true, true, Fmt::Mult, 2, "", -1, F (rhoMult) },
    // ---- 06 LAB
    { "fmDepth",   "FM depth",      "DEPTH",   Group::Lab, 0, 0, 0.8, 0, false, false, Fmt::Pct, 0, "%", 1, F (fmDepth) },
    { "fmRatio",   "FM ratio",      "RATIO",   Group::Lab, 0, 0.1, 4, 1, true, false, Fmt::Mult, 2, "f", -1, F (fmRatio) },
    { "jetGain",   "Jet feedback",  "COUPLING",Group::Lab, 0, 0, 3, 1, false, false, Fmt::Mult, 2, "", -1, F (jetGain) },
    { "vFollow",   "Cut-up follow", "FOLLOW",  Group::Lab, 0, 0, 1, 1, false, false, Fmt::Pct, 0, "%", -1, F (vFollow) },
    { "kappa",     "Wave speed",    "KAPPA",   Group::Lab, 0, 0.6, 2.6, 1.36, true, false, Fmt::Num, 2, "", -1, F (kappa) },
    { "edge",      "Edge tone",     "EDGE",    Group::Lab, 0, 0, 0.4, 0, false, false, Fmt::Num, 3, "", -1, F (edge) },
    { "kick",      "Start pulse",   "KICK",    Group::Lab, 0, -1.5, 0.5, -0.5, false, true, Fmt::Signed, 2, "pf", -1, F (kick) },
    { "loss",      "Wall loss",     "LOSS",    Group::Lab, 0, 0.3, 10, 2.5, true, false, Fmt::Num, 2, "", -1, F (loss) },
    // ---- 07 OUTPUT
    { "reverb",    "Reverb",        "ROOM",    Group::Output, 0, 0, 1, 0.25, false, false, Fmt::Pct, 0, "%", 17, F (reverb) },
    { "outDb",     "Output",        "LEVEL",   Group::Output, 0, -30, 6, 0, false, false, Fmt::Db, 1, "dB", -1, F (outDb) },
    // ---- MALLET (appended: the host order of the parameters above stays as it was)
    { "excite",    "Excitation",    "EXCITE",  Group::Mallet, 0, 0, 1, 0, false, false, Fmt::Choice, 0, "", -1, F (excite), kExciteNames },
    { "metal",     "Pipe metal",    "METAL",   Group::Mallet, 0, 0, 4, 1, false, false, Fmt::Choice, 0, "", -1, F (metal), kMetalNames },
    { "wallMult",  "Wall",          "THICK",   Group::Mallet, 0, 0.5, 2, 1, true, false, Fmt::Mult, 2, "", -1, F (wallMult) },
    { "head",      "Mallet head",   "HEAD",    Group::Mallet, 0, 0, 3, 1, false, false, Fmt::Choice, 0, "", -1, F (head), kHeadNames },
    { "headD",     "Head size",     "HEAD D",  Group::Mallet, 0, 10, 50, 25, false, false, Fmt::Num, 0, "mm", -1, F (headD) },
    { "strikePos", "Strike point",  "STRIKE",  Group::Mallet, 0, 0.05, 0.95, 0.3, false, false, Fmt::Pct, 0, "%", -1, F (strikePos) },
    { "damper",    "Damper",        "DAMPER",  Group::Mallet, 0, 0, 1, 0.6, false, false, Fmt::Pct, 0, "%", -1, F (damper) },
    // ---- P5: Lab (appended)
    { "nicking",   "Nicking",       "NICKS",   Group::Voice, 0, 0, 1, 0, false, false, Fmt::Pct, 0, "%", -1, F (nicking) },
    { "fmTarget",  "FM target",     "TARGET",  Group::Lab, 0, 0, 2, 0, false, false, Fmt::Choice, 0, "", -1, F (fmTarget), kFmTargetNames },
    { "crossDrive","Cross drive",   "CROSS",   Group::Lab, 0, 0, 1, 0, false, false, Fmt::Pct, 0, "%", -1, F (crossDrive) },
    { "sideHole",  "Side hole",     "POS",     Group::Shape, 0, 0, 1, 0, false, false, Fmt::Pct, 0, "%", -1, F (sideHole) },
    // ---- P5: modulation matrix
    { "modSrc1", "Mod 1 source", "SRC 1", Group::Mod, 0, 0, MS_COUNT - 1, 0, false, false, Fmt::Choice, 0, "", -1, F (modSrc1), kModSrcNames },
    { "modDst1", "Mod 1 target", "DST 1", Group::Mod, 0, 0, 27, 0, false, false, Fmt::Choice, 0, "", -1, F (modDst1), kModDstNames },
    { "modAmt1", "Mod 1 amount", "AMT 1", Group::Mod, 0, -1, 1, 0, false, true, Fmt::Signed, 2, "", -1, F (modAmt1) },
    { "modSrc2", "Mod 2 source", "SRC 2", Group::Mod, 0, 0, MS_COUNT - 1, 0, false, false, Fmt::Choice, 0, "", -1, F (modSrc2), kModSrcNames },
    { "modDst2", "Mod 2 target", "DST 2", Group::Mod, 0, 0, 27, 0, false, false, Fmt::Choice, 0, "", -1, F (modDst2), kModDstNames },
    { "modAmt2", "Mod 2 amount", "AMT 2", Group::Mod, 0, -1, 1, 0, false, true, Fmt::Signed, 2, "", -1, F (modAmt2) },
    { "modSrc3", "Mod 3 source", "SRC 3", Group::Mod, 0, 0, MS_COUNT - 1, 0, false, false, Fmt::Choice, 0, "", -1, F (modSrc3), kModSrcNames },
    { "modDst3", "Mod 3 target", "DST 3", Group::Mod, 0, 0, 27, 0, false, false, Fmt::Choice, 0, "", -1, F (modDst3), kModDstNames },
    { "modAmt3", "Mod 3 amount", "AMT 3", Group::Mod, 0, -1, 1, 0, false, true, Fmt::Signed, 2, "", -1, F (modAmt3) },
    { "modSrc4", "Mod 4 source", "SRC 4", Group::Mod, 0, 0, MS_COUNT - 1, 0, false, false, Fmt::Choice, 0, "", -1, F (modSrc4), kModSrcNames },
    { "modDst4", "Mod 4 target", "DST 4", Group::Mod, 0, 0, 27, 0, false, false, Fmt::Choice, 0, "", -1, F (modDst4), kModDstNames },
    { "modAmt4", "Mod 4 amount", "AMT 4", Group::Mod, 0, -1, 1, 0, false, true, Fmt::Signed, 2, "", -1, F (modAmt4) },
    { "lfo1Rate",  "LFO 1 rate",    "LFO 1",   Group::Mod, 0, 0.02, 20, 0.5, true, false, Fmt::Num, 2, "Hz", -1, F (lfo1Rate) },
    { "lfo2Rate",  "LFO 2 rate",    "LFO 2",   Group::Mod, 0, 0.02, 20, 3, true, false, Fmt::Num, 2, "Hz", -1, F (lfo2Rate) },
    { "envAttack", "Env attack",    "ATTACK",  Group::Mod, 0, 0.001, 5, 0.05, true, false, Fmt::Num, 3, "s", -1, F (envAttack) },
    { "envDecay",  "Env decay",     "DECAY",   Group::Mod, 0, 0.01, 10, 1, true, false, Fmt::Num, 2, "s", -1, F (envDecay) },
};
#undef F
const int kNumParams = (int) (sizeof (kParamDefs) / sizeof (kParamDefs[0]));

int paramIndex (const char* id)
{
    for (int i = 0; i < kNumParams; ++i)
        if (std::strcmp (kParamDefs[i].id, id) == 0) return i;
    return -1;
}

/* the knob curves run on whatever thread sets a parameter (the host's too): rounded to nearest, exceptions masked,
   so that a value converts the same in every host and in the standalone (the caller's denormal mode is kept) */
namespace
{
using NearestRounding = fpenv::ScopedNearest;
} // namespace

double toNorm (const ParamDef& d, double v)
{
    const NearestRounding nr;
    if (d.curve == 1) return std::log (v / d.min) / std::log (d.max / d.min);
    if (d.curve == 2) return v <= 1.0 ? v * kWindKnee : kWindKnee + (1.0 - kWindKnee) * std::log (v) / std::log (d.max);
    return (v - d.min) / (d.max - d.min);
}

double fromNorm (const ParamDef& d, double n)
{
    const NearestRounding nr;
    n = n < 0 ? 0 : (n > 1 ? 1 : n);
    if (d.curve == 1) return d.min * std::pow (d.max / d.min, n);
    if (d.curve == 2) return n <= kWindKnee ? n / kWindKnee : std::pow (d.max, (n - kWindKnee) / (1.0 - kWindKnee));
    return d.min + n * (d.max - d.min);
}

void formatValue (const ParamDef& d, double v, char* buf, bool withUnit)
{
    char num[32];
    const char* unit = withUnit ? d.unit : "";
    switch (d.fmt)
    {
        case Fmt::Pct:
            std::snprintf (num, sizeof num, "%.*f", d.decimals, v * 100.0);
            break;
        case Fmt::Mult:
            std::snprintf (num, sizeof num, "\xC3\x97%.*f", d.decimals, v);
            break;
        case Fmt::Signed:
            std::snprintf (num, sizeof num, "%+.*f", d.decimals, v);
            break;
        case Fmt::Gas:
            if (v > 0.005) std::snprintf (buf, 32, "He %.0f%%", v * 100.0);
            else if (v < -0.005) std::snprintf (buf, 32, "CO\xE2\x82\x82 %.0f%%", -v * 100.0);
            else std::snprintf (buf, 32, "AIR");
            return;
        case Fmt::Morph:
            if (v < 0.005) { std::snprintf (buf, 32, "OPEN"); return; }
            if (v > 0.995) { std::snprintf (buf, 32, "STOPPED"); return; }
            std::snprintf (num, sizeof num, "%.0f", v * 100.0);
            break;
        case Fmt::Lock:
            std::snprintf (num, sizeof num, "%.0f", v * 100.0);
            break;
        case Fmt::Inv:
            std::snprintf (num, sizeof num, "1/%.*f", d.decimals, v > 0 ? 1.0 / v : 0.0);
            break;
        case Fmt::Db:
            std::snprintf (num, sizeof num, "%+.*f", d.decimals, v);
            break;
        case Fmt::Wind:          // "1.00 (500 Pa)"
            std::snprintf (buf, 32, withUnit ? "%.2f (%.0f Pa)" : "%.2f", v, v * kWindRefPa);
            return;
        case Fmt::Choice:
        {
            const int i = (int) (v + 0.5), n = (int) (d.max + 0.5);
            std::snprintf (buf, 32, "%s", d.choices && i >= 0 && i <= n ? d.choices[i] : "?");
            return;
        }
        case Fmt::Num:
        default:
            if (d.min > 0 && d.max >= 100 && v >= 100) std::snprintf (num, sizeof num, "%.0f", v);
            else std::snprintf (num, sizeof num, "%.*f", d.decimals, v);
            break;
    }
    if (unit && *unit) std::snprintf (buf, 32, "%s %s", num, unit);
    else std::snprintf (buf, 32, "%s", num);
}

const char* groupTitle (Group g)
{
    switch (g)
    {
        case Group::Midi: return "MIDI CONTROLLER";
        case Group::Wind: return "WIND";
        case Group::Voice: return "VOICING";
        case Group::Shape: return "GEOMETRY";
        case Group::Atmos: return "ATMOSPHERE";
        case Group::Lab: return "LAB";
        case Group::Output: return "OUTPUT";
        case Group::Mallet: return "MALLET";
        case Group::Mod: return "MOD MATRIX";
    }
    return "";
}

int modDstParam (int dst)
{
    static int idx[64];
    static bool init = false;
    if (! init)
    {
        for (int i = 0; i < kModDstCount && i < 64; ++i) idx[i] = i == 0 ? -1 : paramIndex (kModDstIds[i]);
        init = true;
    }
    return dst > 0 && dst < kModDstCount ? idx[dst] : -1;
}

void defaultCcMap (int (&map)[128])
{
    for (int& m : map) m = -1;
    for (int i = 0; i < kNumParams; ++i)
        if (kParamDefs[i].cc >= 0 && kParamDefs[i].cc < 128) map[kParamDefs[i].cc] = i;
    // the MiniLab 3 faders keep wind pressure and tremulant (also on knobs 5 and 8); the expression pedal
    // (CC11) stays on the former Bellows, now the Wind pressure knob
    map[82] = paramIndex ("bellows");
    map[11] = paramIndex ("bellows");
    map[83] = paramIndex ("trem");
}

} // namespace okl
