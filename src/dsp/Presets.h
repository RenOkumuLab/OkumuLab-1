/*
 * OkumuLab 1 — the host's programs
 *
 * v2.0: the programs are the stops (they replace P5's "experiment recipes"). Selecting one
 * sets the "stop" parameter and every other parameter to that stop as built (stopValues,
 * Params.h): the Prinzipal 8' and the Gedackt 8' each with their own scale, mouth and top.
 */
#pragma once

#include "Params.h"

#include <vector>

namespace okl
{

struct PresetValue { const char* id; double v; };
struct Preset
{
    const char* name;
    const char* recipe;          // what the stop is (shown on the screen)
    std::vector<PresetValue> values;
};

const std::vector<Preset>& factoryPresets();

/* every parameter of program i (= stop i as built), by parameter index */
void presetValues (int index, double* out);

} // namespace okl
