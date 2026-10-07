/*
 * OkumuLab 1 — plugin editor
 */
#include "PluginEditor.h"

using namespace okl;
using namespace okl::ui;

namespace
{

juce::String fmt (const ParamDef& d, double v)
{
    char buf[32];
    formatValue (d, v, buf);
    return juce::String::fromUTF8 (buf);
}

juce::String midiText (uint32_t m)
{
    const int st = (int) (m & 0xff), d1 = (int) ((m >> 8) & 0xff), d2 = (int) ((m >> 16) & 0xff);
    const int ch = (st & 0x0f) + 1;
    switch (st & 0xf0)
    {
        case 0x90: return "CH" + juce::String (ch) + (d2 ? " NOTE ON " : " NOTE OFF ") + juce::String (d1) + (d2 ? " v" + juce::String (d2) : "");
        case 0x80: return "CH" + juce::String (ch) + " NOTE OFF " + juce::String (d1);
        case 0xA0: return "CH" + juce::String (ch) + " POLY AT " + juce::String (d1) + " " + juce::String (d2);
        case 0xB0: return "CH" + juce::String (ch) + " CC" + juce::String (d1) + " " + juce::String (d2);
        case 0xD0: return "CH" + juce::String (ch) + " AT " + juce::String (d1);
        case 0xE0: return "CH" + juce::String (ch) + " PITCH " + juce::String (((d2 << 7) | d1) - 8192);
        default: return st ? "SYS" : "-";
    }
}
} // namespace

// =================================================================== knob
OklKnob::OklKnob (OkumuLabProcessor& p, int index, bool large) : proc (p), idx (index), big (large)
{
    const ParamDef& d = kParamDefs[idx];
    slider.onRightClick = [this] { showMenu(); };
    slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    slider.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
    slider.setMouseDragSensitivity (big ? 260 : 220);
    slider.getProperties().set ("defPos", (float) toNorm (d, d.def));
    slider.getProperties().set ("bipolar", d.bipolar);
    slider.addListener (this);
    addAndMakeVisible (slider);
    attach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, d.id, slider);
    slider.setDoubleClickReturnValue (true, d.def);
    lastValue = (float) slider.getValue();
    ccText = proc.controllerText (idx);
    slider.setTooltip (juce::String::fromUTF8 (d.name) + "  (" + juce::String::fromUTF8 (d.tag) + ")\n"
                       + "Right-click: MIDI learn / forget / default.  Double-click: default.");
}

OklKnob::~OklKnob()
{
    slider.removeListener (this);
}

void OklKnob::setBand (double v0, double v1)
{
    const ParamDef& d = kParamDefs[idx];
    const double a = juce::jlimit (0.0, 1.0, toNorm (d, juce::jlimit (d.min, d.max, v0)));
    const double b = juce::jlimit (0.0, 1.0, toNorm (d, juce::jlimit (d.min, d.max, v1)));
    slider.getProperties().set ("band0", (float) juce::jmin (a, b));
    slider.getProperties().set ("band1", (float) juce::jmax (a, b));
}

void OklKnob::resized()
{
    auto r = getLocalBounds();
    r.removeFromTop (big ? 16 : 14);
    r.removeFromBottom (big ? 30 : 27);
    const int sz = juce::jmin (r.getWidth() - 6, r.getHeight());
    slider.setBounds (r.withSizeKeepingCentre (sz, sz));
}

void OklKnob::paint (juce::Graphics& g)
{
    const ParamDef& d = kParamDefs[idx];
    auto r = getLocalBounds().toFloat();
    g.setColour (hot > 0.02f ? fg.interpolatedWith (amber, hot) : fg2);
    g.setFont (sans (big ? 12.5f : 11.5f, true));
    g.drawText (juce::String::fromUTF8 (d.name), r.removeFromTop (big ? 16.0f : 14.0f), juce::Justification::centred, true);
    auto bottom = r.removeFromBottom (big ? 30.0f : 27.0f);
    g.setColour (fg);
    g.setFont (mono (big ? 13.0f : 12.0f, true));
    g.drawText (fmt (d, slider.getValue()), bottom.removeFromTop (big ? 16.0f : 15.0f), juce::Justification::centred, true);
    g.setColour (ccText.isNotEmpty() ? cyan2 : dim2);
    g.setFont (mono (9.5f));
    g.drawText ((ccText.isNotEmpty() ? ccText + " · " : juce::String()) + juce::String::fromUTF8 (d.tag), bottom, juce::Justification::centred, true);
}

