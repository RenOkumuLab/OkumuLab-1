/*
 * OkumuLab 1 — factory presets (P5): "experiment recipes"
 *
 * Each recipe is the canonical Prinzipal 8' (every parameter at its default) with a
 * few knobs moved, so that one physical experiment can be heard: too little or too
 * much wind, other gases, impossible shapes, physical FM, the modulation matrix
 * reacting to the pipe's own state, the struck pipe.
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
    const char* recipe;          // what to listen for (shown on the screen)
    std::vector<PresetValue> values;
};

const std::vector<Preset>& factoryPresets();

/* every parameter of preset i (defaults, then its own values), by parameter index */
void presetValues (int index, double* out);

} // namespace okl
