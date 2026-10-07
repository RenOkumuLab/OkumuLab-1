/*
 * OkumuLab 1 — MIDI routing (see MidiRouter.h)
 */
#include "MidiRouter.h"

#include <algorithm>

namespace okl
{

MidiRouter::MidiRouter (Engine& e) : engine (e)
{
    glideIndex = paramIndex ("glide");
    resetCcMap();
    resetPads();
}

void MidiRouter::resetCcMap()
{
    int m[128];
    defaultCcMap (m);
    for (int i = 0; i < 128; ++i) ccMap[(size_t) i].store (m[i]);
}

void MidiRouter::resetPads()
{
    for (auto& p : padA) p.store (0.0f);
    for (auto& p : padB) p.store (0.0f);
}

/* pad bank A: what each pad's pressure pushes (js/midi.js PAD_A) */
void MidiRouter::applyPads (Globals& g) const
{
    const double p0 = padA[0].load(), p1 = padA[1].load(), p2 = padA[2].load(), p3 = padA[3].load();
    const double p4 = padA[4].load(), p5 = padA[5].load(), p6 = padA[6].load(), p7 = padA[7].load();
    if (p0 > 0) g.wind *= 1.0 + 4.0 * p0;
    if (p1 > 0) g.cutup *= 1.0 - 0.55 * p1;
    if (p2 > 0) g.y0b += 2.5 * p2;
    if (p3 > 0) g.morph = std::min (1.0, g.morph + p3);
    if (p4 > 0) g.gas = std::min (1.0, g.gas + p4);
    if (p5 > 0) g.tempC += 70.0 * p5;
    if (p6 > 0) g.fmDepth = std::min (0.8, g.fmDepth + 0.5 * p6);
    if (p7 > 0) g.glide -= 12.0 * p7;
}

void MidiRouter::pad (int bank, int i, double p)
{
    if (bank == 0)
    {
        padA[(size_t) i].store ((float) p);
        return;
    }
    const float was = padB[(size_t) i].load();
    padB[(size_t) i].store ((float) p);
    const int id = kPadPipeBase + i;
    if (p > 0 && ! (was > 0))
    {
        VoiceOverride vo;
        vo.wind = p * kPadWind;
        engine.noteOn (id, kPadBNotes[i], 100, vo);
    }
    else if (p > 0) engine.voiceWind (id, p * kPadWind);
    else engine.noteOff (id);
}

void MidiRouter::handle (const uint8_t* d, int size)
{
    if (size < 1) return;
    const uint8_t st = d[0];
    if (st >= 0xF0) return;                         // system messages
    const int d1 = size > 1 ? d[1] : 0, d2 = size > 2 ? d[2] : 0;
    const int ty = st & 0xF0, ch = st & 0x0F;
    const uint32_t packed = (uint32_t) st | ((uint32_t) d1 << 8) | ((uint32_t) d2 << 16);
    lastMessage.store (packed, std::memory_order_relaxed);
    const uint32_t k = messageCount.load (std::memory_order_relaxed);
    log[(size_t) (k % kLog)].store (packed, std::memory_order_relaxed);
    messageCount.store (k + 1, std::memory_order_release);

    // pads (channel 10): bank A = notes 36..43, bank B = 44..51
    if (ch == padChannel.load (std::memory_order_relaxed) && (ty == 0x90 || ty == 0x80 || ty == 0xA0))
    {
        int bank = -1, i = 0;
        if (d1 >= kPadNotesA && d1 < kPadNotesA + 8) { bank = 0; i = d1 - kPadNotesA; }
        else if (d1 >= kPadNotesB && d1 < kPadNotesB + 8) { bank = 1; i = d1 - kPadNotesB; }
        if (bank >= 0)
        {
            if (ty == 0x90 && d2 > 0) pad (bank, i, std::max (0.15, d2 / 127.0));
            else if (ty == 0xA0) pad (bank, i, std::max (0.05, d2 / 127.0));
            else pad (bank, i, 0.0);
            return;
        }
    }

    switch (ty)
    {
        case 0x90:
            if (d2 > 0) engine.noteOn (d1, d1, d2);
            else engine.noteOff (d1);
            break;
        case 0x80:
            engine.noteOff (d1);
            break;
        case 0xB0:
        {
            if (d1 == 64) { engine.setSustain (d2 >= 64); break; }
            if (d1 == 1) engine.setModWheel (d2 / 127.0);                         // P5: matrix source (also mapped below)
            if (d1 == 120) { engine.allOff (true); resetPads(); break; }        // all sound off
            if (d1 == 123) { engine.allOff (false); break; }                    // all notes off
            const int learn = learnParam.load (std::memory_order_relaxed);
            if (learn >= 0 && learn < kNumParams)
            {
                for (auto& m : ccMap) if (m.load (std::memory_order_relaxed) == learn) m.store (-1, std::memory_order_relaxed);
                ccMap[(size_t) d1].store (learn, std::memory_order_relaxed);
                learnParam.store (-1);
                learnSerial.fetch_add (1);
            }
            const int idx = ccMap[(size_t) d1].load (std::memory_order_relaxed);
            if (idx >= 0 && sink) sink->midiParam (idx, d2 / 127.0);
            break;
        }
        case 0xD0:                                                              // P5: channel pressure -> matrix
            engine.setChannelPressure (d1 / 127.0);
            break;
        case 0xA0:                                                              // P5: key pressure -> matrix
            engine.setPolyPressure (d1, d2 / 127.0);
            break;
        case 0xE0:
        {
            const double pb = (((d2 << 7) | d1) - 8192) / 8192.0;
            if (glideIndex >= 0 && sink) sink->midiParam (glideIndex, 0.5 * (pb + 1.0));
            break;
        }
        default:
            break;
    }
}

} // namespace okl