void OklKnob::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu()) showMenu();
}

void OklKnob::showMenu()
{
    auto& router = proc.midi();
    const ParamDef& d = kParamDefs[idx];
    juce::PopupMenu m;
    m.addSectionHeader (juce::String::fromUTF8 (d.name));
    const bool learning = router.learnParam.load() == idx;
    m.addItem (1, learning ? "Cancel MIDI learn" : "MIDI learn (move a controller)");
    m.addItem (2, "Forget controller", ccText.isNotEmpty() && ! ccText.startsWith ("PB"));
    m.addSeparator();
    m.addItem (3, "Reset to default (" + fmt (d, d.def) + ")");
    m.addItem (4, "Default controller map (MiniLab 3)");
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&slider), [this, learning] (int r) {
        auto& rt = proc.midi();
        if (r == 1) rt.learnParam.store (learning ? -1 : idx);
        else if (r == 2) { for (auto& c : rt.ccMap) if (c.load() == idx) c.store (-1); }
        else if (r == 3)
        {
            auto* p = proc.param (idx);
            p->beginChangeGesture();
            p->setValueNotifyingHost (p->getDefaultValue());
            p->endChangeGesture();
        }
        else if (r == 4) rt.resetCcMap();
        ccText = proc.controllerText (idx);
        repaint();
    });
}

void OklKnob::tick (const Telemetry& t, bool learning)
{
    const ParamDef& d = kParamDefs[idx];
    // moved by a controller / host automation (not by the mouse): flash
    const float v = (float) slider.getValue();
    bool dirty = false;
    if (v != lastValue)
    {
        if (! dragging) hot = 1.0f;
        lastValue = v;
        dirty = true;
    }
    else if (hot > 0.0f)
    {
        hot = juce::jmax (0.0f, hot - 0.06f);
        dirty = true;
    }
    slider.getProperties().set ("hot", hot);
    if (dirty) repaint();
    const bool wasLearning = (bool) slider.getProperties().getWithDefault ("learn", false);
    const bool blink = learning && (juce::Time::getMillisecondCounter() / 300) % 2 == 0;
    if (learning || wasLearning)
    {
        slider.getProperties().set ("learn", blink);
        slider.repaint();
    }
    if (! learning && wasLearning) slider.getProperties().set ("learn", false);
    const juce::String cc = proc.controllerText (idx);
    if (cc != ccText) { ccText = cc; repaint(); }
    // the cut-up band follows the focus pipe: Ising number 2..3
    if (std::string (d.id) == "cutup" && t.focusMidi >= 0 && t.pf > 1.0f && t.W > 0 && t.fTarget > 0)
    {
        const double cur = slider.getValue();
        auto wFor = [&] (double I) { return std::cbrt (2.0 * t.pf * t.h / (t.rho * I * I * t.fTarget * t.fTarget)); };
        setBand (wFor (3.0) / t.W * cur, wFor (2.0) / t.W * cur);
    }
}

