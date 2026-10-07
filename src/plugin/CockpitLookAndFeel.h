/*
 * OkumuLab 1 — cockpit look (the colour tokens of the Phase 2 screen, css/okumulab.css)
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace okl::ui
{
const juce::Colour bg0 { 0xff020509 }, bg1 { 0xff060c13 }, panelA { 0xf00b151f }, panelB { 0xf0060c13 };
const juce::Colour rule { 0xff15303c }, rule2 { 0xff1f4757 };
const juce::Colour cyan { 0xff46e3ff }, cyan2 { 0xff26a9c6 }, cyanD { 0xff12495a };
const juce::Colour amber { 0xffffb547 }, amberD { 0xff6b4612 }, red { 0xffff4d5e }, green { 0xff6dffa0 }, mag { 0xffff6ad5 };
const juce::Colour fg { 0xffd6f4ff }, fg2 { 0xff93b8c7 }, dim { 0xff58737f }, dim2 { 0xff3a515b };

juce::Font mono (float h, bool bold = false);
juce::Font sans (float h, bool bold = false);
juce::Font disp (float h, bool bold = true);

/* panel frame with corner ticks and a numbered title bar */
void drawPanel (juce::Graphics& g, juce::Rectangle<float> r, const juce::String& num, const juce::String& title, const juce::String& sub);
}

class CockpitLookAndFeel : public juce::LookAndFeel_V4
{
public:
    CockpitLookAndFeel();

    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos, float start, float end, juce::Slider&) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool over, bool down) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool over, bool down) override;
    void drawComboBox (juce::Graphics&, int w, int h, bool down, int bx, int by, int bw, int bh, juce::ComboBox&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    juce::Font getPopupMenuFont() override;
    void drawPopupMenuBackground (juce::Graphics&, int w, int h) override;
};
