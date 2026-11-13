/*
 * OkumuLab 1 — plugin processor
 */
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "WebEditor.h"
#include "Presets.h"
#include "FpEnv.h"

#include <cstring>

using namespace okl;

namespace
{
/* the floating-point mode of every block, whatever the host's audio thread uses: round to nearest, exceptions
   masked, denormals flushed to zero, as the standalone's own audio thread runs (another rounding mode set by a
   host changed the sound engine's results; the MIDI router's note-on work runs here too); the host's mode comes
   back after the block (FpEnv.h: x86-64 and ARM64) */
struct ScopedEngineFpMode : fpenv::ScopedEngineMode
{
    ScopedEngineFpMode() : fpenv::ScopedEngineMode (true) {}
};

juce::NormalisableRange<float> rangeFor (const ParamDef& d)
{
    if (d.curve != 0)            // logarithmic, or the wind knee
    {
        const ParamDef* pd = &d;
        return { (float) d.min, (float) d.max,
                 [pd] (float, float, float n) { return (float) fromNorm (*pd, n); },
                 [pd] (float, float, float v) { return (float) toNorm (*pd, v); },
                 [] (float lo, float hi, float v) { return juce::jlimit (lo, hi, v); } };
    }
    return { (float) d.min, (float) d.max };
}

/* OKL_RENDERTEST=<events.txt>|<out.wav> (tests only): the notes of a diagnostic log ("<t> midi 9x nn vv" lines) played
   offline through a private processor of this binary (its own processBlock, 48 kHz, 480-sample blocks, default knobs,
   the first note at 0.5 s) into a float WAV, once per process. The standalone and the VST3 write the same file when
   their sound engines are the same. */
void renderTest()
{
    static std::atomic<bool> done { false };
    const auto spec = juce::SystemStats::getEnvironmentVariable ("OKL_RENDERTEST", {});
    if (spec.isEmpty() || done.exchange (true)) return;
    const juce::File events (spec.upToFirstOccurrenceOf ("|", false, false)), out (spec.fromFirstOccurrenceOf ("|", false, false));
    struct Ev { long long ms; juce::MidiMessage m; };              // whole milliseconds: sample positions as integers, as a host gives them
    std::vector<Ev> evs;
    for (const auto& line : juce::StringArray::fromLines (events.loadFileAsString()))
    {
        const auto tok = juce::StringArray::fromTokens (line, " ", "");
        if (tok.size() < 5 || tok[1] != "midi") continue;
        evs.push_back ({ std::llround (tok[0].getDoubleValue() * 1000.0),
                         juce::MidiMessage (tok[2].getHexValue32(), tok[3].getHexValue32(), tok[4].getHexValue32()) });
    }
    if (evs.empty()) return;
    const double fs = 48000.0;
    const long long ms0 = evs.front().ms - 500;
    const int bs = 480, total = (int) ((evs.back().ms - ms0 + 3000) * 48) / bs * bs;
    auto proc = std::make_unique<OkumuLabProcessor>();     // (on the heap: a processor is several megabytes)
    auto& p = *proc;
    p.setPlayConfigDetails (0, 2, fs, bs);
    p.prepareToPlay (fs, bs);
    juce::AudioBuffer<float> all (2, total), blk (2, bs);
    juce::MidiBuffer midi;
    size_t k = 0;
    for (int s0 = 0; s0 < total; s0 += bs)
    {
        midi.clear();
        for (; k < evs.size(); ++k)
        {
            const int at = (int) ((evs[k].ms - ms0) * 48);
            if (at >= s0 + bs) break;
            midi.addEvent (evs[k].m, juce::jlimit (0, bs - 1, at - s0));
        }
        blk.clear();
        p.processBlock (blk, midi);
        for (int c = 0; c < 2; ++c) all.copyFrom (c, s0, blk, c, 0, bs);
    }
    out.deleteFile();
    std::unique_ptr<juce::OutputStream> os (out.createOutputStream());
    if (os)
    {
        juce::WavAudioFormat wav;
        const auto opts = juce::AudioFormatWriterOptions{}.withSampleRate (fs).withNumChannels (2).withBitsPerSample (32)
                              .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
        if (auto w = wav.createWriterFor (os, opts)) w->writeFromAudioSampleBuffer (all, 0, total);
    }
}
} // namespace

