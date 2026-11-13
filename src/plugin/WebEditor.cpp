/*
 * OkumuLab 1 — the Phase 4 screen (see WebEditor.h)
 */
#include "WebEditor.h"
#include "Simd.h"
#include "Presets.h"

#include "OklWebData.h"

using namespace okl;

namespace
{
using Opt = juce::WebBrowserComponent::Options;

juce::String mimeFor (const juce::String& path)
{
    const auto ext = path.fromLastOccurrenceOf (".", false, false).toLowerCase();
    if (ext == "html") return "text/html";
    if (ext == "js" || ext == "mjs") return "text/javascript";
    if (ext == "css") return "text/css";
    if (ext == "json") return "application/json";
    if (ext == "woff2") return "font/woff2";
    if (ext == "png") return "image/png";
    if (ext == "svg") return "image/svg+xml";
    return "application/octet-stream";
}

juce::String b64 (const float* x, int n)
{
    return n > 0 ? juce::Base64::toBase64 (x, (size_t) n * sizeof (float)) : juce::String();
}

double nowMs() { return juce::Time::getMillisecondCounterHiRes(); }

juce::DynamicObject::Ptr obj() { return new juce::DynamicObject(); }

#if JUCE_WINDOWS
/* WebView2's profile, in the user's folder. Without one, WebView2 makes "<host>.exe.WebView2" next to the host's exe
   and does not start where it cannot (a DAW in Program Files): the check below failed there and the DAW got the
   native screen */
juce::File webViewDataFolder()
{
    const auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                         .getChildFile ("OkumuLab").getChildFile ("OkumuLab 1").getChildFile ("WebView2");
    if (dir.createDirectory().wasOk()) return dir;
    return juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("OkumuLab 1 WebView2");
}
#endif

/* Windows: WebView2. macOS: WKWebView (part of the system). Linux: WebKitGTK, loaded when the screen opens
   (libwebkit2gtk-4.1 or 4.0; without it the native screen opens) */
Opt backendOptions()
{
#if JUCE_WINDOWS
    return Opt {}
        .withBackend (Opt::Backend::webview2)
        .withWinWebView2Options (Opt::WinWebView2 {}
                                     .withUserDataFolder (webViewDataFolder())
                                     .withBackgroundColour (juce::Colour (0xff020509))
                                     .withStatusBarDisabled()
                                     .withBuiltInErrorPageDisabled());
#else
    return Opt {}.withBackend (Opt::Backend::defaultBackend);
#endif
}
} // namespace

/* checked with the same profile folder the screen will use */
bool WebEditor::available()
{
    return juce::WebBrowserComponent::areOptionsSupported (backendOptions());
}

juce::WebBrowserComponent::Options WebEditor::makeOptions()
{
    return backendOptions()
        .withNativeIntegrationEnabled()
        .withKeepPageLoadedWhenBrowserIsHidden()
        .withEventListener ("okl", [this] (const juce::var& m) { onMessage (m); })
        .withResourceProvider ([this] (const juce::String& url) { return resource (url); });
}

WebEditor::WebEditor (OkumuLabProcessor& p)
    : AudioProcessorEditor (&p),
      proc (p),
      zip (std::make_unique<juce::ZipFile> (new juce::MemoryInputStream (OklWeb::okl_web_zip, (size_t) OklWeb::okl_web_zipSize, false), true)),
      browser (makeOptions())
{
    lastSent.fill (std::numeric_limits<float>::quiet_NaN());
    outTmp.assign ((size_t) 1 << 15, 0.0f);
    sigTmp.assign ((size_t) 1 << 15, 0.0f);
    perfPath = juce::SystemStats::getEnvironmentVariable ("OKL_PERFTEST", {});

    addAndMakeVisible (browser);
    browser.goToURL (juce::WebBrowserComponent::getResourceProviderRoot());
    proc.setUiStreams (true);
    proc.setCaptureEnabled (true);

    setResizable (true, true);
    getConstrainer()->setFixedAspectRatio ((double) kW / kH);
    setResizeLimits (800, 500, 2560, 1600);
    setSize (kW, kH);
    startTimerHz (60);
}

WebEditor::~WebEditor()
{
    stopTimer();
    for (int i = 0; i < kNumParams; ++i)
        if (gestureUntil[(size_t) i] > 0) proc.param (i)->endChangeGesture();
    proc.midi().learnParam.store (-1);
    proc.setUiStreams (false);
    proc.setCaptureEnabled (false);
}

void WebEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff020509));
}

void WebEditor::resized()
{
    browser.setBounds (getLocalBounds());
}

