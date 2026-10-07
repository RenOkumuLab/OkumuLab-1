/*
 * OkumuLab 1 — cockpit look
 */
#include "CockpitLookAndFeel.h"

namespace okl::ui
{
juce::Font mono (float h, bool bold)
{
    return juce::Font (juce::FontOptions ("Consolas", h, bold ? juce::Font::bold : juce::Font::plain));
}

juce::Font sans (float h, bool bold)
{
    return juce::Font (juce::FontOptions ("Segoe UI", h, bold ? juce::Font::bold : juce::Font::plain));
}

juce::Font disp (float h, bool bold)
{
    return juce::Font (juce::FontOptions ("Bahnschrift", h, bold ? juce::Font::bold : juce::Font::plain));
}

void drawPanel (juce::Graphics& g, juce::Rectangle<float> r, const juce::String& num, const juce::String& title, const juce::String& sub)
{
    juce::ColourGradient grad (panelA, r.getX(), r.getY(), panelB, r.getX(), r.getBottom(), false);
    g.setGradientFill (grad);
    g.fillRoundedRectangle (r, 3.0f);
    g.setColour (rule);
    g.drawRoundedRectangle (r.reduced (0.5f), 3.0f, 1.0f);
    // corner ticks
    g.setColour (cyan.withAlpha (0.55f));
    const float t = 7.0f;
    auto corner = [&] (float x, float y, float dx, float dy) {
        g.drawLine (x, y, x + dx * t, y, 1.2f);
        g.drawLine (x, y, x, y + dy * t, 1.2f);
    };
    corner (r.getX() + 1, r.getY() + 1, 1, 1);
    corner (r.getRight() - 1, r.getY() + 1, -1, 1);
    corner (r.getX() + 1, r.getBottom() - 1, 1, -1);
    corner (r.getRight() - 1, r.getBottom() - 1, -1, -1);
    // title bar
    auto bar = r.removeFromTop (20.0f).reduced (8.0f, 0.0f);
    g.setColour (cyan);
    g.setFont (mono (11.0f, true));
    g.drawText (num, bar.removeFromLeft (20.0f), juce::Justification::centredLeft);
    g.setColour (fg);
    g.setFont (disp (12.5f));
    const float tw = juce::GlyphArrangement::getStringWidth (disp (12.5f), title) + 12.0f;
    g.drawText (title, bar.removeFromLeft (juce::jmin (tw, bar.getWidth())), juce::Justification::centredLeft);
    g.setColour (dim);
    g.setFont (mono (9.5f));
    g.drawText (sub, bar, juce::Justification::centredRight);
    g.setColour (rule);
    g.drawHorizontalLine ((int) (r.getY()), r.getX() + 6, r.getRight() - 6);
}
} // namespace okl::ui

using namespace okl::ui;

CockpitLookAndFeel::CockpitLookAndFeel()
{
    setColour (juce::PopupMenu::backgroundColourId, bg1);
    setColour (juce::PopupMenu::textColourId, fg);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, cyanD);
    setColour (juce::PopupMenu::highlightedTextColourId, fg);
    setColour (juce::ComboBox::backgroundColourId, bg1);
    setColour (juce::ComboBox::textColourId, fg2);
    setColour (juce::ComboBox::outlineColourId, rule2);
    setColour (juce::ComboBox::arrowColourId, cyan2);
    setColour (juce::TooltipWindow::backgroundColourId, bg1);
    setColour (juce::TooltipWindow::textColourId, fg);
    setColour (juce::TooltipWindow::outlineColourId, rule2);
}