juce::AudioProcessorValueTreeState::ParameterLayout OkumuLabProcessor::createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    for (int i = 0; i < kNumParams; ++i)
    {
        const ParamDef* d = &kParamDefs[i];
        if (d->fmt == Fmt::Choice)
        {
            // excitation, pipe metal, mallet head: a list in the host
            juce::StringArray names;
            for (int c = 0; c <= (int) (d->max + 0.5); ++c) names.add (juce::String::fromUTF8 (d->choices[c]));
            layout.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { d->id, 1 }, juce::String::fromUTF8 (d->name),
                                                                      names, (int) (d->def + 0.5)));
            continue;
        }
        auto attrs = juce::AudioParameterFloatAttributes()
                         .withStringFromValueFunction ([d] (float v, int) {
                             char buf[32];
                             formatValue (*d, v, buf);
                             return juce::String::fromUTF8 (buf);
                         })
                         .withValueFromStringFunction ([d] (const juce::String& s) {
                             const auto t = s.trim();
                             if (d->fmt == Fmt::Gas)
                             {
                                 if (t.startsWithIgnoreCase ("air")) return 0.0f;
                                 const float x = t.retainCharacters ("0123456789.").getFloatValue() / 100.0f;
                                 return t.startsWithIgnoreCase ("co") ? -x : x;
                             }
                             if (d->fmt == Fmt::Wind)
                             {
                                 // "1.20", "1.20 (600 Pa)" or "600 Pa"
                                 const auto head = t.upToFirstOccurrenceOf ("(", false, false).trim();
                                 const float x = head.retainCharacters ("0123456789.").getFloatValue();
                                 return head.endsWithIgnoreCase ("pa") ? x / (float) kWindRefPa : x;
                             }
                             if (d->fmt == Fmt::Inv)
                             {
                                 const float den = t.fromFirstOccurrenceOf ("/", false, false).retainCharacters ("0123456789.").getFloatValue();
                                 return den > 0 ? 1.0f / den : t.retainCharacters ("0123456789.").getFloatValue();
                             }
                             float v = t.retainCharacters ("-+0123456789.").getFloatValue();
                             if (d->fmt == Fmt::Pct || d->fmt == Fmt::Morph || d->fmt == Fmt::Lock) v /= 100.0f;
                             return v;
                         })
                         .withLabel (juce::String::fromUTF8 (d->unit));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { d->id, 1 }, juce::String::fromUTF8 (d->name),
                                                                 rangeFor (*d), (float) d->def, attrs));
    }
    return layout;
}

OkumuLabProcessor::OkumuLabProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "OkumuLab1", createLayout())
{
    for (int i = 0; i < kNumParams; ++i)
    {
        raw[(size_t) i] = apvts.getRawParameterValue (kParamDefs[i].id);
        params[(size_t) i] = apvts.getParameter (kParamDefs[i].id);
    }
    stopIndex = paramIndex ("stop");
    for (auto& m : midiPending) m.store (-1.0f);
    outBuf.assign ((size_t) kStream, 0.0f);
    sigBuf.assign ((size_t) kStream, 0.0f);
    router.setSink (this);
    muteOutput = juce::SystemStats::getEnvironmentVariable ("OKL_MUTE", {}).isNotEmpty();     // (screen tests without sound)
    startTimerHz (30);
    renderTest();
}

OkumuLabProcessor::~OkumuLabProcessor()
{
    stopTimer();
}

bool OkumuLabProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono();
}

void OkumuLabProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    fs = sampleRate;
    engine.prepare (sampleRate, samplesPerBlock);
    scratch.assign ((size_t) juce::jmax (samplesPerBlock, 1) + 1, 0.0f);
    monoTmp.assign ((size_t) juce::jmax (samplesPerBlock, 1) + 1, 0.0f);
    sigTmp.assign (4096, 0.0f);
    scopeRead = engine.scopeCount();
    haveLastG = false;
    cpuAvg = 0;
    lastStartTicks = 0;
}

// ---------------------------------------------------------------- screen streams
void OkumuLabProcessor::setUiStreams (bool on)
{
    if (on && ! uiStreams.load())
    {
        // forget what piled up while the screen was closed
        outFifo.reset();
        sigFifo.reset();
    }
    uiStreams.store (on);
}