/* the embedded web/ folder */
std::optional<juce::WebBrowserComponent::Resource> WebEditor::resource (const juce::String& url)
{
    auto path = url.upToFirstOccurrenceOf ("?", false, false).trimCharactersAtStart ("/");
    if (path.isEmpty()) path = "index.html";
    if (auto it = cache.find (path); it != cache.end()) return it->second;
    const int idx = zip->getIndexOfFileName (path);
    if (idx < 0) return std::nullopt;
    std::unique_ptr<juce::InputStream> in (zip->createStreamForEntry (idx));
    if (! in) return std::nullopt;
    juce::MemoryBlock mb;
    in->readIntoMemoryBlock (mb);
    juce::WebBrowserComponent::Resource r;
    r.data.resize (mb.getSize());
    std::memcpy (r.data.data(), mb.getData(), mb.getSize());
    r.mimeType = mimeFor (path);
    cache[path] = r;
    return r;
}

// ---------------------------------------------------------------- page -> plugin
void WebEditor::onMessage (const juce::var& m)
{
    const auto t = m["t"].toString();
    if (t == "ready")
    {
        sendInit();
        if (! ready)
        {
            ready = true;
            readyAt = nowMs();
            proc.resetAudioStats();
        }
    }
    else if (t == "param")
    {
        const int idx = paramIndex (m["key"].toString().toRawUTF8());
        if (idx < 0) return;
        auto* p = proc.param (idx);
        const float v = (float) (double) m["v"];
        if (gestureUntil[(size_t) idx] <= 0) p->beginChangeGesture();
        gestureUntil[(size_t) idx] = nowMs() + 300.0;        // the gesture ends when the slider stops
        p->setValueNotifyingHost (p->convertTo0to1 (v));
        lastSent[(size_t) idx] = proc.apvts.getRawParameterValue (kParamDefs[idx].id)->load();
    }
    else if (t == "midi")
    {
        const auto* b = m["b"].getArray();
        if (b && b->size() >= 3) proc.pushUiMidi ((int) (*b)[0], (int) (*b)[1], (int) (*b)[2]);
    }
    else if (t == "reset") proc.resetAllParameters();
    else if (t == "resetParam")                                              // v1.1.1: a row's RESET button
    {
        const int idx = paramIndex (m["key"].toString().toRawUTF8());
        if (idx < 0) return;
        if (gestureUntil[(size_t) idx] > 0)                                  // a slider move still open: close it first
        {
            proc.param (idx)->endChangeGesture();
            gestureUntil[(size_t) idx] = 0;
        }
        proc.resetParameter (idx);
    }
    else if (t == "preset") proc.setCurrentProgram ((int) m["i"]);          // V2: a stop (the host's program)
    else if (t == "panic") proc.panic();
    else if (t == "learn")
    {
        const int idx = paramIndex (m["key"].toString().toRawUTF8());
        auto& lp = proc.midi().learnParam;
        lp.store (lp.load() == idx ? -1 : idx);              // right-click again cancels
    }
    else if (t == "fps")
    {
        const double ts = nowMs() - readyAt;
        fpsSamples.push_back ({ ts, (double) m["fps"], (double) m["maxMs"], (double) m["p99Ms"], (int) m["longFrames"], (double) m["workMs"], (double) m["telMs"], (double) m["workMaxMs"] });
        if (fpsSamples.size() > 3600) fpsSamples.erase (fpsSamples.begin());
        screenInfo = m;
    }
    else if (t == "log")
    {
        jsLog.add (m["msg"].toString());
        DBG (m["msg"].toString());
    }
    // v2.0: Scala files: a file dialog, a file dropped on the page (its text), or none
    else if (t == "tuningBrowse") browseTuning (m["kind"].toString() == "kbm");
    else if (t == "tuningLoad")
    {
        const auto text = m["text"].toString();
        if (text.getNumBytesAsUTF8() <= (1 << 20)) proc.loadTuningText (m["kind"].toString() == "kbm", text, m["name"].toString());
    }
    else if (t == "tuningClear") proc.clearTuning (m["kind"].toString() == "kbm");
}

void WebEditor::browseTuning (bool kbm)
{
    chooser = std::make_unique<juce::FileChooser> (kbm ? "Keyboard mapping (Scala .kbm)" : "Scale (Scala .scl)", juce::File(),
                                                   kbm ? "*.kbm" : "*.scl");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this, kbm] (const juce::FileChooser& fc) {
                              const auto f = fc.getResult();
                              if (f != juce::File()) proc.loadTuningFile (kbm, f);
                          });
}