// ================================================================= editor
NativeEditor::NativeEditor (OkumuLabProcessor& p)
    : AudioProcessorEditor (&p), proc (p), keyboard (p.keyboardState, juce::MidiKeyboardComponent::horizontalKeyboard)
{
    setLookAndFeel (&lnf);
    addAndMakeVisible (content);

    // panel geometry (base size kW x kH)
    const int L = 12, colW = 840, R = L + colW + 12, rightW = kW - R - 12;
    headerR = { 0, 0, (float) kW, 58 };
    auto box = [&] (int x, int y, int w, int h, const char* num, const char* title, const char* sub) {
        panels.push_back ({ juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h), num, juce::String::fromUTF8 (title), juce::String::fromUTF8 (sub) });
        return juce::Rectangle<int> (x, y, w, h);
    };
    const auto pMidi = box (L, 66, colW, 170, "01", "MIDI CONTROLLER", "8 KNOBS · MINILAB 3 USER PROGRAM");
    const int unit = (colW - 3 * 8) / 10;
    const auto pWind = box (L, 244, 3 * unit, 160, "02", "WIND", "BELLOWS · PALLET");
    const auto pVoice = box (L + 3 * unit + 8, 244, 3 * unit, 160, "03", "VOICING", "MOUTH · JET · TOE");
    const auto pShape = box (L + 6 * unit + 16, 244, 2 * unit, 160, "04", "GEOMETRY", "PIPE BODY");
    const auto pAtmos = box (L + 8 * unit + 24, 244, colW - (8 * unit + 24), 160, "05", "ATMOSPHERE", "c · rho");
    const int unit2 = (colW - 8) / 10;
    labPanel = (int) panels.size();
    const auto pLab = box (L, 412, 8 * unit2, 160, "06", "LAB", "IMPOSSIBLE PHYSICS");
    const auto pOut = box (L + 8 * unit2 + 8, 412, colW - (8 * unit2 + 8), 160, "07", "OUTPUT", "ROOM · LEVEL");
    statusR = box (R, 66, rightW, 360, "08", "STATUS", "FOCUS PIPE · PITCH LOCK").toFloat();
    boreR = box (R, 434, rightW, 138, "09", "STANDING WAVE", "MOUTH -> TOP · ONE PERIOD").toFloat();
    footerR = { (float) L, 578.0f, (float) (kW - 2 * L), 24.0f };

    layoutPanel (Group::Midi, pMidi, true);
    layoutPanel (Group::Wind, pWind, false);
    layoutPanel (Group::Voice, pVoice, false);
    layoutPanel (Group::Shape, pShape, false);
    layoutPanel (Group::Atmos, pAtmos, false);
    layoutPanel (Group::Lab, pLab, false, &labKnobs);
    layoutPanel (Group::Mallet, pLab, false, &malletKnobs, "excite");
    layoutPanel (Group::Output, pOut, false);

    // static real-instrument bands (green): the Phase 2 panel bands
    for (auto& k : knobs)
    {
        const std::string id = kParamDefs[k->index()].id;
        if (id == "bellows") k->setBand (69.0 * MMWS / kWindRefPa, 97.0 * MMWS / kWindRefPa);    // Baroque organ examples, 69-97 mmWS
        else if (id == "mouthFrac") k->setBand (1.0 / 7.0, 2.0 / 7.0);
        else if (id == "morph") k->setBand (0, 0);
        else if (id == "gas") k->setBand (0, 0);
        else if (id == "cMult" || id == "rhoMult" || id == "jetGain") k->setBand (1, 1);
    }

    resetButton.getProperties().set ("accent", (juce::int64) cyan.getARGB());
    panicButton.getProperties().set ("accent", (juce::int64) red.getARGB());
    resetButton.setTooltip ("Every sound parameter back to its default");
    panicButton.setTooltip ("All sound off");
    resetButton.onClick = [this] { proc.resetAllParameters(); };
    panicButton.onClick = [this] { proc.panic(); };
    content.addAndMakeVisible (resetButton);
    content.addAndMakeVisible (panicButton);
    resetButton.setBounds (kW - 12 - 2 * 76 - 8, 16, 76, 26);
    panicButton.setBounds (kW - 12 - 76, 16, 76, 26);

    padChannel.addItem ("PADS OFF", 1);
    for (int c = 1; c <= 16; ++c) padChannel.addItem ("PADS CH " + juce::String (c), c + 1);
    padChannel.setSelectedId (proc.midi().padChannel.load() + 2, juce::dontSendNotification);
    padChannel.onChange = [this] { proc.midi().padChannel.store (padChannel.getSelectedId() - 2); };
    padChannel.setTooltip ("MIDI channel of the pads (MiniLab 3: 10). Bank A notes 36-43 = modulation pads, bank B 44-51 = wind pipes");
    content.addAndMakeVisible (padChannel);
    padChannel.setBounds (kW - 12 - 132, 578, 132, 22);

    // MALLET: wind or mallet (a host parameter)
    excite.addItemList ({ "BLOW (WIND)", "STRIKE (MALLET)" }, 1);
    excite.setTooltip ("Excitation: the pipe blown by the wind, or struck with a hard mallet");
    content.addAndMakeVisible (excite);
    excite.setBounds (kW - 12 - 132 - 8 - 150, 578, 150, 22);
    exciteAttach = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (proc.apvts, "excite", excite);

    // on-screen keyboard (plays like channel 1; the Standalone can be tried without a controller)
    keyboard.setAvailableRange (24, 108);
    keyboard.setLowestVisibleKey (24);
    keyboard.setKeyWidth ((kW - 24) / 50.0f);
    keyboard.setScrollButtonsVisible (false);
    keyboard.setOctaveForMiddleC (4);
    keyboard.setColour (juce::MidiKeyboardComponent::whiteNoteColourId, juce::Colour (0xffb9cdd6));
    keyboard.setColour (juce::MidiKeyboardComponent::blackNoteColourId, bg1);
    keyboard.setColour (juce::MidiKeyboardComponent::keySeparatorLineColourId, rule2);
    keyboard.setColour (juce::MidiKeyboardComponent::keyDownOverlayColourId, cyan.withAlpha (0.75f));
    keyboard.setColour (juce::MidiKeyboardComponent::mouseOverKeyOverlayColourId, cyan.withAlpha (0.25f));
    keyboard.setColour (juce::MidiKeyboardComponent::shadowColourId, bg0);
    keyboard.setColour (juce::MidiKeyboardComponent::textLabelColourId, bg1);
    keyboard.setColour (juce::MidiKeyboardComponent::upDownButtonBackgroundColourId, bg1);
    content.addAndMakeVisible (keyboard);
    keyboard.setBounds (12, 606, kW - 24, 82);
    snapshotPath = juce::SystemStats::getEnvironmentVariable ("OKL_SNAPSHOT", {});

    proc.setCaptureEnabled (true);
    setResizable (true, true);
    getConstrainer()->setFixedAspectRatio ((double) kW / kH);
    setResizeLimits (kW * 6 / 10, kH * 6 / 10, kW * 2, kH * 2);
    setSize (kW, kH);
    startTimerHz (30);
}