namespace
{
int fifoRead (juce::AbstractFifo& f, const std::vector<float>& buf, float* dst, int maxN)
{
    const auto scope = f.read (juce::jmin (maxN, f.getNumReady()));
    if (scope.blockSize1 > 0) std::copy_n (buf.data() + scope.startIndex1, scope.blockSize1, dst);
    if (scope.blockSize2 > 0) std::copy_n (buf.data() + scope.startIndex2, scope.blockSize2, dst + scope.blockSize1);
    return scope.blockSize1 + scope.blockSize2;
}

void fifoWrite (juce::AbstractFifo& f, std::vector<float>& buf, const float* src, int n)
{
    const auto scope = f.write (juce::jmin (n, f.getFreeSpace()));          // a full queue drops the rest
    if (scope.blockSize1 > 0) std::copy_n (src, scope.blockSize1, buf.data() + scope.startIndex1);
    if (scope.blockSize2 > 0) std::copy_n (src + scope.blockSize1, scope.blockSize2, buf.data() + scope.startIndex2);
}
} // namespace

int OkumuLabProcessor::readOutput (float* dst, int maxN) { return fifoRead (outFifo, outBuf, dst, maxN); }
int OkumuLabProcessor::readFocusSignal (float* dst, int maxN) { return fifoRead (sigFifo, sigBuf, dst, maxN); }

bool OkumuLabProcessor::readCapture (PeriodCapture& c, int& voiceId, uint32_t& serial)
{
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        const uint32_t s0 = capSeq.load (std::memory_order_acquire);
        if (s0 & 1u) continue;
        if (uiCapSerial == serial) return false;
        const uint32_t ser = uiCapSerial;
        const int id = uiCapId;
        std::memcpy (&c, &uiCap, sizeof (PeriodCapture));
        std::atomic_thread_fence (std::memory_order_acquire);
        if (capSeq.load (std::memory_order_relaxed) == s0) { serial = ser; voiceId = id; return true; }
    }
    return false;
}

void OkumuLabProcessor::pushUiMidi (int status, int d1, int d2)
{
    const auto scope = uiMidiFifo.write (1);
    const uint32_t packed = (uint32_t) (status & 0xff) | ((uint32_t) (d1 & 0x7f) << 8) | ((uint32_t) (d2 & 0x7f) << 16);
    if (scope.blockSize1 > 0) uiMidiBuf[(size_t) scope.startIndex1] = packed;
    else if (scope.blockSize2 > 0) uiMidiBuf[(size_t) scope.startIndex2] = packed;
}

OkumuLabProcessor::AudioStats OkumuLabProcessor::audioStats() const
{
    AudioStats a;
    a.blocks = stBlocks.load(); a.overruns = stOverruns.load(); a.late = stLate.load();
    a.maxLoad = stMaxLoad.load(); a.maxGap = stMaxGap.load();
    return a;
}

/* knob values (host parameters, or a controller move not yet confirmed by the host) */
void OkumuLabProcessor::readGlobals (Globals& g) const
{
    for (int i = 0; i < kNumParams; ++i)
    {
        const ParamDef& d = kParamDefs[i];
        const float pend = midiPending[(size_t) i].load (std::memory_order_relaxed);
        g.*(d.field) = pend >= 0.0f ? fromNorm (d, pend) : (double) raw[(size_t) i]->load (std::memory_order_relaxed);
    }
}

void OkumuLabProcessor::updateGlobals()
{
    Globals g;
    readGlobals (g);
    router.applyPads (g);
    if (! haveLastG || std::memcmp (&g, &lastG, sizeof (Globals)) != 0)
    {
        engine.setGlobals (g);
        lastG = g;
        haveLastG = true;
    }
}

void OkumuLabProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    const ScopedEngineFpMode fpMode;
    const auto t0 = juce::Time::getHighResolutionTicks();
    const int n = buffer.getNumSamples();
    // callback timing: the gap since the previous callback, against that block's real-time budget
    if (statsReset.exchange (false))
    {
        stBlocks.store (0); stOverruns.store (0); stLate.store (0); stMaxLoad.store (0); stMaxGap.store (0);
        lastStartTicks = 0;
    }
    if (lastStartTicks != 0 && lastBudget > 0)
    {
        const double gap = juce::Time::highResolutionTicksToSeconds (t0 - lastStartTicks) / lastBudget;
        if (gap > stMaxGap.load (std::memory_order_relaxed)) stMaxGap.store ((float) gap, std::memory_order_relaxed);
        if (gap > 2.5) stLate.fetch_add (1, std::memory_order_relaxed);
    }
    lastStartTicks = t0;
    lastBudget = n / fs;
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch) buffer.clear (ch, 0, n);
    if (buffer.getNumChannels() == 0 || n == 0) { midiMessages.clear(); return; }

    if (panicRequest.exchange (false))
    {
        engine.allOff (true);
        router.resetPads();
    }
    // v2.0: a scale loaded on the message thread (never waited for: if it is being written, the next block takes it)
    {
        const juce::SpinLock::ScopedTryLockType tl (tuneLock);
        if (tl.isLocked() && tuneDirty)
        {
            engine.setCustomTuning (tuneShared);
            tuneDirty = false;
        }
    }

    float* L = buffer.getWritePointer (0);
    float* R = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : nullptr;
    if (! R)
    {
        if ((int) scratch.size() < n) scratch.resize ((size_t) n);     // (only if the host exceeds the announced block size)
        R = scratch.data();
    }

    keyboardState.processNextMidiBuffer (midiMessages, 0, n, true);
    // notes, pads and the pedal played on the screen: through the same router, at the block start
    {
        const auto scope = uiMidiFifo.read (uiMidiFifo.getNumReady());
        auto play = [this] (uint32_t m) {
            const uint8_t b[3] { (uint8_t) (m & 0xff), (uint8_t) ((m >> 8) & 0x7f), (uint8_t) ((m >> 16) & 0x7f) };
            router.handle (b, 3);
        };
        for (int i = 0; i < scope.blockSize1; ++i) play (uiMidiBuf[(size_t) (scope.startIndex1 + i)]);
        for (int i = 0; i < scope.blockSize2; ++i) play (uiMidiBuf[(size_t) (scope.startIndex2 + i)]);
    }

    // MIDI events at their sample positions: render up to each event, then apply it
    int pos = 0;
    for (const auto meta : midiMessages)
    {
        const int s = juce::jlimit (0, n, meta.samplePosition);
        if (s > pos)
        {
            updateGlobals();
            engine.process (L + pos, R + pos, s - pos);
            pos = s;
        }
        router.handle (meta.data, meta.numBytes);
    }
    if (pos < n)
    {
        updateGlobals();
        engine.process (L + pos, R + pos, n - pos);
    }
    if (buffer.getNumChannels() == 1)
        for (int i = 0; i < n; ++i) L[i] = 0.5f * (L[i] + R[i]);

    // screen streams: the output (spectrogram) and the focus pipe's mouth signal (pitch, harmonics)
    if (uiStreams.load (std::memory_order_relaxed))
    {
        if ((int) monoTmp.size() >= n)
        {
            if (buffer.getNumChannels() == 1) std::copy_n (L, n, monoTmp.data());
            else for (int i = 0; i < n; ++i) monoTmp[(size_t) i] = 0.5f * (L[i] + R[i]);
            fifoWrite (outFifo, outBuf, monoTmp.data(), n);
        }
        const uint64_t sc = engine.scopeCount();
        const int fresh = (int) juce::jmin<uint64_t> (sc - scopeRead, (uint64_t) sigTmp.size());
        if (fresh > 0)
        {
            engine.copyScope (sigTmp.data(), fresh);
            fifoWrite (sigFifo, sigBuf, sigTmp.data(), fresh);
        }
        scopeRead = sc;
    }
    else scopeRead = engine.scopeCount();

    if (muteOutput) buffer.clear();
    midiMessages.clear();

    // telemetry for the editor, ~60 times a second
    telTimer += n / fs;
    if (telTimer >= 1.0 / 60.0)
    {
        telTimer = 0;
        telSeq.fetch_add (1, std::memory_order_acq_rel);
        engine.fillTelemetry (tel);
        engine.copyScope (telScope, kScope);
        telSeq.fetch_add (1, std::memory_order_release);
        if (engine.lastCaptureSerial() != uiCapSerial)
        {
            capSeq.fetch_add (1, std::memory_order_acq_rel);
            uiCap = engine.lastCapture();
            uiCapId = engine.lastCaptureVoiceId();
            uiCapSerial = engine.lastCaptureSerial();
            capSeq.fetch_add (1, std::memory_order_release);
        }
    }

    const double used = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0);
    const double load = used / (n / fs);
    stBlocks.fetch_add (1, std::memory_order_relaxed);
    if (load > 1.0) stOverruns.fetch_add (1, std::memory_order_relaxed);
    if (load > stMaxLoad.load (std::memory_order_relaxed)) stMaxLoad.store ((float) load, std::memory_order_relaxed);
    cpuAvg += (load - cpuAvg) * juce::jmin (1.0, n / (0.3 * fs));
    cpu.store ((float) cpuAvg, std::memory_order_relaxed);
}

