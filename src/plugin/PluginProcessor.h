/*
 * OkumuLab 1 — plugin processor (JUCE): parameters, MIDI, state, telemetry
 */
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "Engine.h"
#include "MidiRouter.h"
#include "Params.h"

#include <array>
#include <atomic>

class OkumuLabProcessor : public juce::AudioProcessor,
                          private juce::Timer,
                          private okl::MidiRouter::Sink
{
public:
    OkumuLabProcessor();
    ~OkumuLabProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override
    {
        // P5: the computed nave rings longest in its lowest band (T60 about 5.7 s at 63 Hz)
        double t = 0;
        for (double v : engine.room().t60) t = juce::jmax (t, v);
        return t > 0 ? std::ceil (t) : 6.0;
    }

    /* P5: the factory presets (experiment recipes, Presets.h) as the host's programs */
    int getNumPrograms() override;
    int getCurrentProgram() override { return currentProgram.load(); }
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // ---- editor interface (message thread)
    juce::AudioProcessorValueTreeState apvts;
    juce::MidiKeyboardState keyboardState;     // the on-screen keyboard (merged into the MIDI input)
    okl::MidiRouter& midi() { return router; }
    juce::RangedAudioParameter* param (int index) const { return params[(size_t) index]; }
    const okl::RoomInfo& engineRoom() const { return engine.room(); }
    void resetAllParameters();                 // RESET: every parameter back to its default
    void panic() { panicRequest.store (true); }
    /* latest telemetry + focus mouth signal (n <= 512); false if nothing new could be read */
    bool readTelemetry (okl::Telemetry& t, float* scope, int n);
    float cpuLoad() const { return cpu.load(); }
    juce::String controllerText (int paramIndex) const;   // e.g. "CC74", "PB", "CC93/82"
    void setCaptureEnabled (bool on) { engine.captureEnabled.store (on); }

    static constexpr int kScope = 512;
    static constexpr int kMaxParams = 64;

    // ---- Phase 4 screen (WebView): lock-free streams the audio thread fills while the screen is open
    void setUiStreams (bool on);
    int readOutput (float* dst, int maxN);                  // mono output since the last read
    int readFocusSignal (float* dst, int maxN);             // focus pipe's mouth signal since the last read
    /* the latest period snapshot if it is newer than `serial` (which is then updated) */
    bool readCapture (okl::PeriodCapture& c, int& voiceId, uint32_t& serial);
    void pushUiMidi (int status, int d1, int d2);           // notes / pads / pedal from the screen
    struct AudioStats { uint32_t blocks = 0, overruns = 0, late = 0; float maxLoad = 0, maxGap = 0; };
    AudioStats audioStats() const;                          // audio callback timing (dropout check)
    void resetAudioStats() { statsReset.store (true); }
    float padPressure (int bank, int i) const { return bank == 0 ? router.padA[(size_t) i].load() : router.padB[(size_t) i].load(); }

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void midiParam (int index, double norm) override;       // audio thread, from the router
    void timerCallback() override;                           // flushes controller moves to the host
    void readGlobals (okl::Globals& g) const;
    void updateGlobals();

    okl::Engine engine;
    okl::MidiRouter router { engine };
    std::array<std::atomic<float>*, kMaxParams> raw {};
    std::array<juce::RangedAudioParameter*, kMaxParams> params {};
    std::array<std::atomic<float>, kMaxParams> midiPending;   // normalised; < 0 = none
    std::atomic<int> currentProgram { 0 };
    okl::Globals lastG;
    bool haveLastG = false;
    std::atomic<bool> panicRequest { false };

    // telemetry, written by the audio thread (seqlock), read by the editor
    std::atomic<uint32_t> telSeq { 0 };
    okl::Telemetry tel;
    float telScope[kScope] {};
    double telTimer = 0;
    std::atomic<float> cpu { 0.0f };
    double cpuAvg = 0, fs = 48000;
    std::vector<float> scratch;
    bool muteOutput = false;

    // screen streams
    std::atomic<bool> uiStreams { false };
    static constexpr int kStream = 1 << 15;
    juce::AbstractFifo outFifo { kStream }, sigFifo { kStream }, uiMidiFifo { 1024 };
    std::vector<float> outBuf, sigBuf, monoTmp, sigTmp;
    std::array<uint32_t, 1024> uiMidiBuf {};
    uint64_t scopeRead = 0;
    std::atomic<uint32_t> capSeq { 0 };
    okl::PeriodCapture uiCap;
    int uiCapId = -1;
    uint32_t uiCapSerial = 0;
    // audio timing
    std::atomic<uint32_t> stBlocks { 0 }, stOverruns { 0 }, stLate { 0 };
    std::atomic<float> stMaxLoad { 0 }, stMaxGap { 0 };
    std::atomic<bool> statsReset { false };
    juce::int64 lastStartTicks = 0;
    double lastBudget = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OkumuLabProcessor)
};