NativeEditor::~NativeEditor()
{
    stopTimer();
    proc.setCaptureEnabled (false);
    proc.midi().learnParam.store (-1);
    knobs.clear();
    setLookAndFeel (nullptr);
}

void NativeEditor::layoutPanel (Group grp, juce::Rectangle<int> area, bool large, std::vector<OklKnob*>* made, const char* skip)
{
    std::vector<int> ids;
    for (int i = 0; i < kNumParams; ++i)
        if (kParamDefs[i].group == grp && ! (skip && std::string (kParamDefs[i].id) == skip)) ids.push_back (i);
    if (grp == Group::Midi)
        std::sort (ids.begin(), ids.end(), [] (int a, int b) { return kParamDefs[a].slot < kParamDefs[b].slot; });
    auto inner = area.reduced (6, 0).withTrimmedTop (24).withTrimmedBottom (6);
    const int n = (int) ids.size();
    const int cw = inner.getWidth() / juce::jmax (1, n);
    for (int k = 0; k < n; ++k)
    {
        auto knob = std::make_unique<OklKnob> (proc, ids[(size_t) k], large);
        knob->setBounds (inner.getX() + k * cw, inner.getY(), cw, inner.getHeight());
        content.addAndMakeVisible (*knob);
        if (made) made->push_back (knob.get());
        knobs.push_back (std::move (knob));
    }
}

/* MALLET: the LAB knobs (jet physics) have nothing to act on in a struck pipe: the panel shows the mallet instead */
void NativeEditor::showMalletPanel (bool on)
{
    if (malletShown == (int) on) return;
    malletShown = (int) on;
    for (auto* k : labKnobs) k->setVisible (! on);
    for (auto* k : malletKnobs) k->setVisible (on);
    if (labPanel >= 0)
    {
        panels[(size_t) labPanel].title = on ? "MALLET" : "LAB";
        panels[(size_t) labPanel].sub = on ? juce::String::fromUTF8 ("METAL · WALL · HEAD · STRIKE") : juce::String ("IMPOSSIBLE PHYSICS");
    }
    content.repaint();
}

void NativeEditor::resized()
{
    const float s = (float) getWidth() / (float) kW;
    content.setBounds (0, 0, kW, kH);
    content.setTransform (juce::AffineTransform::scale (s));
}

void NativeEditor::paint (juce::Graphics& g)
{
    g.fillAll (bg0);
}

void NativeEditor::paintContent (juce::Graphics& g)
{
    // background: dark field with a faint grid
    g.fillAll (bg0);
    g.setColour (cyan.withAlpha (0.035f));
    for (int x = 0; x < kW; x += 24) g.drawVerticalLine (x, 0, (float) kH);
    for (int y = 0; y < kH; y += 24) g.drawHorizontalLine (y, 0, (float) kW);

    paintHeader (g, headerR);
    for (auto& p : panels) drawPanel (g, p.r, p.num, p.title, p.sub);
    paintStatus (g, statusR);
    paintBore (g, boreR);

    // footer
    auto f = footerR;
    g.setColour (rule);
    g.drawHorizontalLine ((int) f.getY(), f.getX(), f.getRight());
    g.setColour (dim);
    g.setFont (mono (10.5f));
    g.drawText (juce::String::fromUTF8 ("PADS: A 36–43 modulation · B 44–51 wind pipes   PB glide · CC1 wind FM · CC11 wind pressure · CC64 hold   "
                                        "knob: double-click = default · right-click = MIDI learn · green = real pipes"),
                f.withTrimmedRight (150), juce::Justification::centredLeft, true);
}