void WebEditor::sendTuning()
{
    const auto& tf = proc.tuningFiles();
    const auto& ct = proc.customTuning();
    Globals g;
    g.temperament = proc.apvts.getRawParameterValue ("temperament")->load();
    g.pitchA = proc.apvts.getRawParameterValue ("pitchA")->load();
    auto o = obj();
    o->setProperty ("t", "tuning");
    o->setProperty ("scl", tf.sclName);
    o->setProperty ("kbm", tf.kbmName);
    o->setProperty ("description", tf.description);
    o->setProperty ("notes", tf.notes);
    o->setProperty ("error", tf.error);
    o->setProperty ("loaded", ct.valid);
    o->setProperty ("usesPitch", ct.usesPitch);
    juce::Array<juce::var> semis, mapped;
    for (int k = 0; k < 128; ++k)
    {
        double s = 0.0;
        const bool on = keyTuning (g, ct, k, s);
        semis.add (s);
        mapped.add (on);
    }
    o->setProperty ("semis", semis);
    o->setProperty ("mapped", mapped);
    browser.emitEventIfBrowserIsVisible ("okl", o.get());
    lastTuneSerial = proc.tuningSerial();
    lastTemper = (float) g.temperament;
    lastPitchA = (float) g.pitchA;
}

void WebEditor::sendInit()
{
    auto o = obj();
    o->setProperty ("t", "init");
    o->setProperty ("fs", proc.getSampleRate() > 0 ? proc.getSampleRate() : 48000.0);
    o->setProperty ("os", proc.getSampleRate() < 70000.0 ? 2 : 1);
    o->setProperty ("maxVoices", Engine::kMaxVoices);
    o->setProperty ("version", JucePlugin_VersionString);
    juce::String host (juce::AudioProcessor::getWrapperTypeDescription (proc.wrapperType));
    const juce::PluginHostType hostType;
    if (hostType.type != juce::PluginHostType::UnknownHost)
        host << " · " << hostType.getHostDescription();
    o->setProperty ("host", host);
    o->setProperty ("padChannel", proc.midi().padChannel.load());
    auto params = obj(), defaults = obj(), cc = obj();
    for (int i = 0; i < kNumParams; ++i)
    {
        const float v = proc.apvts.getRawParameterValue (kParamDefs[i].id)->load();
        params->setProperty (kParamDefs[i].id, v);
        defaults->setProperty (kParamDefs[i].id, proc.paramDefault (i));     // v1.1.1: what RESET sets (V2: the stop's)
        lastSent[(size_t) i] = v;
        lastCc[(size_t) i] = proc.controllerText (i);
        cc->setProperty (kParamDefs[i].id, lastCc[(size_t) i]);
    }
    o->setProperty ("params", params.get());
    o->setProperty ("defaults", defaults.get());
    o->setProperty ("cc", cc.get());
    // MALLET: the material tables (what the panel and the HUD show about the metal and the head)
    {
        juce::Array<juce::var> metals, heads;
        for (int i = 0; i < kNumMetals; ++i)
        {
            const auto& mp = metalProps (i);
            auto mo = obj();
            mo->setProperty ("name", juce::String::fromUTF8 (mp.name)); mo->setProperty ("E", mp.E); mo->setProperty ("rho", mp.rho);
            mo->setProperty ("nu", mp.nu); mo->setProperty ("eta", mp.eta);
            metals.add (mo.get());
        }
        for (int i = 0; i < kNumHeads; ++i)
        {
            const auto& hp = headProps (i);
            auto ho = obj();
            ho->setProperty ("name", juce::String::fromUTF8 (hp.name)); ho->setProperty ("E", hp.E); ho->setProperty ("rho", hp.rho); ho->setProperty ("e", hp.e);
            ho->setProperty ("p", hp.p); ho->setProperty ("K", hp.K);          // V2: the contact law (yarn: a measured power law)
            heads.add (ho.get());
        }
        o->setProperty ("metals", metals);
        o->setProperty ("heads", heads);
        // V2: the woods of a wooden pipe
        juce::Array<juce::var> woods;
        for (int i = 0; i < kNumWoods; ++i)
        {
            const auto& wp = woodProps (i);
            auto wo = obj();
            wo->setProperty ("name", juce::String::fromUTF8 (wp.name)); wo->setProperty ("EL", wp.EL); wo->setProperty ("ET", wp.ETL * wp.EL);
            wo->setProperty ("rho", wp.rho); wo->setProperty ("eta", wp.eta);
            woods.add (wo.get());
        }
        o->setProperty ("woods", woods);
    }
    // the programs (V2: the stops), and the names of the matrix sources and targets
    {
        juce::Array<juce::var> pl;
        for (const auto& pr : factoryPresets())
        {
            auto po = obj();
            po->setProperty ("name", juce::String::fromUTF8 (pr.name));
            po->setProperty ("recipe", juce::String::fromUTF8 (pr.recipe));
            pl.add (po.get());
        }
        o->setProperty ("presets", pl);
        o->setProperty ("program", proc.getCurrentProgram());
        lastProgram = proc.getCurrentProgram();
        juce::Array<juce::var> src, dst;
        for (int i = 0; i < MS_COUNT; ++i) src.add (juce::String::fromUTF8 (kModSrcNames[i]));
        for (int i = 0; i < kModDstCount; ++i) dst.add (juce::String (kModDstIds[i]));
        o->setProperty ("modSources", src);
        o->setProperty ("modTargets", dst);
        o->setProperty ("simd", juce::String (simdLevel()));
    }
    // P5: the room the reverb is computed from
    {
        const auto& room = proc.engineRoom();
        auto ro = obj();
        juce::Array<juce::var> t60;
        for (double v : room.t60) t60.add (v);
        ro->setProperty ("t60", t60);
        ro->setProperty ("V", room.V); ro->setProperty ("dist", room.directDist); ro->setProperty ("first", room.firstReflection);
        ro->setProperty ("dr", room.drRatioDb);
        o->setProperty ("room", ro.get());
    }
    midiRead = proc.midi().messageCount.load();
    browser.emitEventIfBrowserIsVisible ("okl", o.get());
    sendTuning();                                    // v2.0
    // the streams start now
    proc.readOutput (outTmp.data(), (int) outTmp.size());
    proc.readFocusSignal (sigTmp.data(), (int) sigTmp.size());
}