/* a controller moved a parameter: the engine uses it at once, the host hears about it on the timer */
void OkumuLabProcessor::midiParam (int index, double norm)
{
    if (index >= 0 && index < kNumParams) midiPending[(size_t) index].store ((float) juce::jlimit (0.0, 1.0, norm), std::memory_order_relaxed);
}

void OkumuLabProcessor::timerCallback()
{
    for (int i = 0; i < kNumParams; ++i)
    {
        float v = midiPending[(size_t) i].load();
        if (v < 0.0f) continue;
        auto* p = params[(size_t) i];
        p->beginChangeGesture();
        p->setValueNotifyingHost (v);
        p->endChangeGesture();
        midiPending[(size_t) i].compare_exchange_strong (v, -1.0f);   // a newer move stays pending
    }
}

int OkumuLabProcessor::currentStop() const
{
    if (stopIndex < 0) return 0;
    return juce::jlimit (0, kNumStops - 1, (int) std::lround (raw[(size_t) stopIndex]->load()));
}

double OkumuLabProcessor::paramDefault (int index) const
{
    return stopDefault (currentStop(), index);
}

/* every parameter back to the default of the stop in use (the stop stays: its own default is itself) */
void OkumuLabProcessor::resetAllParameters()
{
    double v[kMaxParams];
    stopValues (currentStop(), v);
    for (int i = 0; i < kNumParams; ++i)
    {
        midiPending[(size_t) i].store (-1.0f);
        auto* p = params[(size_t) i];
        p->beginChangeGesture();
        p->setValueNotifyingHost (p->convertTo0to1 ((float) v[i]));
        p->endChangeGesture();
    }
}

void OkumuLabProcessor::resetParameter (int index)
{
    if (index < 0 || index >= kNumParams) return;
    midiPending[(size_t) index].store (-1.0f);
    auto* p = params[(size_t) index];
    p->beginChangeGesture();
    p->setValueNotifyingHost (p->convertTo0to1 ((float) paramDefault (index)));
    p->endChangeGesture();
}

// ---------------------------------------------------------------- v2.0: tuning from Scala files
namespace
{
/* a Scala file's bytes as text: UTF-8 as it is, anything else read as Latin-1 (descriptions of older .scl files) */
juce::String scalaText (const juce::MemoryBlock& mb)
{
    const auto* p = static_cast<const char*> (mb.getData());
    const int n = (int) mb.getSize();
    if (juce::CharPointer_UTF8::isValidString (p, n)) return juce::String::fromUTF8 (p, n);
    juce::String s;
    s.preallocateBytes ((size_t) n * 2);
    for (int i = 0; i < n; ++i) s += juce::String::charToString ((juce::juce_wchar) (uint8_t) p[i]);
    return s;
}
} // namespace

void OkumuLabProcessor::pushTuning()
{
    const juce::SpinLock::ScopedLockType l (tuneLock);
    tuneShared = tuneMsg;
    tuneDirty = true;
}

bool OkumuLabProcessor::applyTuning (const juce::String& scl, const juce::String& kbm, bool report)
{
    if (scl.isEmpty() && kbm.isEmpty())
    {
        tuneMsg = okl::CustomTuning();
        tuneFiles.description.clear();
        tuneFiles.notes = 0;
        pushTuning();
        return true;
    }
    okl::CustomTuning c;
    okl::TuningInfo info;
    if (! okl::buildCustomTuning (scl.toStdString(), kbm.toStdString(), c, info))
    {
        if (report) tuneFiles.error = juce::String::fromUTF8 (info.error.c_str());
        return false;
    }
    tuneMsg = c;
    tuneFiles.description = juce::String::fromUTF8 (info.description.c_str()).trim();
    tuneFiles.notes = info.notes;
    tuneFiles.error.clear();
    pushTuning();
    return true;
}