void NativeEditor::paintHeader (juce::Graphics& g, juce::Rectangle<float> r)
{
    juce::ColourGradient grad (bg1, 0, 0, bg0, 0, r.getBottom(), false);
    g.setGradientFill (grad);
    g.fillRect (r);
    g.setColour (rule2);
    g.drawHorizontalLine ((int) r.getBottom() - 1, 0, r.getRight());
    // logo
    g.setColour (cyan.withAlpha (0.18f));
    g.setFont (disp (30.0f));
    g.drawText ("OkumuLab 1", juce::Rectangle<float> (15, 7, 260, 34), juce::Justification::centredLeft);
    g.setColour (fg);
    g.drawText ("OkumuLab 1", juce::Rectangle<float> (14, 6, 260, 34), juce::Justification::centredLeft);
    g.setColour (cyan2);
    g.setFont (mono (10.0f));
    g.drawText (juce::String::fromUTF8 ("LABIUM · PRINZIPAL 8′ · FLUE PIPE PHYSICAL MODEL · v") + JucePlugin_VersionString,
                juce::Rectangle<float> (16, 38, 420, 14), juce::Justification::centredLeft);

    // status chips
    auto chip = [&] (float x, float w, const juce::String& k, const juce::String& v, juce::Colour vc) {
        juce::Rectangle<float> c (x, 14, w, 30);
        g.setColour (bg0.withAlpha (0.7f));
        g.fillRoundedRectangle (c, 2.0f);
        g.setColour (rule2);
        g.drawRoundedRectangle (c, 2.0f, 1.0f);
        g.setColour (dim);
        g.setFont (mono (9.0f));
        g.drawText (k, c.withHeight (12).translated (6, 2), juce::Justification::centredLeft);
        g.setColour (vc);
        g.setFont (mono (12.5f, true));
        g.drawText (v, c.withTrimmedTop (12).translated (6, 0).withTrimmedRight (8), juce::Justification::centredLeft);
    };
    const double hz = haveTel ? tel.hostFs : 48000.0;
    const int os = haveTel ? tel.osFactor : 2;
    chip (450, 120, "ENGINE", juce::String (hz / 1000.0, 1) + "k x" + juce::String (os) + " -> " + juce::String (hz * os / 1000.0, 0) + "k", fg);
    chip (576, 70, "VOICES", haveTel ? juce::String (tel.nActive) + "/" + juce::String (tel.maxVoices) : "-", fg);
    const float cpu = proc.cpuLoad() * 100.0f;
    chip (652, 66, "DSP", juce::String (cpu, 1) + "%", cpu > 70 ? red : (cpu > 40 ? amber : fg));
    const float grDb = haveTel ? 20.0f * std::log10 (juce::jmax (1e-6f, tel.gr)) : 0.0f;
    chip (724, 74, "LIMITER", juce::String (grDb, 1) + " dB", grDb < -0.5f ? amber : fg);
    // MIDI
    {
        juce::Rectangle<float> c (804, 14, 190, 30);
        g.setColour (bg0.withAlpha (0.7f));
        g.fillRoundedRectangle (c, 2.0f);
        g.setColour (rule2);
        g.drawRoundedRectangle (c, 2.0f, 1.0f);
        g.setColour (dim);
        g.setFont (mono (9.0f));
        g.drawText ("MIDI IN", c.withHeight (12).translated (18, 2), juce::Justification::centredLeft);
        g.setColour (midiLed > 0.05f ? green.withAlpha (0.4f + 0.6f * midiLed) : dim2);
        g.fillEllipse (c.getX() + 6, c.getY() + 5, 7, 7);
        g.setColour (fg2);
        g.setFont (mono (11.0f, true));
        g.drawText (midiText (proc.midi().lastMessage.load()), c.withTrimmedTop (12).translated (6, 0).withTrimmedRight (8), juce::Justification::centredLeft);
    }
}