// ---------------------------------------------------------------- plugin -> page
void WebEditor::timerCallback()
{
    const double now = nowMs();
    for (int i = 0; i < kNumParams; ++i)
        if (gestureUntil[(size_t) i] > 0 && now > gestureUntil[(size_t) i])
        {
            proc.param (i)->endChangeGesture();
            gestureUntil[(size_t) i] = 0;
        }
    if (! ready) return;
    const double t0 = nowMs();
    sendFrame();
    const double used = nowMs() - t0;
    sendMsSum += used; sendMsMax = juce::jmax (sendMsMax, used); ++sendN;
    if (perfPath.isNotEmpty()) perfTest (now - readyAt);
}

void WebEditor::sendFrame()
{
    Telemetry tl;
    proc.readTelemetry (tl, nullptr, 0);
    auto o = obj();
    o->setProperty ("t", "tel");
    o->setProperty ("time", juce::Time::getMillisecondCounterHiRes() * 0.001);
    o->setProperty ("cpu", proc.cpuLoad());

    // telemetry in the Phase 2 format (js/engine-core.js Engine.telemetry)
    auto te = obj();
    juce::Array<juce::var> vox;
    for (int i = 0; i < tl.nVox; ++i)
    {
        const auto& v = tl.vox[i];
        auto vo = obj();
        vo->setProperty ("id", v.id); vo->setProperty ("midi", v.midi); vo->setProperty ("gate", v.gate);
        vo->setProperty ("peak", v.peak); vo->setProperty ("pf", v.pf); vo->setProperty ("age", (int) v.age);
        vox.add (vo.get());
    }
    te->setProperty ("vox", vox);
    te->setProperty ("gr", tl.gr);
    if (tl.focusId >= 0)
    {
        auto f = obj();
        f->setProperty ("id", tl.focusId); f->setProperty ("midi", tl.focusMidi); f->setProperty ("gate", tl.focusGate);
        f->setProperty ("pg", tl.pg); f->setProperty ("pf", tl.pf); f->setProperty ("Uj", tl.Uj);
        f->setProperty ("etaN", tl.etaN); f->setProperty ("inflow", tl.inflow); f->setProperty ("vm", tl.vm);
        f->setProperty ("resets", tl.resets); f->setProperty ("fres", tl.fres); f->setProperty ("Leff", tl.Leff);
        f->setProperty ("M", tl.M); f->setProperty ("W", tl.W); f->setProperty ("dline", tl.dline);
        f->setProperty ("d", tl.d); f->setProperty ("H", tl.H); f->setProperty ("h", tl.h); f->setProperty ("toe", tl.toe);
        f->setProperty ("rho", tl.rho); f->setProperty ("c", tl.c); f->setProperty ("kappa", tl.kappa); f->setProperty ("y0", tl.y0);
        f->setProperty ("morph", tl.morph); f->setProperty ("ftarget", tl.fTarget); f->setProperty ("ampcap", tl.ampcap);
        f->setProperty ("bjet", tl.bJet);
        f->setProperty ("pchest", tl.pchest);
        // V2: the focus pipe's stop, a rectangular body's depth, its ears, arched cut-up, wood
        f->setProperty ("stop", tl.focusStop); f->setProperty ("depth", tl.depth); f->setProperty ("ears", tl.ears);
        f->setProperty ("arch", tl.arch); f->setProperty ("wood", tl.wood);
        // P3 pitch-lock servo
        f->setProperty ("servo", tl.servoCents); f->setProperty ("locked", tl.locked);
        f->setProperty ("lockCents", tl.cents); f->setProperty ("fMeas", tl.fMeas); f->setProperty ("ratio", tl.ratio);
        te->setProperty ("focus", f.get());
    }
    else te->setProperty ("focus", juce::var());
    te->setProperty ("excite", tl.excite);
    te->setProperty ("stop", tl.stop);
    // P5: modulation matrix: the sources as the focus voice sees them, and each slot's output
    if (tl.focusId >= 0)
    {
        auto mo = obj();
        juce::Array<juce::var> sv, ov;
        for (int i = 0; i < MS_COUNT; ++i) sv.add (tl.modSrc[i]);
        for (int i = 0; i < kModSlots; ++i) ov.add (tl.modOut[i]);
        mo->setProperty ("src", sv);
        mo->setProperty ("out", ov);
        te->setProperty ("mod", mo.get());
    }
    // MALLET: the struck pipe in focus
    if (tl.focusId >= 0 && tl.mallet.strikes > 0)
    {
        const auto& ml = tl.mallet;
        auto mo = obj();
        mo->setProperty ("strikes", ml.strikes); mo->setProperty ("contacts", ml.contacts);
        mo->setProperty ("v0", ml.v0); mo->setProperty ("fPeak", ml.fPeak); mo->setProperty ("tContact", ml.tContact);
        mo->setProperty ("h", ml.h); mo->setProperty ("K", ml.K); mo->setProperty ("p", ml.p); mo->setProperty ("mass", ml.mass);
        mo->setProperty ("nModes", ml.nModes); mo->setProperty ("nActive", ml.nActive); mo->setProperty ("forceSpan", ml.forceSpan);
        auto arr = [] (const float* x, int n) { juce::Array<juce::var> a; for (int i = 0; i < n; ++i) a.add (x[i]); return a; };
        mo->setProperty ("force", arr (ml.force, MalletTelemetry::kForce));
        mo->setProperty ("ring", arr (ml.ring, MalletTelemetry::kRing));
        mo->setProperty ("bend", arr (ml.bend, MalletTelemetry::kBend));
        juce::Array<juce::var> modes;
        for (int i = 0; i < ml.nShown; ++i)
        {
            const auto& md = ml.modes[i];
            modes.add (juce::Array<juce::var> { md.f, md.level, md.t60, md.n, md.m });
        }
        mo->setProperty ("modes", modes);
        te->setProperty ("mallet", mo.get());
    }
    o->setProperty ("tel", te.get());

    // signals since the last frame
    o->setProperty ("out", b64 (outTmp.data(), proc.readOutput (outTmp.data(), (int) outTmp.size())));
    o->setProperty ("sig", b64 (sigTmp.data(), proc.readFocusSignal (sigTmp.data(), (int) sigTmp.size())));

    // the bore over one period
    int capId = -1;
    if (proc.readCapture (cap, capId, capSerial))
    {
        auto c = obj();
        c->setProperty ("id", capId); c->setProperty ("midi", cap.midi);
        c->setProperty ("K", K_SNAP); c->setProperty ("NX", NX_SNAP);
        c->setProperty ("p", b64 (cap.p, K_SNAP * NX_SNAP)); c->setProperty ("u", b64 (cap.u, K_SNAP * NX_SNAP));
        c->setProperty ("eta", b64 (cap.eta, K_SNAP)); c->setProperty ("inflow", b64 (cap.inflow, K_SNAP)); c->setProperty ("vm", b64 (cap.vm, K_SNAP));
        c->setProperty ("T", cap.periodSamples); c->setProperty ("fs", cap.fs); c->setProperty ("dline", cap.dline); c->setProperty ("Leff", cap.Leff);
        o->setProperty ("cap", c.get());
    }

    // parameters the host / a controller moved, controller tags
    juce::DynamicObject::Ptr params, cc;
    for (int i = 0; i < kNumParams; ++i)
    {
        const float v = proc.apvts.getRawParameterValue (kParamDefs[i].id)->load();
        if (! (v == lastSent[(size_t) i]))
        {
            if (! params) params = obj();
            params->setProperty (kParamDefs[i].id, v);
            lastSent[(size_t) i] = v;
        }
        const auto ct = proc.controllerText (i);
        if (ct != lastCc[(size_t) i])
        {
            if (! cc) cc = obj();
            cc->setProperty (kParamDefs[i].id, ct);
            lastCc[(size_t) i] = ct;
        }
    }
    if (params) o->setProperty ("params", params.get());
    if (cc) o->setProperty ("cc", cc.get());

    // MIDI activity
    auto& rt = proc.midi();
    const uint32_t count = rt.messageCount.load (std::memory_order_acquire);
    if (count - midiRead > (uint32_t) MidiRouter::kLog) midiRead = count - (uint32_t) MidiRouter::kLog;
    if (count != midiRead)
    {
        juce::Array<juce::var> list;
        for (; midiRead != count; ++midiRead)
        {
            const uint32_t pk = rt.log[(size_t) (midiRead % MidiRouter::kLog)].load (std::memory_order_relaxed);
            list.add (juce::Array<juce::var> { (int) (pk & 0xff), (int) ((pk >> 8) & 0xff), (int) ((pk >> 16) & 0xff) });
        }
        o->setProperty ("midi", list);
    }
    // pads, MIDI learn, pad channel
    {
        auto pads = obj();
        juce::Array<juce::var> a, b;
        for (int i = 0; i < 8; ++i) { a.add (proc.padPressure (0, i)); b.add (proc.padPressure (1, i)); }
        pads->setProperty ("a", a);
        pads->setProperty ("b", b);
        o->setProperty ("pads", pads.get());
    }
    const int learn = rt.learnParam.load();
    o->setProperty ("learn", learn >= 0 && learn < kNumParams ? juce::var (kParamDefs[learn].id) : juce::var());
    if (proc.getCurrentProgram() != lastProgram)
    {
        // V2: another stop: its defaults are what RESET sets now
        lastProgram = proc.getCurrentProgram();
        o->setProperty ("program", lastProgram);
        auto defaults = obj();
        for (int i = 0; i < kNumParams; ++i) defaults->setProperty (kParamDefs[i].id, proc.paramDefault (i));
        o->setProperty ("defaults", defaults.get());
    }
    const int pc = rt.padChannel.load();
    if (pc != lastPadChannel) { o->setProperty ("padChannel", pc); lastPadChannel = pc; }

    browser.emitEventIfBrowserIsVisible ("okl", o.get());
    // v2.0: the tuning, when a file, the Temperament or the Pitch changed
    if (proc.tuningSerial() != lastTuneSerial || proc.apvts.getRawParameterValue ("temperament")->load() != lastTemper
        || proc.apvts.getRawParameterValue ("pitchA")->load() != lastPitchA)
        sendTuning();
}

