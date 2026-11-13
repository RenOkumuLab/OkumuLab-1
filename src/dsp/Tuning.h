/*
 * OkumuLab 1 — tuning (v2.0): the temperament the organ is tuned in, or a scale and a keyboard mapping from
 * Scala files (.scl / .kbm, read with the Surge Synth Team's tuning-library, MIT: third_party/tuning-library).
 *
 * A tuning says, for every key, how far its pipe sounds from equal temperament at a1 = 440 Hz [semitones]. The
 * pipe of a key is the canonical pipe of the nearest key of the equal scale, its body cut to the tuned length as
 * an organ builder tunes a pipe (the Length glide does the same in voiceParams), so the mouth, the scale and the
 * stopper are those of a pipe of that pitch. Equal temperament at 440 Hz moves nothing (exactly 0).
 *
 * The historical temperaments are built from their circles of fifths: each fifth pure (3:2) or narrowed by a
 * fraction of the Pythagorean comma (531441:524288) or the syntonic comma (81:80), as the authors gave them
 * (J. M. Barbour, Tuning and Temperament, 1951): Pythagorean (all pure, the wolf G#-Eb), 1/4-comma meantone
 * (Aron 1523: eleven fifths 1/4 syntonic comma narrow, Eb..G#), Werckmeister III (1691: C-G-D-A and B-F# 1/4
 * Pythagorean comma narrow), Kirnberger III (1779: C-G-D-A-E 1/4 syntonic comma, F#-C# a schisma), Vallotti
 * (F-C-G-D-A-E-B 1/6 Pythagorean comma), Young (1800, his second: C-G-D-A-E-B-F# 1/6 Pythagorean comma). An
 * organ is tuned to its pitch at a1, so a1 sounds at the Pitch knob in every temperament.
 *
 * No JUCE in here (also linked into the offline checks).
 */
#pragma once

#include <array>
#include <string>

namespace okl
{

struct Globals;

/* the Temperament knob */
enum Temperament
{
    TEMP_EQUAL, TEMP_PYTHAGOREAN, TEMP_MEANTONE, TEMP_WERCKMEISTER3, TEMP_KIRNBERGER3, TEMP_VALLOTTI, TEMP_YOUNG, TEMP_CUSTOM,
    kNumTemperaments
};
extern const char* const kTemperamentNames[kNumTemperaments];

/* a historical temperament's departure from equal temperament for a pitch class (0 = C), a1 on the pitch [cents] */
double temperamentCents (int temperament, int pitchClass);

/* a scale (and mapping) from Scala files, per key: semitones from equal temperament at a1 = 440 Hz. Without a .kbm the
   scale starts (1/1) on c1, MIDI 60, and a1 (69) is tuned to the Pitch knob (usesPitch); a .kbm brings its own
   reference note and frequency, and may leave keys out */
struct CustomTuning
{
    bool valid = false, usesPitch = true;
    std::array<bool, 128> mapped {};
    std::array<double, 128> semis {};
};

struct TuningInfo
{
    std::string description;     // the .scl's description line
    int notes = 0;               // notes per period of the scale
    std::string error;           // why a file was refused
};

/* the tuning of the Scala texts (scl: the scale, empty = equal temperament; kbm: the mapping, empty = the default
   above). False on a file the parser refuses: info.error says why and out stays as it was. Not for the audio thread */
bool buildCustomTuning (const std::string& scl, const std::string& kbm, CustomTuning& out, TuningInfo& info);

/* the tuning of a key under the knobs (Temperament, Pitch) and a loaded scale: false for a key the mapping leaves out
   (it sounds nothing); semis from equal temperament at 440 Hz. Real-time safe */
bool keyTuning (const Globals& g, const CustomTuning& c, int midi, double& semis);

/* the pipe that sounds a key: the canonical pipe of the nearest key of the equal scale (clamped to 0..127), and what
   is left to cut [semitones] */
int pipeKeyFor (int midi, double semis, double& rest);

} // namespace okl
