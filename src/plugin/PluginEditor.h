/*
 * OkumuLab 1 — native editor (JUCE, cockpit style): the fallback screen
 *
 * Phase 3 screen: every parameter as a knob in the panels of the Phase 2
 * prototype, the focus pipe's physical state, the pitch-lock meter, the
 * standing wave of the last period capture and the mouth signal. Since Phase 4
 * the WebView + Three.js screen (WebEditor) is used; this one is the fallback
 * when WebView2 is not available (or OKL_NATIVE_UI is set).
 */
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include "CockpitLookAndFeel.h"
#include "PluginProcessor.h"

/* a rotary slider that hands right-clicks to its knob (MIDI learn menu) instead of dragging */
class KnobSlider : public juce::Slider
{
public:
    std::function<void()> onRightClick;
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu()) { if (onRightClick) onRightClick(); return; }
        juce::Slider::mouseDown (e);
    }
    void mouseDrag (const juce::MouseEvent& e) override { if (! e.mods.isPopupMenu()) juce::Slider::mouseDrag (e); }
    void mouseUp (const juce::MouseEvent& e) override { if (! e.mods.isPopupMenu()) juce::Slider::mouseUp (e); }
};

class OklKnob : public juce::Component, private juce::Slider::Listener
{
public:
    OklKnob (OkumuLabProcessor& p, int index, bool large);
    ~OklKnob() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void tick (const okl::Telemetry& t, bool learning);
    void setBand (double v0, double v1);           // real-instrument range (parameter units)
    int index() const { return idx; }

private:
    void sliderValueChanged (juce::Slider*) override {}
    void sliderDragStarted (juce::Slider*) override { dragging = true; }
    void sliderDragEnded (juce::Slider*) override { dragging = false; }
    void showMenu();

    OkumuLabProcessor& proc;
    const int idx;
    const bool big;
    KnobSlider slider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attach;
    juce::String ccText;
    float lastValue = 0, hot = 0;
    bool dragging = false;
};

class NativeEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit NativeEditor (OkumuLabProcessor&);
    ~NativeEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr int kW = 1180, kH = 700;

private:
    struct Content : juce::Component
    {
        NativeEditor& ed;
        explicit Content (NativeEditor& e) : ed (e) {}
        void paint (juce::Graphics& g) override { ed.paintContent (g); }
    };
    void paintContent (juce::Graphics&);
    void paintHeader (juce::Graphics&, juce::Rectangle<float>);
    void paintStatus (juce::Graphics&, juce::Rectangle<float>);
    void paintBore (juce::Graphics&, juce::Rectangle<float>);
    void timerCallback() override;
    void layoutPanel (okl::Group grp, juce::Rectangle<int> area, bool large, std::vector<OklKnob*>* made = nullptr, const char* skip = nullptr);
    void showMalletPanel (bool on);

    OkumuLabProcessor& proc;
    CockpitLookAndFeel lnf;
    Content content { *this };
    std::vector<std::unique_ptr<OklKnob>> knobs;
    juce::TextButton resetButton { "RESET" }, panicButton { "PANIC" };
    juce::ComboBox padChannel;
    // MALLET: the excitation switch; the LAB panel becomes the MALLET panel
    juce::ComboBox excite;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> exciteAttach;
    std::vector<OklKnob*> labKnobs, malletKnobs;
    int labPanel = -1;
    int malletShown = -1;
    juce::MidiKeyboardComponent keyboard;
    juce::String snapshotPath;
    int frames = 0;
    juce::TooltipWindow tooltips { this, 600 };

    struct PanelBox { juce::Rectangle<float> r; juce::String num, title, sub; };
    std::vector<PanelBox> panels;
    juce::Rectangle<float> headerR, statusR, boreR, footerR;

    okl::Telemetry tel;
    float scope[OkumuLabProcessor::kScope] {};
    bool haveTel = false;
    uint32_t lastMidiCount = 0;
    float midiLed = 0;
    int learnSerial = 0;
    float boreShown[okl::NX_SNAP] {};
    int lastCap = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NativeEditor)
};
