/*
 * OkumuLab 1 — the host's programs: the stops (see Presets.h)
 */
#include "Presets.h"

#include <algorithm>

namespace okl
{

const std::vector<Preset>& factoryPresets()
{
    static const std::vector<Preset> p {
        { "Prinzipal 8'",
          "Round organ metal, open: Normalmensur -2 HT, mouth 1/4 of the circumference, cut up for an intonation number of 2.0 (bass) to "
          "2.4 (treble) at 75 mmWS, played at the reference wind (1.00 = 500 Pa), air at 20 C, pitch lock off: the pipe sounds as built.",
          {} },
        { "Gedackt 8'",
          "Wood, stopped: C 60 x 75 mm inside (Janke), the mouth the full inside width, cut up 5 half-tones above the norm (Janke's rule "
          "at a house organ's wind), side ears, a straight upper lip (Arched cut-up arches it); a leather-sealed stopper, set to tune "
          "each pipe, closes the top (Open/Stopped draws it out). Oak by default for the mallet.",
          {} },
    };
    return p;
}

void presetValues (int index, double* out)
{
    stopValues (std::clamp (index, 0, kNumStops - 1), out);
}

} // namespace okl