void NativeEditor::paintStatus (juce::Graphics& g, juce::Rectangle<float> r)
{
    auto a = r.reduced (12, 0).withTrimmedTop (26).withTrimmedBottom (8);
    const bool focus = haveTel && tel.focusMidi >= 0;
    char nm[8] = "-";
    if (focus) organName (tel.focusMidi, nm);
    // big name
    auto top = a.removeFromTop (44);
    g.setColour (focus ? fg : dim2);
    g.setFont (disp (36.0f));
    g.drawText (focus ? juce::String (nm) : juce::String ("--"), top.removeFromLeft (110), juce::Justification::centredLeft);
    g.setFont (mono (11.0f));
    g.setColour (dim);
    g.drawText (focus ? "MIDI " + juce::String (tel.focusMidi) : "PLAY A KEY", top.removeFromTop (18), juce::Justification::centredLeft);
    const bool struck = haveTel && tel.excite;
    g.setColour (focus && tel.focusGate ? green : (focus ? amber : dim2));
    g.setFont (mono (11.0f, true));
    g.drawText (focus ? (struck ? (tel.focusGate ? "RINGING" : "DAMPED") : (tel.focusGate ? "SPEAKING" : "RELEASED")) : "IDLE", top, juce::Justification::centredLeft);
    if (struck)
    {
        // MALLET: the stroke and the strongest modes of the wall
        const auto& ml = tel.mallet;
        a.removeFromTop (6);
        struct Row { juce::String k, v; };
        std::vector<Row> rows;
        const int mi = (int) proc.apvts.getRawParameterValue ("metal")->load(), hi = (int) proc.apvts.getRawParameterValue ("head")->load();
        rows.push_back ({ "Pipe metal", juce::String::fromUTF8 (metalProps (mi).name) });
        rows.push_back ({ "Mallet head", juce::String::fromUTF8 (headProps (hi).name) + "  " + juce::String (ml.mass * 1000.0f, 1) + " g" });
        rows.push_back ({ "Wall / diameter", focus ? juce::String (ml.h * 1000.0f, 2) + " mm / " + juce::String (tel.d * 1000.0f, 1) + " mm" : "-" });
        rows.push_back ({ "Stroke", ml.strikes > 0 ? juce::String (ml.v0, 2) + " m/s  " + juce::String (ml.fPeak, 1) + " N  " + juce::String (ml.tContact * 1000.0f, 3) + " ms" : "-" });
        rows.push_back ({ "Modes ringing", juce::String (ml.nActive) + " / " + juce::String (ml.nModes) });
        rows.push_back ({ "Air column (passive)", focus ? juce::String (tel.fres, 1) + " Hz" : "-" });
        for (int i = 0; i < juce::jmin (ml.nShown, 8); ++i)
        {
            const auto& md = ml.modes[i];
            rows.push_back ({ "  n" + juce::String (md.n) + " m" + juce::String (md.m), juce::String (md.f, 1) + " Hz  " + juce::String (md.level, 0) + " dB  T60 " + juce::String (md.t60, 2) + " s" });
        }
        const float rh = juce::jmin (16.0f, a.getHeight() / (float) rows.size());
        for (const auto& row : rows)
        {
            auto rowR = a.removeFromTop (rh);
            g.setColour (dim);
            g.setFont (sans (11.5f));
            g.drawText (row.k, rowR, juce::Justification::centredLeft);
            g.setColour (fg);
            g.setFont (mono (11.5f, true));
            g.drawText (row.v, rowR, juce::Justification::centredRight);
        }
        return;
    }

    // pitch lock meter (+-10 cents, +-2 cents band)
    a.removeFromTop (6);
    g.setFont (mono (10.5f));
    g.setColour (dim);
    const juce::String regime = ! focus || tel.ratio <= 0 ? "-" :
        (std::abs (tel.ratio - 1.0f) < 0.01f ? "FUNDAMENTAL" :
         (std::abs (tel.ratio - 0.5f) < 0.01f ? "STOPPED  (x1/2)" : "REGIME  x" + juce::String (tel.ratio, 1)));
    g.drawText ("KEY " + (focus ? juce::String (tel.fTarget, 2) + " Hz" : juce::String ("-")), a.removeFromTop (15), juce::Justification::centredLeft);
    g.drawText ("SOUNDS " + (focus && tel.fMeas > 0 ? juce::String (tel.fMeas, 2) + " Hz" : juce::String ("-")) + "   " + regime,
                a.removeFromTop (15), juce::Justification::centredLeft);
    a.removeFromTop (4);
    auto meter = a.removeFromTop (34);
    {
        auto bar = meter.removeFromTop (14).reduced (2, 0);
        g.setColour (bg0);
        g.fillRect (bar);
        g.setColour (rule2);
        g.drawRect (bar, 1.0f);
        auto xOf = [&] (float c) { return bar.getX() + (juce::jlimit (-10.0f, 10.0f, c) + 10.0f) / 20.0f * bar.getWidth(); };
        g.setColour (green.withAlpha (0.18f));
        g.fillRect (juce::Rectangle<float>::leftTopRightBottom (xOf (-2), bar.getY() + 1, xOf (2), bar.getBottom() - 1));
        g.setColour (dim2);
        for (int c = -10; c <= 10; c += 2) g.drawVerticalLine ((int) xOf ((float) c), bar.getY() + (c % 10 ? 9.0f : 3.0f), bar.getBottom());
        g.setColour (fg2);
        g.drawVerticalLine ((int) xOf (0), bar.getY(), bar.getBottom());
        if (focus && tel.fMeas > 0)
        {
            const bool inBand = std::abs (tel.cents) <= 2.0f;
            g.setColour (inBand ? green : amber);
            const float x = xOf (tel.cents);
            g.fillRect (x - 1.5f, bar.getY() - 2, 3.0f, bar.getHeight() + 4);
        }
        g.setFont (mono (11.0f, true));
        g.setColour (focus && tel.fMeas > 0 ? (std::abs (tel.cents) <= 2.0f ? green : amber) : dim);
        const juce::String lockTxt = "PITCH " + (focus && tel.fMeas > 0 ? juce::String (tel.cents >= 0 ? "+" : "") + juce::String (tel.cents, 2) + juce::String::fromUTF8 (" ¢") : juce::String ("-"))
                                     + "   LOCK " + juce::String ((int) std::round (tel.lock * 100)) + "%"
                                     + (focus ? "   SERVO " + juce::String (tel.servoCents >= 0 ? "+" : "") + juce::String (tel.servoCents, 1) + juce::String::fromUTF8 (" ¢") : juce::String());
        g.drawText (lockTxt, meter.translated (2, 2), juce::Justification::centredLeft);
    }

    // physical state table
    a.removeFromTop (6);
    g.setColour (rule);
    g.drawHorizontalLine ((int) a.getY(), a.getX(), a.getRight());
    a.removeFromTop (6);
    struct Row { const char* k; juce::String v; };
    auto mm = [] (float m) { return juce::String (m * 1000.0f, m < 0.01f ? 2 : 1) + " mm"; };
    const Row rows[] = {
        { "L eff (acoustic)", focus ? mm (tel.Leff) : "-" },
        { "Diameter", focus ? mm (tel.d) : "-" },
        { "Mouth width H", focus ? mm (tel.H) : "-" },
        { "Cut-up W", focus ? mm (tel.W) : "-" },
        { "Flue h", focus ? mm (tel.h) : "-" },
        { "Toe hole", focus ? mm (tel.toe) : "-" },
        { "Wind (chest)", focus ? juce::String (tel.pchest / MMWS, 1) + " mmWS" : "-" },
        { "Foot pressure", focus ? juce::String (tel.pf / MMWS, 1) + " mmWS" : "-" },
        { "Jet velocity", focus ? juce::String (tel.Uj, 1) + " m/s" : "-" },
        { "Ising number I", focus && tel.ising > 0 ? juce::String (tel.ising, 2) : "-" },
        { "Sound speed / density", focus ? juce::String (tel.c, 0) + " m/s  " + juce::String (tel.rho, 3) : "-" },
        { "Jet / inflow", focus ? juce::String (tel.etaN, 2) + " b  " + juce::String ((int) std::round (tel.inflow * 100)) + "%" : "-" },
        { "Resets", focus ? juce::String (tel.resets) : "-" },
    };
    const float rh = juce::jmin (16.0f, a.getHeight() / (float) std::size (rows));
    for (const auto& row : rows)
    {
        auto rowR = a.removeFromTop (rh);
        g.setColour (dim);
        g.setFont (sans (11.5f));
        g.drawText (row.k, rowR, juce::Justification::centredLeft);
        g.setColour (fg);
        g.setFont (mono (11.5f, true));
        g.drawText (row.v, rowR, juce::Justification::centredRight);
    }
}

