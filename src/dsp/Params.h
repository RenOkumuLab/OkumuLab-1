/*
 * OkumuLab 1 — parameter table (same keys, ranges and defaults as the Phase 2
 * screen, OkumuLab1/js/params.js). The order is the host's parameter order:
 * the eight MIDI CONTROLLER knobs first, then the panels.
 */
#pragma once

#include "LabiumCore.h"

namespace okl
{

enum class Group { Midi, Wind, Voice, Shape, Atmos, Lab, Output, Mallet, Mod };

enum class Fmt { Num, Pct, Mult, Signed, Gas, Morph, Lock, Db, Inv, Choice, Wind };

/* the Wind pressure knob (curve 2): 0.00 .. 1.00 over the first 2/3 of the travel, at the speed of the former
   0 .. 1.5 Bellows knob; 1.00 .. max over the last third, geometric (equal turns, equal factors) */
constexpr double kWindKnee = 2.0 / 3.0;
constexpr double kWindRefPa = 500.0;      // Wind pressure 1.00 = 500 Pa in the windchest

constexpr int kPitchBend = 128;          // pseudo controller number of the pitch-bend wheel

struct ParamDef
{
    const char* id;          // stable host ID (= Phase 2 key)
    const char* name;        // panel label
    const char* tag;         // short physics tag
    Group group;
    int slot;                // MIDI CONTROLLER knob 1..8 (0 = other panel)
    double min, max, def;
    int curve;               // knob travel: 0 linear, 1 logarithmic, 2 the wind knee (kWindKnee)
    bool bipolar;
    Fmt fmt;
    int decimals;
    const char* unit;
    int cc;                  // default controller (-1 none, kPitchBend)
    double Globals::* field;
    const char* const* choices = nullptr;    // Fmt::Choice: names of 0 .. max
};

extern const ParamDef kParamDefs[];
extern const int kNumParams;

int paramIndex (const char* id);
double toNorm (const ParamDef& d, double v);
double fromNorm (const ParamDef& d, double n);
/* display text of a value, e.g. "75.0 mmWS", "He 40 %" (buf >= 32 chars) */
void formatValue (const ParamDef& d, double v, char* buf, bool withUnit = true);
const char* groupTitle (Group g);

/* P5: modulation matrix. Sources (per note: velocity, key, envelope, the pipe's own level / overblowing /
   pitch deviation, random; shared: two LFOs, mod wheel, pressure) and destinations (parameter ids; 0 = off) */
constexpr int kModSlots = 4;
enum ModSrc { MS_OFF, MS_VEL, MS_KEY, MS_LFO1, MS_LFO2, MS_ENV, MS_WHEEL, MS_PRESSURE, MS_LEVEL, MS_OVERBLOW, MS_PITCH, MS_RANDOM, MS_COUNT };
extern const char* const kModSrcNames[MS_COUNT];
extern const char* const kModDstIds[];
extern const int kModDstCount;
/* the parameter index of a destination (-1 off) */
int modDstParam (int dst);

/* default controller map (the MiniLab 3 user program): CC -> parameter index, -1 none */
void defaultCcMap (int (&map)[128]);

} // namespace okl
