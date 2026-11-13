/*
 * OkumuLab 1 — tuning (see Tuning.h)
 */
#include "Tuning.h"
#include "LabiumCore.h"

#include "Tunings.h"

#include <algorithm>
#include <cmath>

namespace okl
{

const char* const kTemperamentNames[kNumTemperaments] { "Equal", "Pythagorean", "Meantone 1/4", "Werckmeister III",
                                                        "Kirnberger III", "Vallotti", "Young", "Custom (.scl)" };

namespace
{
const double kPureFifth = 1200.0 * std::log2 (1.5);
const double kPythComma = 1200.0 * std::log2 (531441.0 / 524288.0);
const double kSyntComma = 1200.0 * std::log2 (81.0 / 80.0);

/* the circle of fifths from Eb: fifth i joins kCircle[i] and kCircle[i + 1] (the twelfth, G#-Eb, closes it) */
constexpr int kCircle[12] { 3, 10, 5, 0, 7, 2, 9, 4, 11, 6, 1, 8 };      // Eb Bb F C G D A E B F# C# G#
enum Fifth { EB_BB, BB_F, F_C, C_G, G_D, D_A, A_E, E_B, B_FS, FS_CS, CS_GS };

using Table = std::array<double, 12>;

/* the pitch classes of a circle whose fifths are each narrowed by narrow[i] cents; their departure from equal
   temperament, with a1 where equal temperament has it */
Table fromFifths (const std::array<double, 11>& narrow)
{
    double pos[12] {};
    int at = 3;                                   // C
    pos[0] = 0.0;
    for (int i = at; i < 11; ++i) pos[kCircle[i + 1]] = pos[kCircle[i]] + kPureFifth - narrow[(size_t) i];
    for (int i = at - 1; i >= 0; --i) pos[kCircle[i]] = pos[kCircle[i + 1]] - (kPureFifth - narrow[(size_t) i]);
    Table dev {};
    for (int pc = 0; pc < 12; ++pc)
    {
        double d = std::fmod (pos[pc] - 100.0 * pc, 1200.0);
        if (d > 600.0) d -= 1200.0;
        if (d <= -600.0) d += 1200.0;
        dev[(size_t) pc] = d;
    }
    const double a = dev[9];
    for (double& d : dev) d -= a;
    return dev;
}

std::array<Table, kNumTemperaments> makeTables()
{
    std::array<Table, kNumTemperaments> t {};
    auto circle = [] (std::initializer_list<int> which, double by) {
        std::array<double, 11> n {};
        for (int f : which) n[(size_t) f] = by;
        return n;
    };
    t[TEMP_PYTHAGOREAN] = fromFifths ({});
    std::array<double, 11> mt {};
    mt.fill (kSyntComma / 4.0);
    t[TEMP_MEANTONE] = fromFifths (mt);
    t[TEMP_WERCKMEISTER3] = fromFifths (circle ({ C_G, G_D, D_A, B_FS }, kPythComma / 4.0));
    auto k3 = circle ({ C_G, G_D, D_A, A_E }, kSyntComma / 4.0);
    k3[FS_CS] = kPythComma - kSyntComma;          // the schisma
    t[TEMP_KIRNBERGER3] = fromFifths (k3);
    t[TEMP_VALLOTTI] = fromFifths (circle ({ F_C, C_G, G_D, D_A, A_E, E_B }, kPythComma / 6.0));
    t[TEMP_YOUNG] = fromFifths (circle ({ C_G, G_D, D_A, A_E, E_B, B_FS }, kPythComma / 6.0));
    return t;                                     // (equal and custom: zeros)
}

const std::array<Table, kNumTemperaments> kTables = makeTables();

/* a .kbm with every line trimmed: the parser takes an unmapped key only as a bare "x" (and a comment only from column 1),
   files written with leading blanks, as .scl files often are, would be refused */
std::string trimmedLines (const std::string& text)
{
    std::string out;
    out.reserve (text.size());
    size_t i = 0;
    while (i <= text.size())
    {
        const size_t e = std::min (text.find ('\n', i), text.size());
        size_t a = i, b = e;
        while (a < b && (text[a] == ' ' || text[a] == '\t')) ++a;
        while (b > a && (text[b - 1] == ' ' || text[b - 1] == '\t' || text[b - 1] == '\r')) --b;
        out.append (text, a, b - a);
        if (e < text.size()) out += '\n';
        i = e + 1;
    }
    return out;
}
} // namespace

double temperamentCents (int temperament, int pitchClass)
{
    if (temperament <= TEMP_EQUAL || temperament >= TEMP_CUSTOM) return 0.0;
    return kTables[(size_t) temperament][(size_t) (((pitchClass % 12) + 12) % 12)];
}

bool buildCustomTuning (const std::string& scl, const std::string& kbm, CustomTuning& out, TuningInfo& info)
{
    try
    {
        const Tunings::Scale s = scl.empty() ? Tunings::evenTemperament12NoteScale() : Tunings::parseSCLData (scl);
        const Tunings::KeyboardMapping k = kbm.empty() ? Tunings::startScaleOnAndTuneNoteTo (60, 69, 440.0) : Tunings::parseKBMData (trimmedLines (kbm));
        const Tunings::Tuning t (s, k);
        CustomTuning c;
        c.valid = true;
        c.usesPitch = kbm.empty();
        for (int m = 0; m < 128; ++m)
        {
            const double f = t.frequencyForMidiNote (m);
            c.mapped[(size_t) m] = t.isMidiNoteMapped (m) && f > 0.0 && std::isfinite (f);
            c.semis[(size_t) m] = c.mapped[(size_t) m] ? 12.0 * std::log2 (f / midiToHz (m)) : 0.0;
        }
        out = c;
        info.description = s.description;
        info.notes = s.count;
        info.error.clear();
        return true;
    }
    catch (const std::exception& e)
    {
        info.error = e.what();
        return false;
    }
}

bool keyTuning (const Globals& g, const CustomTuning& c, int midi, double& semis)
{
    const int m = std::clamp (midi, 0, 127);
    const int t = std::clamp ((int) std::lround (g.temperament), 0, kNumTemperaments - 1);
    double s = 0.0;
    bool pitch = true;
    if (t == TEMP_CUSTOM)
    {
        if (c.valid)
        {
            if (! c.mapped[(size_t) m]) return false;
            s = c.semis[(size_t) m];
            pitch = c.usesPitch;
        }
    }
    else if (t != TEMP_EQUAL) s = temperamentCents (t, m) / 100.0;
    if (pitch && g.pitchA != 440.0) s += 12.0 * std::log2 (g.pitchA / 440.0);
    semis = s;
    return true;
}

int pipeKeyFor (int midi, double semis, double& rest)
{
    const int k = std::clamp ((int) std::lround (midi + semis), 0, 127);
    rest = midi + semis - k;
    return k;
}

} // namespace okl