void NativeEditor::paintBore (juce::Graphics& g, juce::Rectangle<float> r)
{
    auto a = r.reduced (12, 0).withTrimmedTop (26).withTrimmedBottom (8);
    auto wave = a.removeFromTop (a.getHeight() * 0.58f);
    auto sc = a.withTrimmedTop (6);
    // standing wave: |p| envelope (filled), one phase-aligned frame (line)
    g.setColour (bg0);
    g.fillRect (wave);
    g.setColour (rule);
    g.drawRect (wave, 1.0f);
    const float cy = wave.getCentreY(), hh = wave.getHeight() * 0.42f;
    juce::Path env, frame;
    for (int i = 0; i < NX_SNAP; ++i)
    {
        const float x = wave.getX() + 2 + (wave.getWidth() - 4) * i / (float) (NX_SNAP - 1);
        const float y = cy - boreShown[i] * hh;
        if (i == 0) env.startNewSubPath (x, y); else env.lineTo (x, y);
    }
    for (int i = NX_SNAP - 1; i >= 0; --i)
        env.lineTo (wave.getX() + 2 + (wave.getWidth() - 4) * i / (float) (NX_SNAP - 1), cy + boreShown[i] * hh);
    env.closeSubPath();
    g.setColour (cyan.withAlpha (0.22f));
    g.fillPath (env);
    g.setColour (cyan.withAlpha (0.7f));
    g.strokePath (env, juce::PathStrokeType (1.0f));
    if (haveTel)
    {
        for (int i = 0; i < NX_SNAP; ++i)
        {
            const float x = wave.getX() + 2 + (wave.getWidth() - 4) * i / (float) (NX_SNAP - 1);
            const float y = cy - tel.boreSigned[i] * hh;
            if (i == 0) frame.startNewSubPath (x, y); else frame.lineTo (x, y);
        }
        g.setColour (amber.withAlpha (0.8f));
        g.strokePath (frame, juce::PathStrokeType (1.2f));
    }
    g.setColour (dim);
    g.setFont (mono (9.0f));
    g.drawText ("MOUTH", wave.reduced (4, 2), juce::Justification::bottomLeft);
    g.drawText ("TOP", wave.reduced (4, 2), juce::Justification::bottomRight);

    // mouth signal (last ~10 ms at the host rate)
    g.setColour (bg0);
    g.fillRect (sc);
    g.setColour (rule);
    g.drawRect (sc, 1.0f);
    float pk = 1e-9f;
    for (float v : scope) pk = juce::jmax (pk, std::abs (v));
    juce::Path sp;
    const int n = OkumuLabProcessor::kScope;
    for (int i = 0; i < n; ++i)
    {
        const float x = sc.getX() + 1 + (sc.getWidth() - 2) * i / (float) (n - 1);
        const float y = sc.getCentreY() - scope[i] / pk * sc.getHeight() * 0.42f;
        if (i == 0) sp.startNewSubPath (x, y); else sp.lineTo (x, y);
    }
    g.setColour (green.withAlpha (haveTel && tel.focusMidi >= 0 ? 0.85f : 0.25f));
    g.strokePath (sp, juce::PathStrokeType (1.0f));
    g.setColour (dim);
    g.drawText ("MOUTH SIGNAL", sc.reduced (4, 2), juce::Justification::topLeft);
}

