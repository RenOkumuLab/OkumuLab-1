/*
 * OkumuLab 1 — MIDI routing (audio thread)
 *
 * Keyboard notes play pipes; the pads on the pad channel (10 by default, the
 * MiniLab 3 user program) are split off:
 *   bank A (notes 36..43): modulation pads — each pad's pressure pushes one
 *                          physical parameter on top of its knob
 *   bank B (notes 44..51): wind pipes — one pipe per pad, pressure = wind
 * Controllers go through a learnable CC map to the parameters; the router
 * hands those changes to its Sink (the processor turns them into host
 * parameter changes). Same behaviour as the Phase 2 screen (js/midi.js, app.js).
 */
#pragma once

#include "Engine.h"
#include "Params.h"

#include <array>
#include <atomic>

namespace okl
{

class MidiRouter
{
public:
    struct Sink
    {
        virtual ~Sink() = default;
        /* a controller moved parameter `index` to normalised value `norm` */
        virtual void midiParam (int index, double norm) = 0;
    };

    static constexpr int kPadNotesA = 36, kPadNotesB = 44;
    static constexpr int kPadPipeBase = 1000;            // engine voice id of the wind pipes
    static constexpr int kPadBNotes[8] { 36, 43, 48, 55, 60, 64, 67, 72 };
    static constexpr double kPadWind = 300.0;           // mmWS at full pressure

    explicit MidiRouter (Engine& e);
    void setSink (Sink* s) { sink = s; }

    void handle (const uint8_t* data, int size);

    /* knob values + pad bank A pressures -> what the voices get */
    void applyPads (Globals& g) const;
    void resetPads();
    void resetCcMap();

    // learnable controller map: CC -> parameter index (-1 none); written by the editor
    std::array<std::atomic<int>, 128> ccMap;
    std::atomic<int> learnParam { -1 };       // next CC is assigned to this parameter
    std::atomic<int> learnSerial { 0 };       // bumps when a CC was learned
    std::atomic<int> padChannel { 9 };        // 0-based MIDI channel of the pads, -1 = off

    // monitor
    std::atomic<uint32_t> lastMessage { 0 };  // status | d1 << 8 | d2 << 16
    std::atomic<uint32_t> messageCount { 0 };
    static constexpr int kLog = 64;           // the last messages (status | d1 << 8 | d2 << 16); entry k is at k % kLog
    std::array<std::atomic<uint32_t>, kLog> log {};
    std::array<std::atomic<float>, 8> padA {}, padB {};

private:
    void pad (int bank, int i, double p);

    Engine& engine;
    Sink* sink = nullptr;
    int glideIndex = -1;
};

} // namespace okl