// ---------------------------------------------------------------- OKL_PERFTEST
/* a scripted session: settle, chord, knob sweeps, pad pressure, release; then the report */
void WebEditor::perfTest (double ms)
{
    auto at = [&] (int stage, double t) { if (perfStage == stage && ms >= t) { ++perfStage; return true; } return false; };
    if (at (0, 2000))
    {
        fpsSamples.clear();
        proc.resetAudioStats();
        for (int n : { 36, 48, 55, 60, 64 }) proc.pushUiMidi (0x90, n, 100);
    }
    if (perfStage >= 1 && perfStage <= 3 && ms >= 4000 && ms < 10000)
    {
        // a knob turning continuously, as a MIDI controller would do it
        const double x = (ms - 4000) / 6000.0;
        auto set = [&] (const char* id, double v) {
            auto* p = proc.param (paramIndex (id));
            p->setValueNotifyingHost (p->convertTo0to1 ((float) v));
        };
        set ("cutup", 1.0 + 0.5 * std::sin (x * juce::MathConstants<double>::twoPi * 2.0));
        set ("morph", 0.5 - 0.5 * std::cos (x * juce::MathConstants<double>::twoPi));
        set ("bellows", std::pow (2.0, std::sin (x * juce::MathConstants<double>::twoPi * 3.0)));     // Wind pressure 0.5 .. 2
    }
    if (at (1, 7000)) { proc.pushUiMidi (0x99, 36, 100); proc.pushUiMidi (0xA9, 36, 110); }       // pad A1: wind x5
    if (at (2, 8500)) proc.pushUiMidi (0x89, 36, 0);
    if (at (3, 10500))
    {
        for (int n : { 36, 48, 55, 60, 64 }) proc.pushUiMidi (0x80, n, 0);
        proc.resetAllParameters();
    }
    // v1.1.1: a row's RESET button. Cut-up and Labium offset leave their defaults, then the page presses Cut-up's button:
    // only Cut-up returns, and only Labium offset's button stays lit
    const juce::String rowState ("const q = (k) => document.querySelector('.prm[data-key=\"' + k + '\"]');"
                                 "const lit = (k) => !q(k).classList.contains('atdef');"
                                 "const s = { cutupLit: lit('cutup'), y0bLit: lit('y0b'), cutupShown: OKL.params.ctx.G.cutup,"
                                 " rows: document.querySelectorAll('.prm').length, buttons: document.querySelectorAll('.prm > .prm-r').length };");
    auto keep = [this] (const char* name) {
        return [this, name] (juce::WebBrowserComponent::EvaluationResult r) {
            if (resetCheck == nullptr) return;
            if (const auto* v = r.getResult()) resetCheck->setProperty (name, *v);
            else if (const auto* e = r.getError()) resetCheck->setProperty (name, "error: " + e->message);
        };
    };
    if (at (4, 11000))
    {
        resetCheck = obj();
        for (auto [id, v] : { std::pair<const char*, float> { "cutup", 1.6f }, { "y0b", 1.0f } })
        {
            auto* p = proc.param (paramIndex (id));
            p->beginChangeGesture();
            p->setValueNotifyingHost (p->convertTo0to1 (v));
            p->endChangeGesture();
        }
    }
    if (at (5, 11800))
        browser.evaluateJavascript ("(() => { " + rowState + " q('cutup').querySelector('.prm-r').click(); return s; })()", keep ("before"));
    if (at (6, 12600) && resetCheck != nullptr)
    {
        resetCheck->setProperty ("cutup", proc.apvts.getRawParameterValue ("cutup")->load());
        resetCheck->setProperty ("y0b", proc.apvts.getRawParameterValue ("y0b")->load());
        browser.evaluateJavascript ("(() => { " + rowState + " return s; })()", keep ("after"));
    }
    /* V2: the stop menu (where the experiment recipes were): the page selects the Gedackt; every knob goes to the Gedackt as
       built (closed 100 %, ears, arch 0 %, its scale and wall loss), the panels show its wood instead of the metal; a key plays
       a Gedackt pipe (keys outside MIDI 24-96 nothing); Open/Stopped moved and its RESET button pressed returns to 100 % */
    const juce::String stopState ("const q = (k) => document.querySelector('.prm[data-key=\"' + k + '\"]');"
                                  "const shown = (k) => !!q(k) && getComputedStyle(q(k)).display !== 'none';"
                                  "const sel = document.getElementById('presetSel');"
                                  "const s = { sel: sel ? sel.value : null, options: sel ? sel.options.length : 0, label: sel ? sel.options[sel.selectedIndex].text : '',"
                                  " stop: OKL.params.ctx.G.stop, morph: OKL.params.ctx.G.morph, morphLit: !q('morph').classList.contains('atdef'),"
                                  " metalShown: shown('metal'), woodShown: shown('wood'), earsShown: shown('ears'), archShown: shown('arch'),"
                                  " morphText: q('morph').querySelector('.prm-v').textContent, rows: document.querySelectorAll('.prm').length };");
    auto keepStop = [this] (const char* name) {
        return [this, name] (juce::WebBrowserComponent::EvaluationResult r) {
            if (stopCheck == nullptr) return;
            if (const auto* v = r.getResult()) stopCheck->setProperty (name, *v);
            else if (const auto* e = r.getError()) stopCheck->setProperty (name, "error: " + e->message);
        };
    };
    if (at (7, 13400))
    {
        stopCheck = obj();
        stopCheck->setProperty ("stopBefore", proc.currentStop());
        browser.evaluateJavascript ("(() => { " + stopState + " sel.value = '1'; sel.dispatchEvent(new Event('change')); return s; })()", keepStop ("pageBefore"));
    }
    if (at (8, 14600) && stopCheck != nullptr)
    {
        for (const char* id : { "stop", "morph", "ears", "arch", "loss", "scaleHT", "mouthFrac", "wood" })
            stopCheck->setProperty (id, proc.apvts.getRawParameterValue (id)->load());
        stopCheck->setProperty ("program", proc.getCurrentProgram());
        stopCheck->setProperty ("programName", proc.getProgramName (proc.getCurrentProgram()));
        browser.evaluateJavascript ("(() => { " + stopState + " return s; })()", keepStop ("page"));
        proc.pushUiMidi (0x90, 60, 100);
        proc.pushUiMidi (0x90, 23, 100);
        proc.pushUiMidi (0x90, 97, 100);
    }
    if (at (9, 15400) && stopCheck != nullptr)
    {
        Telemetry tl;
        proc.readTelemetry (tl, nullptr, 0);
        stopCheck->setProperty ("voices", tl.nVox);
        stopCheck->setProperty ("focusMidi", tl.focusMidi);
        stopCheck->setProperty ("focusStop", tl.focusStop);
        stopCheck->setProperty ("focusDepth", tl.depth);
        for (int n : { 60, 23, 97 }) proc.pushUiMidi (0x80, n, 0);
        auto* p = proc.param (paramIndex ("morph"));
        p->beginChangeGesture();
        p->setValueNotifyingHost (p->convertTo0to1 (0.4f));
        p->endChangeGesture();
    }
    if (at (10, 16200) && stopCheck != nullptr)
        browser.evaluateJavascript ("(() => { " + stopState + " q('morph').querySelector('.prm-r').click(); return s; })()", keepStop ("moved"));
    if (at (11, 17000) && stopCheck != nullptr)
        browser.evaluateJavascript ("(() => { " + stopState + " return s; })()", [this] (juce::WebBrowserComponent::EvaluationResult r) {
            if (stopCheck == nullptr) return;
            // the plugin's value read with the page's answer: the button's message came before it, however slow the page
            stopCheck->setProperty ("morphAfterReset", proc.apvts.getRawParameterValue ("morph")->load());
            if (const auto* v = r.getResult()) stopCheck->setProperty ("afterReset", *v);
            else if (const auto* e = r.getError()) stopCheck->setProperty ("afterReset", "error: " + e->message);
        });
    // the report once the page has answered (a slow page: up to 25 s)
    if (perfStage == 12 && ms >= 17800 && (stopCheck == nullptr || stopCheck->hasProperty ("afterReset") || ms >= 25000))
    {
        ++perfStage;
        writePerfReport();
        perfPath.clear();
        if (juce::JUCEApplicationBase::isStandaloneApp())
            juce::MessageManager::callAsync ([] { juce::JUCEApplicationBase::quit(); });
    }
}