void NativeEditor::timerCallback()
{
    haveTel = proc.readTelemetry (tel, scope, OkumuLabProcessor::kScope) || haveTel;
    const uint32_t mc = proc.midi().messageCount.load();
    if (mc != lastMidiCount) { midiLed = 1.0f; lastMidiCount = mc; }
    else midiLed *= 0.85f;
    if (haveTel)
    {
        if (tel.capSerial != lastCap) lastCap = tel.capSerial;
        for (int i = 0; i < NX_SNAP; ++i) boreShown[i] += (tel.boreP[i] - boreShown[i]) * 0.35f;
    }
    showMalletPanel (proc.apvts.getRawParameterValue ("excite")->load() > 0.5f);
    const int learn = proc.midi().learnParam.load();
    for (auto& k : knobs) k->tick (tel, learn == k->index());
    content.repaint (headerR.toNearestInt());
    content.repaint (statusR.toNearestInt());
    content.repaint (boreR.toNearestInt());

    // OKL_SNAPSHOT=<file.png>: play a key and save a picture of the screen (layout checks)
    if (snapshotPath.isNotEmpty())
    {
        ++frames;
        const int note = juce::SystemStats::getEnvironmentVariable ("OKL_SNAPSHOT_NOTE", "60").getIntValue();
        if (frames == 20) proc.keyboardState.noteOn (1, note, 0.8f);
        if (frames == 100)
        {
            const auto img = content.createComponentSnapshot (content.getLocalBounds(), true, 1.0f);
            juce::File f (snapshotPath);
            f.deleteFile();
            if (auto os = f.createOutputStream()) juce::PNGImageFormat().writeImageToStream (img, *os);
            snapshotPath.clear();
        }
    }
}