/* the knob: dim track, real-instrument band (green), value arc (cyan, from the default for bipolar knobs) */
void CockpitLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float start, float end, juce::Slider& s)
{
    const auto bounds = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (4.0f);
    const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto c = bounds.getCentre();
    const float angle = start + pos * (end - start);
    const float trackR = radius - 3.0f;
    auto arc = [&] (float a0, float a1, float r, float thick, juce::Colour col) {
        juce::Path p;
        p.addCentredArc (c.x, c.y, r, r, 0.0f, a0, a1, true);
        g.setColour (col);
        g.strokePath (p, juce::PathStrokeType (thick, juce::PathStrokeType::curved, juce::PathStrokeType::butt));
    };
    // body
    g.setColour (bg0);
    g.fillEllipse (c.x - trackR + 4, c.y - trackR + 4, 2 * (trackR - 4), 2 * (trackR - 4));
    g.setColour (rule2);
    g.drawEllipse (c.x - trackR + 4, c.y - trackR + 4, 2 * (trackR - 4), 2 * (trackR - 4), 1.0f);
    arc (start, end, trackR, 3.0f, dim2);
    // band of the real instrument (properties set by the editor, normalised 0..1)
    const auto& props = s.getProperties();
    if (props.contains ("band0"))
    {
        const float b0 = (float) props["band0"], b1 = (float) props["band1"];
        const float a0 = start + b0 * (end - start), a1 = start + b1 * (end - start);
        if (a1 - a0 < 0.03f) arc (a0 - 0.03f, a0 + 0.03f, trackR + 3.5f, 2.0f, green.withAlpha (0.85f));
        else arc (a0, a1, trackR + 3.5f, 2.0f, green.withAlpha (0.7f));
    }
    // value arc
    const float defPos = props.contains ("defPos") ? (float) props["defPos"] : 0.0f;
    const bool bipolar = props.contains ("bipolar") && (bool) props["bipolar"];
    const float from = bipolar ? start + defPos * (end - start) : start;
    const bool learning = props.contains ("learn") && (bool) props["learn"];
    const juce::Colour valCol = learning ? amber : (s.isMouseOverOrDragging() ? cyan.brighter (0.3f) : cyan);
    if (std::abs (angle - from) > 0.001f) arc (juce::jmin (from, angle), juce::jmax (from, angle), trackR, 3.0f, valCol);
    // default tick
    {
        const float a = start + defPos * (end - start);
        const juce::Point<float> p0 = c.getPointOnCircumference (trackR - 6.0f, a), p1 = c.getPointOnCircumference (trackR + 1.0f, a);
        g.setColour (fg2.withAlpha (0.6f));
        g.drawLine ({ p0, p1 }, 1.0f);
    }
    // pointer
    const juce::Point<float> q0 = c.getPointOnCircumference (trackR * 0.25f, angle), q1 = c.getPointOnCircumference (trackR - 6.0f, angle);
    g.setColour (valCol);
    g.drawLine ({ q0, q1 }, 2.0f);
    if (props.contains ("hot") && (float) props["hot"] > 0.01f)
    {
        g.setColour (amber.withAlpha ((float) props["hot"] * 0.8f));
        g.drawEllipse (c.x - trackR - 1, c.y - trackR - 1, 2 * trackR + 2, 2 * trackR + 2, 1.5f);
    }
    if (learning)
    {
        g.setColour (amber.withAlpha (0.25f));
        g.fillEllipse (c.x - trackR + 4, c.y - trackR + 4, 2 * (trackR - 4), 2 * (trackR - 4));
    }
}

void CockpitLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool over, bool down)
{
    auto r = b.getLocalBounds().toFloat().reduced (0.5f);
    const juce::Colour accent = b.getProperties().contains ("accent") ? juce::Colour ((juce::uint32) (juce::int64) b.getProperties()["accent"]) : cyan;
    g.setColour (down ? accent.withAlpha (0.35f) : (over ? accent.withAlpha (0.16f) : bg1));
    g.fillRoundedRectangle (r, 2.0f);
    g.setColour (over ? accent : accent.withAlpha (0.55f));
    g.drawRoundedRectangle (r, 2.0f, 1.0f);
}

void CockpitLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& b, bool over, bool)
{
    const juce::Colour accent = b.getProperties().contains ("accent") ? juce::Colour ((juce::uint32) (juce::int64) b.getProperties()["accent"]) : cyan;
    g.setColour (over ? fg : accent);
    g.setFont (mono (11.5f, true));
    g.drawText (b.getButtonText(), b.getLocalBounds(), juce::Justification::centred);
}

void CockpitLookAndFeel::drawComboBox (juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& box)
{
    auto r = juce::Rectangle<float> (0, 0, (float) w, (float) h).reduced (0.5f);
    g.setColour (bg1);
    g.fillRoundedRectangle (r, 2.0f);
    g.setColour (box.isMouseOver() ? cyan2 : rule2);
    g.drawRoundedRectangle (r, 2.0f, 1.0f);
    juce::Path arrow;
    const float ax = (float) w - 11.0f, ay = (float) h * 0.5f;
    arrow.addTriangle (ax - 4, ay - 2, ax + 4, ay - 2, ax, ay + 3);
    g.setColour (cyan2);
    g.fillPath (arrow);
}

juce::Font CockpitLookAndFeel::getComboBoxFont (juce::ComboBox&) { return mono (11.0f); }
juce::Font CockpitLookAndFeel::getPopupMenuFont() { return mono (12.0f); }

void CockpitLookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int w, int h)
{
    g.fillAll (bg1);
    g.setColour (rule2);
    g.drawRect (0, 0, w, h, 1);
}
