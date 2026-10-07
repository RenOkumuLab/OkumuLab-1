/*
 * OkumuLab 1 — the Phase 4 screen: the Phase 2 WebView + Three.js screen
 * (web/, embedded in the plugin as a zip) driven by the C++ engine.
 *
 * C++ -> page, about 60 times a second (event "okl", t = "tel"): the engine
 * telemetry in the Phase 2 message format, the new output samples (the
 * spectrogram) and focus-pipe mouth samples (pitch, harmonics), the latest
 * period snapshot of the bore, parameter values changed by the host or a
 * controller, controller tags, MIDI activity, pad pressures.
 * Page -> C++ (event "okl"): ready, param, midi, reset, panic, learn, fps, log.
 *
 * The audio thread never waits for the screen: everything passes through the
 * processor's lock-free queues and seqlocks; this editor reads them on its timer.
 *
 * OKL_PERFTEST=<report.json> (Standalone): plays a scripted test, measures the
 * screen's frame rate and the audio callbacks, writes the report and quits.
 */
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include "PluginProcessor.h"

#include <map>

class WebEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit WebEditor (OkumuLabProcessor&);
    ~WebEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /* WebView2 runtime present (otherwise the native editor is used) */
    static bool available();

    static constexpr int kW = 1280, kH = 800;      // the page scales its 1600 x 1000 stage to fit

private:
    juce::WebBrowserComponent::Options makeOptions();
    std::optional<juce::WebBrowserComponent::Resource> resource (const juce::String& url);
    void onMessage (const juce::var& m);
    void timerCallback() override;
    void sendInit();
    void sendFrame();
    void perfTest (double ms);
    void writePerfReport();

    OkumuLabProcessor& proc;
    std::unique_ptr<juce::ZipFile> zip;
    std::map<juce::String, juce::WebBrowserComponent::Resource> cache;

    bool ready = false;
    std::array<float, OkumuLabProcessor::kMaxParams> lastSent {};
    std::array<juce::String, OkumuLabProcessor::kMaxParams> lastCc;
    std::array<double, OkumuLabProcessor::kMaxParams> gestureUntil {};
    uint32_t midiRead = 0, capSerial = 0;
    int lastPadChannel = -100, lastProgram = -1;
    std::vector<float> outTmp, sigTmp;
    okl::PeriodCapture cap;

    // performance test
    juce::String perfPath;
    double readyAt = 0;
    int perfStage = 0;
    struct FpsSample { double t, fps, maxMs, p99Ms; int longFrames; double workMs, telMs, workMaxMs; };
    double sendMsSum = 0, sendMsMax = 0;
    int sendN = 0;
    std::vector<FpsSample> fpsSamples;
    juce::StringArray jsLog;
    juce::var screenInfo;

    juce::WebBrowserComponent browser;      // last: destroyed first (its callbacks use the members above)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WebEditor)
};