void WebEditor::writePerfReport()
{
    auto r = obj();
    // frame rate: only while notes sound and knobs move (2 .. 10.5 s)
    double sum = 0, mn = 1e9, worstP99 = 0, worstMax = 0;
    int n = 0, longFrames = 0;
    juce::Array<juce::var> fpsList;
    for (const auto& s : fpsSamples)
    {
        if (s.t < 2500 || s.t > 10500) continue;
        sum += s.fps; mn = juce::jmin (mn, s.fps); worstP99 = juce::jmax (worstP99, s.p99Ms); worstMax = juce::jmax (worstMax, s.maxMs);
        longFrames += s.longFrames; ++n;
        fpsList.add (juce::String (s.fps, 1) + " fps, js " + juce::String (s.workMs, 0) + " ms/s (max frame " + juce::String (s.workMaxMs, 1) + " ms), tel " + juce::String (s.telMs, 0) + " ms/s");
    }
    auto f = obj();
    f->setProperty ("seconds", n);
    f->setProperty ("mean", n ? sum / n : 0.0);
    f->setProperty ("min", n ? mn : 0.0);
    f->setProperty ("worst_p99_ms", worstP99);
    f->setProperty ("worst_frame_ms", worstMax);
    f->setProperty ("frames_over_25ms", longFrames);
    f->setProperty ("per_second", fpsList);
    r->setProperty ("fps", f.get());
    const auto a = proc.audioStats();
    auto au = obj();
    au->setProperty ("sample_rate", proc.getSampleRate());
    au->setProperty ("block", proc.getBlockSize());
    au->setProperty ("callbacks", (int) a.blocks);
    au->setProperty ("overruns", (int) a.overruns);
    au->setProperty ("late_callbacks", (int) a.late);
    au->setProperty ("max_load", a.maxLoad);
    au->setProperty ("max_gap_blocks", a.maxGap);
    au->setProperty ("dsp_load_now", proc.cpuLoad());
    r->setProperty ("audio", au.get());
    r->setProperty ("screen", screenInfo);
    r->setProperty ("reset_button", resetCheck.get());
    r->setProperty ("stop_menu", stopCheck.get());
    r->setProperty ("cpp_send_ms_avg", sendN ? sendMsSum / sendN : 0.0);
    r->setProperty ("cpp_send_ms_max", sendMsMax);
    juce::Array<juce::var> logs;
    for (const auto& s : jsLog) logs.add (s);
    r->setProperty ("js_messages", logs);
    juce::File (perfPath).replaceWithText (juce::JSON::toString (r.get()));
}