bool OkumuLabProcessor::loadTuningText (bool kbm, const juce::String& text, const juce::String& name)
{
    const bool ok = applyTuning (kbm ? tuneFiles.scl : text, kbm ? text : tuneFiles.kbm, true);
    if (ok)
    {
        if (kbm) { tuneFiles.kbm = text; tuneFiles.kbmName = name; }
        else { tuneFiles.scl = text; tuneFiles.sclName = name; }
        // what was loaded is what sounds: the Temperament knob to Custom
        const int ti = paramIndex ("temperament");
        if (ti >= 0)
        {
            auto* p = params[(size_t) ti];
            midiPending[(size_t) ti].store (-1.0f);
            p->beginChangeGesture();
            p->setValueNotifyingHost (p->convertTo0to1 ((float) okl::TEMP_CUSTOM));
            p->endChangeGesture();
        }
    }
    ++tuneSerial;
    return ok;
}

bool OkumuLabProcessor::loadTuningFile (bool kbm, const juce::File& f)
{
    if (! f.existsAsFile() || f.getSize() > (1 << 20))
    {
        tuneFiles.error = f.getFileName() + ": not a readable file of at most 1 MB";
        ++tuneSerial;
        return false;
    }
    juce::MemoryBlock mb;
    if (! f.loadFileAsData (mb))
    {
        tuneFiles.error = f.getFileName() + ": could not be read";
        ++tuneSerial;
        return false;
    }
    return loadTuningText (kbm, scalaText (mb), f.getFileName());
}

void OkumuLabProcessor::clearTuning (bool kbmOnly)
{
    tuneFiles.kbm.clear(); tuneFiles.kbmName.clear();
    if (! kbmOnly) { tuneFiles.scl.clear(); tuneFiles.sclName.clear(); }
    tuneFiles.error.clear();
    if (! applyTuning (tuneFiles.scl, {}, true)) applyTuning ({}, {}, false);
    // nothing loaded: the Temperament knob leaves Custom for equal temperament
    const int ti = paramIndex ("temperament");
    if (! kbmOnly && ti >= 0 && (int) std::lround (raw[(size_t) ti]->load()) == okl::TEMP_CUSTOM)
    {
        auto* p = params[(size_t) ti];
        p->beginChangeGesture();
        p->setValueNotifyingHost (p->convertTo0to1 ((float) okl::TEMP_EQUAL));
        p->endChangeGesture();
    }
    ++tuneSerial;
}

// ---------------------------------------------------------------- V2: the programs are the stops
int OkumuLabProcessor::getNumPrograms() { return (int) factoryPresets().size(); }

const juce::String OkumuLabProcessor::getProgramName (int index)
{
    const auto& ps = factoryPresets();
    return index >= 0 && index < (int) ps.size() ? juce::String::fromUTF8 (ps[(size_t) index].name) : juce::String();
}

/* the stop, and every parameter to that stop as built, told to the host like a knob move */
void OkumuLabProcessor::setCurrentProgram (int index)
{
    if (index < 0 || index >= getNumPrograms()) return;
    double v[kMaxParams];
    presetValues (index, v);
    for (int i = 0; i < kNumParams; ++i)
    {
        if (keptAcrossStops (i)) continue;         // v2.0: the organ stays in its tuning
        midiPending[(size_t) i].store (-1.0f);
        auto* p = params[(size_t) i];
        p->beginChangeGesture();
        p->setValueNotifyingHost (p->convertTo0to1 ((float) v[i]));
        p->endChangeGesture();
    }
}

bool OkumuLabProcessor::readTelemetry (Telemetry& t, float* scope, int n)
{
    n = juce::jmin (n, kScope);
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        const uint32_t s0 = telSeq.load (std::memory_order_acquire);
        if (s0 & 1u) continue;
        std::memcpy (&t, &tel, sizeof (Telemetry));
        if (scope) std::memcpy (scope, telScope + (kScope - n), sizeof (float) * (size_t) n);
        std::atomic_thread_fence (std::memory_order_acquire);
        if (telSeq.load (std::memory_order_relaxed) == s0) return true;
    }
    return false;
}

juce::String OkumuLabProcessor::controllerText (int paramIndex) const
{
    // the knob's own controller first (e.g. wind pressure: knob 5 = CC93, then fader 1 = CC82)
    juce::StringArray ccs;
    const int own = kParamDefs[paramIndex].cc;
    if (own >= 0 && own < 128 && router.ccMap[(size_t) own].load() == paramIndex) ccs.add ("CC" + juce::String (own));
    for (int c = 0; c < 128; ++c)
        if (c != own && router.ccMap[(size_t) c].load() == paramIndex) ccs.add (ccs.isEmpty() ? "CC" + juce::String (c) : juce::String (c));
    if (own == kPitchBend) ccs.insert (0, "PB");
    return ccs.joinIntoString ("/");
}

// ---------------------------------------------------------------- state
void OkumuLabProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    juce::StringArray map;
    for (int c = 0; c < 128; ++c)
    {
        const int idx = router.ccMap[(size_t) c].load();
        if (idx >= 0 && idx < kNumParams) map.add (juce::String (c) + "=" + kParamDefs[idx].id);
    }
    state.setProperty ("ccMap", map.joinIntoString (","), nullptr);
    state.setProperty ("padChannel", router.padChannel.load() + 1, nullptr);
    state.setProperty ("version", JucePlugin_VersionString, nullptr);
    state.setProperty ("program", currentStop(), nullptr);       // (V2: the stop; the parameter itself is in the tree)
    // v2.0: the loaded Scala files themselves (the project plays without them)
    state.setProperty ("tuningScl", tuneFiles.scl, nullptr);
    state.setProperty ("tuningSclName", tuneFiles.sclName, nullptr);
    state.setProperty ("tuningKbm", tuneFiles.kbm, nullptr);
    state.setProperty ("tuningKbmName", tuneFiles.kbmName, nullptr);
    if (auto xml = state.createXml()) copyXmlToBinary (*xml, destData);
}

void OkumuLabProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (! xml || ! xml->hasTagName (apvts.state.getType())) return;
    auto state = juce::ValueTree::fromXml (*xml);
    apvts.replaceState (state);
    if (state.hasProperty ("ccMap"))
    {
        for (auto& m : router.ccMap) m.store (-1);
        for (const auto& item : juce::StringArray::fromTokens (state["ccMap"].toString(), ",", ""))
        {
            const int c = item.upToFirstOccurrenceOf ("=", false, false).getIntValue();
            const int idx = paramIndex (item.fromFirstOccurrenceOf ("=", false, false).toRawUTF8());
            if (c >= 0 && c < 128 && idx >= 0) router.ccMap[(size_t) c].store (idx);
        }
    }
    // ("program": the stop since V2, its parameter is restored with the tree; a 1.x project's recipe number means nothing now)
    if (state.hasProperty ("padChannel"))
        router.padChannel.store (juce::jlimit (0, 16, (int) state["padChannel"]) - 1);
    // v2.0: the project's Scala files (none in older projects)
    tuneFiles = TuningFiles();
    tuneFiles.scl = state["tuningScl"].toString();
    tuneFiles.sclName = state["tuningSclName"].toString();
    tuneFiles.kbm = state["tuningKbm"].toString();
    tuneFiles.kbmName = state["tuningKbmName"].toString();
    if (! applyTuning (tuneFiles.scl, tuneFiles.kbm, true)) applyTuning ({}, {}, false);    // (the texts stay, the error says why)
    ++tuneSerial;
    for (auto& m : midiPending) m.store (-1.0f);
}

/* the web + Three.js screen (Phase 4); the native cockpit screen if the system's web view is missing
   (WebView2 on Windows, WebKitGTK on Linux) */
juce::AudioProcessorEditor* OkumuLabProcessor::createEditor()
{
    const bool forceNative = juce::SystemStats::getEnvironmentVariable ("OKL_NATIVE_UI", {}).isNotEmpty();
    if (! forceNative && WebEditor::available()) return new WebEditor (*this);
    return new NativeEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new OkumuLabProcessor();
}
