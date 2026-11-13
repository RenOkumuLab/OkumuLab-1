/*
 * OkumuLab 1 — host test: loads the built VST3 the way a DAW does and plays it
 *
 *   okl_hosttest [path/to/OkumuLab 1.vst3] [--no-editor]
 *   okl_hosttest --state-save <older.vst3> <file>, then --state-load <file>: an older build's project opened in this one
 *
 * Checks: plugin description, sound / silence after release, channel-10 pads,
 * controllers -> host parameters, pitch bend, state save/restore, sample rates
 * and block sizes (incl. odd and varying), pitch lock through the plugin, the
 * stops (programs) and the compass MIDI 24-96, CPU load with 16 / 32 voices of
 * each stop, and the screen: the plugin's web editor (WebView2 / WKWebView / WebKitGTK)
 * opened in a window while a real-time audio thread plays (the plugin's
 * OKL_PERFTEST session: frame rate, audio callbacks, page errors).
 * Writes a few renders to test_out/ for listening.
 */
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <vector>
#if defined(__x86_64__) || defined(_M_X64)
 #include <xmmintrin.h>
 #define OKL_HOST_X86 1
#else
 #define OKL_HOST_X86 0
#endif

namespace
{
int g_fail = 0;
int g_hostRounding = 0;     // --render: the host thread's floating-point rounding mode while it calls the plugin (OKL_HOST_ROUNDING, x86-64)
void check (bool ok, const juce::String& what)
{
    std::printf ("  %s %s\n", ok ? "ok  " : "FAIL", what.toRawUTF8());
    if (! ok) ++g_fail;
}

struct Ev { double t; juce::MidiMessage m; };

/* render a scenario: events at times (s), returns stereo audio; varying block sizes if jitter */
juce::AudioBuffer<float> render (juce::AudioPluginInstance& p, double sr, int bs, double dur, std::vector<Ev> evs, bool jitter = false,
                                 double* cpuOut = nullptr)
{
    const int n = (int) (dur * sr);
    juce::AudioBuffer<float> out (2, n);
    out.clear();
    juce::AudioBuffer<float> blk (2, bs);
    std::sort (evs.begin(), evs.end(), [] (const Ev& a, const Ev& b) { return a.t < b.t; });
    size_t ei = 0;
    juce::Random rnd (42);
    double busy = 0;
    for (int pos = 0; pos < n;)
    {
        const int m = juce::jmin (n - pos, jitter ? 1 + rnd.nextInt (bs) : bs);
        juce::MidiBuffer mb;
        while (ei < evs.size() && (int) (evs[ei].t * sr) < pos + m)
        {
            mb.addEvent (evs[ei].m, juce::jmax (0, (int) (evs[ei].t * sr) - pos));
            ++ei;
        }
        blk.setSize (2, m, false, false, true);
        blk.clear();
        const auto t0 = juce::Time::getHighResolutionTicks();
#if OKL_HOST_X86
        const unsigned csr = _mm_getcsr();
        if (g_hostRounding > 0) _mm_setcsr ((csr & ~0x6000u) | ((unsigned) (g_hostRounding & 3) << 13));     // only while the plugin runs
        p.processBlock (blk, mb);
        _mm_setcsr (csr);
#else
        p.processBlock (blk, mb);
#endif
        busy += juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0);
        for (int ch = 0; ch < 2; ++ch) out.copyFrom (ch, pos, blk, ch, 0, m);
        pos += m;
    }
    if (cpuOut) *cpuOut = busy / dur;
    return out;
}

double rms (const juce::AudioBuffer<float>& b, double sr, double t0, double t1)
{
    const int i0 = (int) (t0 * sr), i1 = juce::jmin (b.getNumSamples(), (int) (t1 * sr));
    double s = 0;
    for (int ch = 0; ch < 2; ++ch)
        for (int i = i0; i < i1; ++i) s += (double) b.getSample (ch, i) * b.getSample (ch, i);
    return std::sqrt (s / juce::jmax (1, 2 * (i1 - i0)));
}

bool finiteAndBounded (const juce::AudioBuffer<float>& b, float* peakOut = nullptr)
{
    float pk = 0;
    for (int ch = 0; ch < b.getNumChannels(); ++ch)
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            const float v = b.getSample (ch, i);
            if (! std::isfinite (v)) return false;
            pk = juce::jmax (pk, std::abs (v));
        }
    if (peakOut) *peakOut = pk;
    return pk <= 1.0f;
}

/* strongest spectral partial of channel 0 in [t0, t1] -> cents from the nearest k/2 multiple of fKey */
double pitchCents (const juce::AudioBuffer<float>& b, double sr, double t0, double t1, double fKey, double* fOut)
{
    const int i0 = (int) (t0 * sr), n = (int) ((t1 - t0) * sr);
    size_t N = 1;
    while (N < (size_t) (2 * n)) N <<= 1;
    std::vector<std::complex<double>> a (N);
    for (int i = 0; i < n; ++i)
    {
        const double w = 0.42 - 0.5 * std::cos (2 * juce::MathConstants<double>::pi * i / (n - 1)) + 0.08 * std::cos (4 * juce::MathConstants<double>::pi * i / (n - 1));
        a[(size_t) i] = 0.5 * (b.getSample (0, i0 + i) + b.getSample (1, i0 + i)) * w;
    }
    // FFT
    for (size_t i = 1, j = 0; i < N; ++i)
    {
        size_t bit = N >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap (a[i], a[j]);
    }
    for (size_t len = 2; len <= N; len <<= 1)
    {
        const double ang = -2 * juce::MathConstants<double>::pi / (double) len;
        const std::complex<double> wl (std::cos (ang), std::sin (ang));
        for (size_t i = 0; i < N; i += len)
        {
            std::complex<double> w (1, 0);
            for (size_t j = 0; j < len / 2; ++j) { const auto u = a[i + j], v = a[i + j + len / 2] * w; a[i + j] = u + v; a[i + j + len / 2] = u - v; w *= wl; }
        }
    }
    size_t best = 2;
    for (size_t k = 2; k < N / 2 - 1; ++k) if (std::abs (a[k]) > std::abs (a[best])) best = k;
    const double A = std::log (std::abs (a[best - 1])), B = std::log (std::abs (a[best])), C = std::log (std::abs (a[best + 1]));
    const double f = (best + 0.5 * (A - C) / (A - 2 * B + C)) * sr / (double) N;
    if (fOut) *fOut = f;
    const double r = f / fKey;
    const double cand = 0.5 * juce::jmax (1.0, std::round (2.0 * r));
    return 1200.0 * std::log2 (r / cand);
}

/* level (dB) of the component at frequency f in [t0, t1] (both channels mixed, Blackman window) */
double partialDb (const juce::AudioBuffer<float>& b, double sr, double t0, double t1, double f)
{
    const int i0 = (int) (t0 * sr), n = (int) ((t1 - t0) * sr);
    std::complex<double> s (0, 0);
    double wsum = 0;
    for (int i = 0; i < n; ++i)
    {
        const double w = 0.42 - 0.5 * std::cos (2 * juce::MathConstants<double>::pi * i / (n - 1)) + 0.08 * std::cos (4 * juce::MathConstants<double>::pi * i / (n - 1));
        s += 0.5 * (b.getSample (0, i0 + i) + b.getSample (1, i0 + i)) * w * std::polar (1.0, -2 * juce::MathConstants<double>::pi * f * i / sr);
        wsum += w;
    }
    return 20 * std::log10 (2 * std::abs (s) / wsum + 1e-12);
}

juce::AudioProcessorParameter* findParam (juce::AudioPluginInstance& p, const juce::String& name)
{
    for (auto* q : p.getParameters())
        if (q->getName (64) == name) return q;
    return nullptr;
}

void pump (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); }

void writeWav (const juce::AudioBuffer<float>& b, double sr, const juce::File& f)
{
    f.getParentDirectory().createDirectory();
    f.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> os (f.createOutputStream().release());
    if (! os) return;
    auto w = wav.createWriterFor (os, juce::AudioFormatWriterOptions().withSampleRate (sr).withNumChannels (2).withBitsPerSample (24));
    if (! w) return;
    w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
}

juce::MidiMessage on (int ch, int note, int vel) { return juce::MidiMessage::noteOn (ch, note, (juce::uint8) vel); }
juce::MidiMessage off (int ch, int note) { return juce::MidiMessage::noteOff (ch, note); }

/* the host's audio thread for the editor test: blocks paced in real time, as an audio device would call them */
struct RealtimeAudio : juce::Thread
{
    RealtimeAudio (juce::AudioPluginInstance& pp, double s, int b) : juce::Thread ("okl audio"), p (pp), sr (s), bs (b) {}
    void run() override
    {
        juce::AudioBuffer<float> blk (2, bs);
        juce::MidiBuffer mb;
        const double period = 1000.0 * bs / sr;
        double next = juce::Time::getMillisecondCounterHiRes();
        while (! threadShouldExit())
        {
            blk.clear();
            mb.clear();
            p.processBlock (blk, mb);
            float pk = 0;
            if (! finiteAndBounded (blk, &pk)) bad = true;
            ++blocks;
            next += period;
            for (double now; (now = juce::Time::getMillisecondCounterHiRes()) < next;)
                if (next - now > 2.0) juce::Thread::sleep (1); else juce::Thread::yield();
        }
    }
    juce::AudioPluginInstance& p;
    const double sr;
    const int bs;
    std::atomic<int> blocks { 0 };
    std::atomic<bool> bad { false };
};
} // namespace

/* --render <events.txt> <out.wav> [vst3]: the notes a diagnostic log recorded ("<t> midi 9x nn vv" lines), played
   through the VST3 at 48 kHz in 480-sample blocks on the log's timeline, no editor */
int renderLog (const juce::String& events, const juce::String& wavOut, const juce::String& vst3Path, bool withEditor)
{
    // OKL_HOST_ROUNDING_ALL=1..3: the plugin's whole life (creation, preparation, the OKL_RENDERTEST render) on a
    // thread with that floating-point rounding mode
    if (const int ra = juce::SystemStats::getEnvironmentVariable ("OKL_HOST_ROUNDING_ALL", "0").getIntValue() & 3; ra > 0)
    {
#if OKL_HOST_X86
        _mm_setcsr ((_mm_getcsr() & ~0x6000u) | ((unsigned) ra << 13));
        std::printf ("whole plugin on a thread with rounding mode %d\n", ra);
#else
        std::printf ("OKL_HOST_ROUNDING_ALL: x86-64 only, ignored\n");
#endif
    }
    juce::VST3PluginFormat fmt;
    juce::OwnedArray<juce::PluginDescription> descs;
    fmt.findAllTypesForFile (descs, vst3Path);
    if (descs.isEmpty()) { std::printf ("no plugin found\n"); return 1; }
    // OKL_HOST_FS / OKL_HOST_BS: the host's sample rate and block size (default 48 kHz, 480)
    const double hfs = juce::SystemStats::getEnvironmentVariable ("OKL_HOST_FS", "48000").getDoubleValue();
    const int hbs = juce::SystemStats::getEnvironmentVariable ("OKL_HOST_BS", "480").getIntValue();
    if (hfs != 48000 || hbs != 480) std::printf ("host %.0f Hz, block %d\n", hfs, hbs);
    juce::String err;
    auto p = fmt.createInstanceFromDescription (*descs[0], hfs, hbs, err);
    if (! p) { std::printf ("cannot create instance: %s\n", err.toRawUTF8()); return 1; }
    p->setPlayConfigDetails (0, 2, hfs, hbs);
    p->prepareToPlay (hfs, hbs);
    std::vector<Ev> evs;
    double last = 0;
    for (const auto& line : juce::StringArray::fromLines (juce::File (events).loadFileAsString()))
    {
        const auto tok = juce::StringArray::fromTokens (line, " ", "");
        if (tok.size() < 5 || tok[1] != "midi") continue;
        const double t = tok[0].getDoubleValue();
        evs.push_back ({ t, juce::MidiMessage (tok[2].getHexValue32(), tok[3].getHexValue32(), tok[4].getHexValue32()) });
        last = juce::jmax (last, t);
    }
    std::printf ("%d events, %.1f s%s\n", (int) evs.size(), last + 3.0, withEditor ? ", plugin window open, real-time paced" : "");
    if (! withEditor)
    {
        // OKL_HOST_ROUNDING=1..3: a host thread whose floating-point rounding mode is not round-to-nearest
        g_hostRounding = juce::SystemStats::getEnvironmentVariable ("OKL_HOST_ROUNDING", "0").getIntValue() & 3;
        if (g_hostRounding > 0) std::printf ("host thread rounding mode %d while the plugin runs\n", g_hostRounding);
        const auto b = render (*p, hfs, hbs, last + 3.0, evs);
        writeWav (b, hfs, juce::File (wavOut));
        return 0;
    }
    // with the screen open: an audio thread paced in real time renders the log while the message thread runs the editor
    struct Player : juce::Thread
    {
        Player (juce::AudioPluginInstance& pp, std::vector<Ev> e, double d, double r, int b) : juce::Thread ("okl render"), p (pp), evs (std::move (e)), dur (d), fs (r), bs (b) {}
        void run() override
        {
            const int n = (int) (dur * fs);
            out.setSize (2, n); out.clear();
            juce::AudioBuffer<float> blk (2, bs);
            size_t ei = 0;
            double next = juce::Time::getMillisecondCounterHiRes();
            for (int pos = 0; pos < n && ! threadShouldExit(); pos += bs)
            {
                const int m = juce::jmin (bs, n - pos);
                juce::MidiBuffer mb;
                while (ei < evs.size() && (int) (evs[ei].t * fs) < pos + m) { mb.addEvent (evs[ei].m, juce::jmax (0, (int) (evs[ei].t * fs) - pos)); ++ei; }
                blk.setSize (2, m, false, false, true); blk.clear();
                p.processBlock (blk, mb);
                for (int ch = 0; ch < 2; ++ch) out.copyFrom (ch, pos, blk, ch, 0, m);
                next += 1000.0 * m / fs;
                for (double now; (now = juce::Time::getMillisecondCounterHiRes()) < next;)
                    if (next - now > 2.0) juce::Thread::sleep (1); else juce::Thread::yield();
            }
            done = true;
        }
        juce::AudioPluginInstance& p;
        std::vector<Ev> evs;
        double dur, fs;
        int bs;
        juce::AudioBuffer<float> out;
        std::atomic<bool> done { false };
    };
    std::sort (evs.begin(), evs.end(), [] (const Ev& a, const Ev& b) { return a.t < b.t; });
    std::unique_ptr<juce::AudioProcessorEditor> ed (p->createEditorIfNeeded());
    if (! ed) { std::printf ("no editor\n"); return 1; }
    Player pl (*p, evs, last + 3.0, hfs, hbs);
    {
        juce::DocumentWindow win ("OkumuLab 1 - render", juce::Colours::black, 0);
        win.setUsingNativeTitleBar (true);
        win.setContentNonOwned (ed.get(), true);
        win.centreWithSize (win.getWidth(), win.getHeight());
        win.setVisible (true);
        pump (8000);                                   // the page loads
        pl.startThread (juce::Thread::Priority::highest);
        while (! pl.done) pump (100);
        pl.stopThread (2000);
        win.setContentNonOwned (nullptr, false);
    }
    ed.reset();
    writeWav (pl.out, hfs, juce::File (wavOut));
    return 0;
}

/* a project saved with an older build opened in this one (two runs: one plugin of a kind per process):
     --state-save <older.vst3> <file>   some knobs moved, the state saved
     --state-load <file> [vst3]         the knobs come back, the stop is the Prinzipal (a 1.x state has no Stop), the new
                                        knobs keep their defaults */
const std::pair<const char*, float> kStateMoved[] { { "Cut-up", 0.55f }, { "Wind pressure", 0.6f }, { "Open/Stopped", 0.2f }, { "Reverb", 0.1f } };

std::unique_ptr<juce::AudioPluginInstance> loadPlugin (juce::VST3PluginFormat& fmt, const juce::String& path)
{
    juce::OwnedArray<juce::PluginDescription> ds;
    fmt.findAllTypesForFile (ds, juce::File (path).getFullPathName());
    juce::String err;
    std::unique_ptr<juce::AudioPluginInstance> p;
    if (! ds.isEmpty()) p = fmt.createInstanceFromDescription (*ds[0], 48000, 512, err);
    if (p) { p->setPlayConfigDetails (0, 2, 48000, 512); p->prepareToPlay (48000, 512); }
    return p;
}

int stateSave (const juce::String& oldPath, const juce::String& file)
{
    juce::VST3PluginFormat fmt;
    auto o = loadPlugin (fmt, oldPath);
    if (! o) { std::printf ("FAIL: cannot load %s\n", oldPath.toRawUTF8()); return 1; }
    std::printf ("saved by: %s v%s\n", o->getName().toRawUTF8(), o->getPluginDescription().version.toRawUTF8());
    for (auto [name, v] : kStateMoved) if (auto* q = findParam (*o, name)) q->setValueNotifyingHost (v);
    pump (100);
    render (*o, 48000, 512, 0.05, {});
    for (auto [name, v] : kStateMoved) if (auto* q = findParam (*o, name)) std::printf ("  %-14s %s\n", name, q->getCurrentValueAsText().toRawUTF8());
    juce::MemoryBlock st;
    o->getStateInformation (st);
    return juce::File (file).replaceWithData (st.getData(), st.getSize()) ? 0 : 1;
}

int stateLoad (const juce::String& file, const juce::String& newPath)
{
    juce::VST3PluginFormat fmt;
    juce::MemoryBlock st;
    if (! juce::File (file).loadFileAsData (st)) { std::printf ("FAIL: no state %s\n", file.toRawUTF8()); return 1; }
    auto n = loadPlugin (fmt, newPath);
    if (! n) { std::printf ("FAIL: cannot load %s\n", newPath.toRawUTF8()); return 1; }
    std::printf ("opened in: %s v%s\n", n->getName().toRawUTF8(), n->getPluginDescription().version.toRawUTF8());
    n->setStateInformation (st.getData(), (int) st.getSize());
    pump (100);
    render (*n, 48000, 512, 0.05, {});
    pump (100);
    int diff = 0;
    for (auto [name, v] : kStateMoved)
    {
        auto* qn = findParam (*n, name);
        std::printf ("  %-14s %s\n", name, qn ? qn->getCurrentValueAsText().toRawUTF8() : "-");
        if (! qn || std::abs (qn->getValue() - v) > 2e-3f) ++diff;
    }
    auto* stop = findParam (*n, "Stop");
    auto* ears = findParam (*n, "Ears");
    auto* arch = findParam (*n, "Arched cut-up");
    std::printf ("  Stop %s, Ears %s, Arched cut-up %s\n", stop ? stop->getCurrentValueAsText().toRawUTF8() : "-", ears ? ears->getCurrentValueAsText().toRawUTF8() : "-",
                 arch ? arch->getCurrentValueAsText().toRawUTF8() : "-");
    check (diff == 0, "the older project's knobs come back");
    check (stop && stop->getValue() < 0.01f && ears && ears->getValue() < 0.01f && arch && arch->getValue() < 0.01f,
           "it opens on the Prinzipal, the new knobs at their defaults");
    auto b = render (*n, 48000, 512, 2.0, { { 0.0, on (1, 60, 100) }, { 1.5, off (1, 60) } });
    check (rms (b, 48000, 0.8, 1.4) > 1e-3, "and it plays");
    std::printf ("%s\n", g_fail ? "FAILED" : "OK");
    return g_fail ? 1 : 0;
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    if (argc >= 4 && (juce::String (argv[1]) == "--render" || juce::String (argv[1]) == "--render-editor"))
        return renderLog (argv[2], argv[3], argc >= 5 ? juce::String (argv[4]) : juce::String (OKL_VST3_PATH), juce::String (argv[1]) == "--render-editor");
    if (argc >= 4 && juce::String (argv[1]) == "--state-save") return stateSave (argv[2], argv[3]);
    if (argc >= 3 && juce::String (argv[1]) == "--state-load")
        return stateLoad (argv[2], argc >= 4 ? juce::String (argv[3]) : juce::String (OKL_VST3_PATH));
    juce::String vst3Path (OKL_VST3_PATH);
    bool withEditor = true;
    for (int i = 1; i < argc; ++i)
    {
        const juce::String a (argv[i]);
        if (a == "--no-editor") withEditor = false;
        else vst3Path = a;
    }
    const juce::File vst3 (vst3Path);
    const juce::File outDir = juce::File (OKL_SOURCE_DIR).getChildFile ("test_out");
    std::printf ("OkumuLab 1 host test\nplugin: %s\n", vst3.getFullPathName().toRawUTF8());
    // the editor's scripted session (WebEditor, OKL_PERFTEST) writes its report here
    const juce::File editorReport = outDir.getChildFile ("editor_perf.json");
    if (withEditor)
    {
        outDir.createDirectory();
        editorReport.deleteFile();
#if JUCE_WINDOWS
        _wputenv_s (L"OKL_PERFTEST", editorReport.getFullPathName().toWideCharPointer());
#else
        setenv ("OKL_PERFTEST", editorReport.getFullPathName().toRawUTF8(), 1);
#endif
    }

    juce::VST3PluginFormat fmt;
    juce::OwnedArray<juce::PluginDescription> descs;
    fmt.findAllTypesForFile (descs, vst3.getFullPathName());
    if (descs.isEmpty()) { std::printf ("FAIL: no plugin found\n"); return 1; }
    const auto& d = *descs[0];
    std::printf ("\n== description ==\n  %s | %s | v%s | %s | %s | uid %08x | in %d out %d\n", d.name.toRawUTF8(), d.manufacturerName.toRawUTF8(),
                 d.version.toRawUTF8(), d.category.toRawUTF8(), d.isInstrument ? "instrument" : "effect", (unsigned) d.uniqueId,
                 d.numInputChannels, d.numOutputChannels);
    check (d.name == "OkumuLab 1" && d.manufacturerName == "OkumuLab" && d.isInstrument, "name, vendor, instrument");

    auto make = [&] (double sr, int bs) {
        juce::String err;
        auto p = fmt.createInstanceFromDescription (d, sr, bs, err);
        if (! p) { std::printf ("  cannot create instance: %s\n", err.toRawUTF8()); return std::unique_ptr<juce::AudioPluginInstance>(); }
        p->setPlayConfigDetails (0, 2, sr, bs);
        p->prepareToPlay (sr, bs);
        return p;
    };

    // ---------------------------------------------------------------- basic
    std::printf ("\n== sound and release (48 kHz, 512) ==\n");
    {
        auto p = make (48000, 512);
        if (! p) return 1;
        std::printf ("  parameters: %d, accepts MIDI: %d, latency %d, tail %.1f s\n", (int) p->getParameters().size(), (int) p->acceptsMidi(),
                     p->getLatencySamples(), p->getTailLengthSeconds());
        check (p->getParameters().size() >= 28, "28 parameters exposed");
        auto b = render (*p, 48000, 512, 6.0, { { 0.1, on (1, 60, 100) }, { 2.5, off (1, 60) } });
        float pk = 0;
        const double sus = rms (b, 48000, 1.0, 2.5), tail = rms (b, 48000, 5.5, 6.0);
        const bool fin = finiteAndBounded (b, &pk);
        check (fin, "output finite and within full scale, peak " + juce::String (pk, 3));
        check (sus > 0.01, "c1 sounds: rms " + juce::String (20 * std::log10 (sus), 1) + " dBFS");
        check (tail < sus * 0.03, "falls silent after release: tail " + juce::String (20 * std::log10 (tail + 1e-12) - 20 * std::log10 (sus), 1) + " dB re sustain");
        writeWav (b, 48000, outDir.getChildFile ("01_c1_note.wav"));

        // chord + sustain pedal
        auto c = render (*p, 48000, 512, 5.0, { { 0.1, on (1, 48, 90) }, { 0.15, on (1, 55, 90) }, { 0.2, on (1, 60, 90) }, { 0.25, on (1, 64, 90) },
                                                 { 1.0, juce::MidiMessage::controllerEvent (1, 64, 127) }, { 1.5, off (1, 48) }, { 1.5, off (1, 55) },
                                                 { 1.5, off (1, 60) }, { 1.5, off (1, 64) }, { 3.0, juce::MidiMessage::controllerEvent (1, 64, 0) } });
        check (rms (c, 48000, 2.0, 3.0) > 0.01, "sustain pedal holds the chord after the keys are released");
        check (rms (c, 48000, 4.7, 5.0) < rms (c, 48000, 2.0, 3.0) * 0.1, "pedal up releases it");
        writeWav (c, 48000, outDir.getChildFile ("02_chord_sustain.wav"));
    }

    // ---------------------------------------------------------------- pads and controllers
    std::printf ("\n== channel 10 pads, controllers, pitch bend, state ==\n");
    {
        auto p = make (48000, 256);
        // pad A1 alone (ch10 note 36): a modulation pad, no pipe
        auto a = render (*p, 48000, 256, 1.0, { { 0.1, on (10, 36, 100) }, { 0.8, off (10, 36) } });
        check (rms (a, 48000, 0.3, 0.8) < 1e-4, "pad A1 (ch10 note 36) does not play a pipe");
        // held c1, then pad A1 pressure 127 = wind x5
        juce::MidiMessage at = juce::MidiMessage::aftertouchChange (10, 36, 127);
        auto w = render (*p, 48000, 256, 3.0, { { 0.05, on (1, 60, 100) }, { 1.5, on (10, 36, 100) }, { 1.55, at }, { 2.9, off (1, 60) }, { 2.9, off (10, 36) } });
        const double r0 = rms (w, 48000, 0.9, 1.4), r1 = rms (w, 48000, 2.2, 2.8);
        check (r1 > r0 * 1.6, "pad A1 pressure raises the wind of the held pipe: " + juce::String (20 * std::log10 (r1 / r0), 1) + " dB");
        writeWav (w, 48000, outDir.getChildFile ("03_pad_wind_mod.wav"));
        // pad B1 (ch10 note 44): wind pipe C, pressure = wind. The pipe's own sound (reverb off): with the pitch free (v1.1)
        // the stronger wind also raises the C by ~8 cents, and at 65 Hz the room's modes (T60 5.7 s, ~0.4 Hz wide) alone
        // change the reverberant level by a few dB
        auto pd = make (48000, 256);
        if (auto* rv = findParam (*pd, "Reverb")) rv->setValueNotifyingHost (0.0f);
        auto pb = render (*pd, 48000, 256, 3.0, { { 0.05, on (10, 44, 80) }, { 0.06, juce::MidiMessage::aftertouchChange (10, 44, 40) },
                                                  { 1.2, juce::MidiMessage::aftertouchChange (10, 44, 120) }, { 2.5, off (10, 44) } });
        const double b0 = rms (pb, 48000, 0.6, 1.1), b1 = rms (pb, 48000, 1.8, 2.4);
        check (b0 > 1e-3 && b1 > b0, "pad B1 plays the wind pipe C, more pressure is louder (" + juce::String (20 * std::log10 (b1 / b0), 1) + " dB)");
        writeWav (pb, 48000, outDir.getChildFile ("04_pad_wind_pipe.wav"));

        // controllers -> parameters (applied at once, reported to the host on the plugin's timer)
        auto* cut = findParam (*p, "Cut-up");
        auto* wind = findParam (*p, "Wind pressure");
        auto* glide = findParam (*p, "Length glide");
        check (cut && wind && glide, "parameters found by name");
        if (cut && wind && glide)
        {
            render (*p, 48000, 256, 0.1, { { 0.01, juce::MidiMessage::controllerEvent (1, 74, 127) }, { 0.02, juce::MidiMessage::controllerEvent (1, 93, 0) },
                                            { 0.03, juce::MidiMessage::pitchWheel (1, 16383) } });
            pump (200);
            render (*p, 48000, 256, 0.05, {});
            check (std::abs (cut->getValue() - 1.0f) < 1e-3, "CC74 -> Cut-up at maximum (" + cut->getCurrentValueAsText() + ")");
            check (wind->getValue() < 1e-3, "CC93 (knob 5) -> Wind pressure at minimum (" + wind->getCurrentValueAsText() + ")");
            check (glide->getValue() > 0.99f, "pitch bend -> Length glide (" + glide->getCurrentValueAsText() + ")");
            render (*p, 48000, 256, 0.05, { { 0.01, juce::MidiMessage::controllerEvent (1, 83, 64) } });
            pump (200);
            auto* trem = findParam (*p, "Tremulant");
            check (trem && std::abs (trem->getValue() - 64 / 127.0f) < 0.01f, "CC83 (fader 2) -> Tremulant");
        }
        // state round trip
        juce::MemoryBlock st;
        p->getStateInformation (st);
        auto q = make (48000, 256);
        q->setStateInformation (st.getData(), (int) st.getSize());
        pump (100);
        render (*q, 48000, 256, 0.05, {});
        pump (100);
        int diffs = 0, compared = 0;
        for (int i = 0; i < p->getParameters().size(); ++i)
        {
            auto* pa = p->getParameters()[i];
            auto* b2 = q->getParameters()[i];
            if (pa->getName (64).startsWith ("MIDI CC")) continue;     // the wrapper's controller placeholders
            ++compared;
            if (std::abs (pa->getValue() - b2->getValue()) > 1e-5f)
            {
                if (diffs < 6) std::printf ("    %s: %s vs %s\n", pa->getName (64).toRawUTF8(), pa->getCurrentValueAsText().toRawUTF8(), b2->getCurrentValueAsText().toRawUTF8());
                ++diffs;
            }
        }
        check (diffs == 0, "state saved and restored into a new instance (" + juce::String ((int) st.getSize()) + " bytes, " + juce::String (compared) + " parameters compared)");
    }

    // ---------------------------------------------------------------- rates and block sizes
    std::printf ("\n== sample rates and block sizes ==\n");
    for (double sr : { 44100.0, 48000.0, 88200.0, 96000.0 })
        for (int bs : { 32, 441, 1024 })
            for (bool jitter : { false, true })
            {
                auto p = make (sr, bs);
                auto b = render (*p, sr, bs, 1.6, { { 0.0, on (1, 48, 100) }, { 0.01, on (1, 64, 100) }, { 0.02, on (1, 67, 100) },
                                                     { 0.5, juce::MidiMessage::controllerEvent (1, 77, 90) }, { 1.2, off (1, 48) } }, jitter);
                float pk = 0;
                const bool okb = finiteAndBounded (b, &pk);
                const double r = rms (b, sr, 0.8, 1.2);
                if (! okb || r < 0.005)
                    check (false, juce::String (sr / 1000, 1) + " kHz, block " + juce::String (bs) + (jitter ? " varying" : "") + ": rms " + juce::String (r, 4));
                else
                    std::printf ("  ok   %5.1f kHz  block %4d %s  rms %5.1f dBFS  peak %.2f\n", sr / 1000, bs, jitter ? "varying" : "fixed  ", 20 * std::log10 (r), pk);
            }

    // ---------------------------------------------------------------- pitch lock through the plugin
    std::printf ("\n== pitch lock 100 %% through the plugin (reverb off, sustained 1.4-2.4 s) ==\n");
    for (double sr : { 44100.0, 48000.0, 96000.0 })
    {
        auto p = make (sr, 512);
        if (auto* rv = findParam (*p, "Reverb")) rv->setValueNotifyingHost (0.0f);
        if (auto* lk = findParam (*p, "Pitch lock")) lk->setValueNotifyingHost (1.0f);      // (off by default since v1.1)
        double worst = 0;
        juce::String line;
        for (int note : { 36, 48, 60, 72, 84, 96 })
        {
            auto b = render (*p, sr, 512, 2.6, { { 0.0, on (1, note, 100) }, { 2.5, off (1, note) } });
            double f;
            const double c = pitchCents (b, sr, 1.4, 2.4, 440.0 * std::pow (2.0, (note - 69) / 12.0), &f);
            worst = juce::jmax (worst, std::abs (c));
            line << " " << note << ":" << juce::String (c, 2);
            render (*p, sr, 512, 1.5, {});      // let it die away
        }
        check (worst <= 2.0, juce::String (sr / 1000, 1) + " kHz: cents" + line + "  (worst " + juce::String (worst, 2) + ")");
    }
    {
        // wind 300 mmWS and helium 60 %: the servo still holds the key
        auto p = make (48000, 512);
        if (auto* rv = findParam (*p, "Reverb")) rv->setValueNotifyingHost (0.0f);
        if (auto* lk = findParam (*p, "Pitch lock")) lk->setValueNotifyingHost (1.0f);
        auto* wind = findParam (*p, "Wind pressure");
        auto* gas = findParam (*p, "Gas");
        // 200 mmWS = Wind pressure 3.92: above 1.00 the knob's last third is geometric up to 5.00 (Params.h kWindKnee)
        wind->setValueNotifyingHost ((float) (2.0 / 3.0 + (1.0 / 3.0) * std::log (200.0 * 9.80665 / 500.0) / std::log (5.0)));
        gas->setValueNotifyingHost (0.8f);                                                    // He 60 %
        double worst = 0;
        juce::String line;
        for (int note : { 48, 60, 72 })
        {
            auto b = render (*p, 48000, 512, 2.8, { { 0.0, on (1, note, 100) }, { 2.7, off (1, note) } });
            double f;
            const double c = pitchCents (b, 48000, 1.6, 2.6, 440.0 * std::pow (2.0, (note - 69) / 12.0), &f);
            worst = juce::jmax (worst, std::abs (c));
            line << " " << note << ":" << juce::String (c, 2);
            render (*p, 48000, 512, 1.5, {});
        }
        check (worst <= 2.0, "200 mmWS + He 60 %: cents" + line);
    }

    // ---------------------------------------------------------------- mallet mode through the host
    std::printf ("\n== mallet mode (host parameters: Excitation, Pipe metal, Mallet head ...) ==\n");
    {
        auto p = make (48000, 512);
        auto* ex = findParam (*p, "Excitation");
        auto* metal = findParam (*p, "Pipe metal");
        auto* head = findParam (*p, "Mallet head");
        check (ex && metal && head, "Excitation, Pipe metal, Mallet head are host parameters");
        if (ex && metal && head)
        {
            std::printf ("  Excitation: %s / %s (%d steps); Pipe metal: %s ... %s; Mallet head: %s ... %s\n",
                         ex->getText (0.0f, 32).toRawUTF8(), ex->getText (1.0f, 32).toRawUTF8(), ex->getNumSteps(),
                         metal->getText (0.0f, 32).toRawUTF8(), metal->getText (1.0f, 32).toRawUTF8(),
                         head->getText (0.0f, 32).toRawUTF8(), head->getText (1.0f, 32).toRawUTF8());
            if (auto* rv = findParam (*p, "Reverb")) rv->setValueNotifyingHost (0.0f);
            ex->setValueNotifyingHost (1.0f);
            pump (50);
            auto b = render (*p, 48000, 512, 3.0, { { 0.1, on (1, 60, 100) }, { 1.6, off (1, 60) } });
            float pk = 0;
            const bool fin = finiteAndBounded (b, &pk);
            const double early = rms (b, 48000, 0.1, 0.4), held = rms (b, 48000, 1.2, 1.6), damped = rms (b, 48000, 2.4, 3.0);
            std::printf ("  struck c1: peak %.3f, rms %.1f dBFS (0.1-0.4 s), %.1f dBFS (1.2-1.6 s), after key up %.1f dBFS\n", pk,
                         20 * std::log10 (early + 1e-12), 20 * std::log10 (held + 1e-12), 20 * std::log10 (damped + 1e-12));
            check (fin && pk > 0.01, "the struck pipe sounds, within full scale");
            check (damped < 0.1 * held, "key up: the felt damper stops the ringing");
            writeWav (b, 48000, outDir.getChildFile ("05_mallet_c1.wav"));
            // a chord on zinc with a brass head, then the same on common metal with rubber (the material is heard)
            metal->setValueNotifyingHost (3.0f / 4.0f);        // zinc (5 metals)
            head->setValueNotifyingHost (3.0f / 4.0f);         // brass (5 heads: v2.0 appended yarn)
            pump (50);
            auto z = render (*p, 48000, 512, 4.0, { { 0.1, on (1, 48, 110) }, { 0.1, on (1, 55, 110) }, { 0.1, on (1, 64, 110) }, { 0.1, on (1, 72, 110) } });
            metal->setValueNotifyingHost (0.0f);               // common metal
            head->setValueNotifyingHost (0.0f);                // hard rubber
            pump (50);
            render (*p, 48000, 512, 0.5, { { 0.0, juce::MidiMessage::allNotesOff (1) } });
            auto l = render (*p, 48000, 512, 4.0, { { 0.1, on (1, 48, 110) }, { 0.1, on (1, 55, 110) }, { 0.1, on (1, 64, 110) }, { 0.1, on (1, 72, 110) } });
            const double zt = rms (z, 48000, 2.5, 3.5), lt = rms (l, 48000, 2.5, 3.5);
            std::printf ("  chord at 2.5-3.5 s: zinc + brass %.1f dBFS, common metal + rubber %.1f dBFS\n", 20 * std::log10 (zt + 1e-12), 20 * std::log10 (lt + 1e-12));
            check (zt > 3.0 * lt, "zinc rings on, lead-rich metal is dead");
            writeWav (z, 48000, outDir.getChildFile ("06_mallet_zinc_brass_chord.wav"));
            writeWav (l, 48000, outDir.getChildFile ("07_mallet_common_rubber_chord.wav"));
            // v2.0: the yarn head (wool, F = K x^2.54) against hard rubber on spotted metal, c1: a softer, darker stroke
            // (brightness: rms of the first difference over rms, the first 0.3 s)
            metal->setValueNotifyingHost (1.0f / 4.0f);        // spotted metal (the default)
            auto stroke = [&] (float hv) {
                head->setValueNotifyingHost (hv);
                pump (50);
                render (*p, 48000, 512, 0.5, { { 0.0, juce::MidiMessage::allNotesOff (1) } });
                render (*p, 48000, 512, 1.0, {});
                return render (*p, 48000, 512, 1.5, { { 0.1, on (1, 60, 100) } });
            };
            auto bright = [] (const juce::AudioBuffer<float>& b) {
                const float* x = b.getReadPointer (0);
                double e = 0, d = 0;
                for (int i = (int) (0.1 * 48000) + 1; i < (int) (0.4 * 48000); ++i) { e += x[i] * x[i]; d += (x[i] - x[i - 1]) * (x[i] - x[i - 1]); }
                return std::sqrt (d / std::max (e, 1e-30));
            };
            auto ry = stroke (1.0f);
            const juce::String yName = head->getCurrentValueAsText();
            auto rr = stroke (0.0f);
            float pky = 0;
            const bool finY = finiteAndBounded (ry, &pky);
            const double by = bright (ry), br = bright (rr);
            std::printf ("  c1 struck: %s peak %.3f, brightness %.4f; hard rubber %.4f\n", yName.toRawUTF8(), pky, by, br);
            check (finY && pky > 1e-3 && yName == "Yarn", "the yarn head (the last of the list) sounds, within full scale");
            check (by < br, "yarn: a darker stroke than hard rubber");
            writeWav (ry, 48000, outDir.getChildFile ("07b_mallet_yarn_c1.wav"));
            ex->setValueNotifyingHost (0.0f);
        }
    }

    // ---------------------------------------------------------------- V2: the stops (the programs) and the compass
    std::printf ("\n== stops: the programs, the Gedackt 8', the compass MIDI 24-96 (reverb off) ==\n");
    {
        auto p = make (48000, 512);
        juce::String names;
        for (int i = 0; i < p->getNumPrograms(); ++i) names << (i ? ", " : "") << p->getProgramName (i);
        check (p->getNumPrograms() == 2, "2 programs, the stops: " + names);
        auto* stop = findParam (*p, "Stop");
        auto* morph = findParam (*p, "Open/Stopped");
        auto* ears = findParam (*p, "Ears");
        auto* arch = findParam (*p, "Arched cut-up");
        auto* wood = findParam (*p, "Pipe wood");
        auto* ex = findParam (*p, "Excitation");
        check (stop && morph && ears && arch && wood && ex, "Stop, Ears, Arched cut-up, Pipe wood are host parameters");
        auto reverbOff = [&] (juce::AudioPluginInstance& q) { if (auto* rv = findParam (q, "Reverb")) rv->setValueNotifyingHost (0.0f); pump (50); };
        auto toStop = [&] (juce::AudioPluginInstance& q, int s) { q.setCurrentProgram (s); pump (200); render (q, 48000, 512, 0.05, {}); pump (200); };
        if (stop && morph && ears && arch && wood && ex)
        {
            reverbOff (*p);
            // the compass on the Prinzipal: C, and c'''' sound, the keys beyond them nothing
            auto edge = [&] (int note) {
                auto b = render (*p, 48000, 512, 1.5, { { 0.0, on (1, note, 100) }, { 1.0, off (1, note) } });
                render (*p, 48000, 512, 2.0, {});
                return rms (b, 48000, 0.5, 1.0);
            };
            const double r23 = edge (23), r24 = edge (24), r96 = edge (96), r97 = edge (97);
            std::printf ("  Prinzipal: MIDI 23 %.1f, 24 %.1f, 96 %.1f, 97 %.1f dBFS\n", 20 * std::log10 (r23 + 1e-12), 20 * std::log10 (r24 + 1e-12),
                         20 * std::log10 (r96 + 1e-12), 20 * std::log10 (r97 + 1e-12));
            check (r24 > 1e-3 && r96 > 1e-3 && r23 < 1e-6 && r97 < 1e-6, "Prinzipal: MIDI 24 and 96 sound, 23 and 97 nothing");
            auto pr = render (*p, 48000, 512, 3.2, { { 0.0, on (1, 60, 100) }, { 3.0, off (1, 60) } });
            render (*p, 48000, 512, 2.0, {});

            // the Gedackt as built
            toStop (*p, 1);
            std::printf ("  program 1: Stop %s, Open/Stopped %s, Ears %s, Arched cut-up %s, Pipe wood %s\n", stop->getCurrentValueAsText().toRawUTF8(),
                         morph->getCurrentValueAsText().toRawUTF8(), ears->getCurrentValueAsText().toRawUTF8(), arch->getCurrentValueAsText().toRawUTF8(),
                         wood->getCurrentValueAsText().toRawUTF8());
            check (stop->getValue() > 0.99f && morph->getValue() > 0.99f && ears->getValue() < 0.01f && arch->getValue() < 0.01f && wood->getValue() < 0.01f,
                   "program 1 = the Gedackt: stopped 100 %, no ears, arched cut-up 0 %, white oak");
            reverbOff (*p);
            auto g = render (*p, 48000, 512, 3.2, { { 0.0, on (1, 60, 100) }, { 3.0, off (1, 60) } });
            double f = 0;
            const double cents = pitchCents (g, 48000, 2.0, 3.0, 440.0 * std::pow (2.0, -9.0 / 12.0), &f);
            const double h1 = partialDb (g, 48000, 2.0, 3.0, f), h2 = partialDb (g, 48000, 2.0, 3.0, 2 * f), h3 = partialDb (g, 48000, 2.0, 3.0, 3 * f);
            const double rg = rms (g, 48000, 2.0, 3.0), rp = rms (pr, 48000, 2.0, 3.0);
            std::printf ("  Gedackt c1: %.2f Hz (%+.2f cents, Pitch lock 0 %%), rms %.1f dBFS (Prinzipal %.1f); H2 %+.1f, H3 %+.1f dB re H1\n", f, cents,
                         20 * std::log10 (rg + 1e-12), 20 * std::log10 (rp + 1e-12), h2 - h1, h3 - h1);
            check (rg > 1e-3 && std::abs (cents) <= 5.0, "the Gedackt's c1 sounds on its key with the pitch free");
            check (h2 < h3 - 10.0 && h3 < h1, "a stopped pipe: the octave far below the twelfth, the fundamental strongest");
            writeWav (g, 48000, outDir.getChildFile ("08_gedackt_c1.wav"));
            const double g23 = edge (23), g24 = edge (24), g96 = edge (96), g97 = edge (97);
            std::printf ("  Gedackt: MIDI 23 %.1f, 24 %.1f, 96 %.1f, 97 %.1f dBFS\n", 20 * std::log10 (g23 + 1e-12), 20 * std::log10 (g24 + 1e-12),
                         20 * std::log10 (g96 + 1e-12), 20 * std::log10 (g97 + 1e-12));
            check (g24 > 1e-4 && g96 > 1e-4 && g23 < 1e-6 && g97 < 1e-6, "Gedackt: MIDI 24 and 96 sound, 23 and 97 nothing");

            // drawing the stopper: Open/Stopped toward open takes the pipe toward the octave
            morph->setValueNotifyingHost (0.0f);
            pump (50);
            auto o = render (*p, 48000, 512, 3.2, { { 0.0, on (1, 60, 100) }, { 3.0, off (1, 60) } });
            render (*p, 48000, 512, 2.0, {});
            double fo = 0;
            pitchCents (o, 48000, 2.0, 3.0, 261.63, &fo);
            std::printf ("  stopper drawn (Open/Stopped 0 %%): %.2f Hz = x%.3f\n", fo, fo / f);
            check (fo / f > 1.75 && fo / f < 2.05, "the stopper drawn: the pipe sounds near the octave (a little flat, an open end of that body)");
            morph->setValueNotifyingHost (1.0f);
            pump (50);

            // a held key goes over to the new stop
            toStop (*p, 0);
            reverbOff (*p);
            auto h = render (*p, 48000, 512, 1.5, { { 0.0, on (1, 60, 100) } });
            toStop (*p, 1);
            reverbOff (*p);
            auto h2b = render (*p, 48000, 512, 3.0, { { 2.8, off (1, 60) } });
            render (*p, 48000, 512, 2.0, {});
            double fh = 0;
            pitchCents (h2b, 48000, 1.5, 2.5, 261.63, &fh);
            const double hh2 = partialDb (h2b, 48000, 1.5, 2.5, 2 * fh) - partialDb (h2b, 48000, 1.5, 2.5, fh);
            std::printf ("  c1 held from the Prinzipal (%.1f dBFS) over the change: %.1f dBFS, H2 %+.1f dB re H1\n", 20 * std::log10 (rms (h, 48000, 1.0, 1.5) + 1e-12),
                         20 * std::log10 (rms (h2b, 48000, 1.5, 2.5) + 1e-12), hh2);
            check (rms (h2b, 48000, 1.5, 2.5) > 1e-3 && hh2 < -20.0, "a key held over a stop change sounds on the new stop (the Gedackt's weak octave)");

            // the struck wooden pipe
            ex->setValueNotifyingHost (1.0f);
            pump (50);
            auto m = render (*p, 48000, 512, 3.0, { { 0.1, on (1, 48, 110) }, { 1.6, off (1, 48) } });
            wood->setValueNotifyingHost (2.0f / 3.0f);          // Sitka spruce (4 woods)
            pump (50);
            auto ms = render (*p, 48000, 512, 3.0, { { 0.1, on (1, 48, 110) }, { 1.6, off (1, 48) } });
            float pk = 0, pks = 0;
            const bool fin = finiteAndBounded (m, &pk) && finiteAndBounded (ms, &pks);
            double dif = 0, ref = 0;
            for (int i = 0; i < m.getNumSamples(); ++i) { const double d0 = m.getSample (0, i) - ms.getSample (0, i); dif += d0 * d0; ref += (double) m.getSample (0, i) * m.getSample (0, i); }
            std::printf ("  struck c: oak peak %.3f, %.1f dBFS (0.1-0.4 s), after key up %.1f; spruce peak %.3f, difference %.1f dB re oak\n", pk,
                         20 * std::log10 (rms (m, 48000, 0.1, 0.4) + 1e-12), 20 * std::log10 (rms (m, 48000, 2.4, 3.0) + 1e-12), pks,
                         10 * std::log10 (dif / juce::jmax (ref, 1e-20)));
            check (fin && pk > 0.01 && rms (m, 48000, 2.4, 3.0) < 0.1 * rms (m, 48000, 1.2, 1.6), "the struck Gedackt sounds and the damper stops it");
            check (dif > 0.01 * ref, "the wood is heard (oak and spruce differ)");
            writeWav (m, 48000, outDir.getChildFile ("09_gedackt_mallet_oak.wav"));
            wood->setValueNotifyingHost (0.0f);
            ex->setValueNotifyingHost (0.0f);
            pump (50);
        }
        // the stop is saved with the project
        juce::MemoryBlock st;
        p->getStateInformation (st);
        auto q = make (48000, 512);
        q->setStateInformation (st.getData(), (int) st.getSize());
        pump (100);
        render (*q, 48000, 512, 0.05, {});
        pump (100);
        auto* qs = findParam (*q, "Stop");
        auto* qm = findParam (*q, "Open/Stopped");
        check (qs && qm && qs->getValue() > 0.99f && qm->getValue() > 0.99f, "the stop is saved and restored (" + (qs ? qs->getCurrentValueAsText() : juce::String()) + ")");
    }

    // ---------------------------------------------------------------- CPU
    std::printf ("\n== CPU (48 kHz, 256-sample blocks, this machine, one core) ==\n");
    for (int stopNo : { 0, 1 })
        for (int voices : { 1, 8, 16, 32 })
        {
            auto p = make (48000, 256);
            if (stopNo > 0) { p->setCurrentProgram (stopNo); pump (200); render (*p, 48000, 256, 0.05, {}); pump (200); }
            std::vector<Ev> evs;
            for (int i = 0; i < voices; ++i) evs.push_back ({ 0.001 * i, on (1, 36 + (i * 7) % 60, 100) });
            render (*p, 48000, 256, 1.0, evs);
            double cpu = 0;
            render (*p, 48000, 256, 4.0, {}, false, &cpu);
            std::printf ("  %s %2d voices: %5.1f %% of real time\n", stopNo ? "Gedackt  " : "Prinzipal", voices, cpu * 100);
        }

    // ---------------------------------------------------------------- the screen in a host
    if (withEditor)
    {
        std::printf ("\n== the screen: the plugin editor in a host window, real-time audio thread (48 kHz, 480) ==\n");
        auto p = make (48000, 480);
        if (! p) return 1;
        std::unique_ptr<juce::AudioProcessorEditor> ed (p->createEditorIfNeeded());
        check (ed != nullptr, "editor created");
        if (ed)
        {
            RealtimeAudio audio (*p, 48000, 480);
            audio.startThread (juce::Thread::Priority::highest);
            {
                juce::DocumentWindow win ("OkumuLab 1 - host test", juce::Colours::black, 0);
                win.setUsingNativeTitleBar (true);
                win.setContentNonOwned (ed.get(), true);
                win.centreWithSize (win.getWidth(), win.getHeight());
                win.setVisible (true);
                win.toFront (false);
                // the session: settles, chord at 2 s, knob sweeps 4-10 s, pad 7-8.5 s, release 10.5 s, a row's RESET button
                // 11-12.6 s, the stop menu 13.4-17 s, report 17.8 s after the page is ready (a slow page: when it has answered)
                const auto t0 = juce::Time::getMillisecondCounterHiRes();
                while (! editorReport.existsAsFile() && juce::Time::getMillisecondCounterHiRes() - t0 < 30000) pump (200);
                pump (200);
                win.clearContentComponent();
            }
            audio.stopThread (2000);
            std::printf ("  audio thread: %d blocks\n", audio.blocks.load());
            check (! audio.bad, "output finite and within full scale while the screen runs");
            const auto r = juce::JSON::parse (editorReport);
            check (r.isObject(), "the web screen loaded, played the session and reported (" + editorReport.getFileName() + ")");
            if (r.isObject())
            {
                const auto f = r["fps"], a = r["audio"], s = r["screen"];
                std::printf ("  screen %dx%d dpr %.2f: %.1f fps mean, %.1f min over %d s, worst frame %.1f ms, %d frames > 25 ms\n",
                             (int) s["w"], (int) s["h"], (double) s["dpr"], (double) f["mean"], (double) f["min"], (int) f["seconds"],
                             (double) f["worst_frame_ms"], (int) f["frames_over_25ms"]);
                if (auto* ps = f["per_second"].getArray())
                    for (const auto& line : *ps) std::printf ("    %s\n", line.toString().toRawUTF8());
                std::printf ("  plugin -> page: %.2f ms per frame on the message thread (max %.2f)\n", (double) r["cpp_send_ms_avg"], (double) r["cpp_send_ms_max"]);
                // the frame rate is a measurement (printed above; it depends on what else the machine is doing:
                // with little free memory it falls to a few fps); the check is only that the screen draws
                check ((int) f["seconds"] >= 6 && (double) f["mean"] >= 1.0, "the screen draws while notes sound and knobs move");
                check ((int) a["late_callbacks"] == 0, "no late audio callbacks (max gap " + juce::String ((double) a["max_gap_blocks"], 2) + " blocks)");
                check ((int) a["overruns"] == 0, "no audio overruns while the screen runs (" + a["callbacks"].toString() + " callbacks, max load "
                                                     + juce::String ((double) a["max_load"], 2) + ")");
                // v1.1.1: the session pressed Cut-up's RESET button on the page after moving Cut-up and Labium offset
                {
                    const auto rb = r["reset_button"], before = rb["before"], after = rb["after"];
                    const double cut = rb["cutup"], y0 = rb["y0b"];
                    std::printf ("  row RESET buttons: %d of %d rows; Cut-up's pressed: cutup %.3f (default 1), y0b %.3f (left at 1.00); "
                                 "lit before %d/%d, after %d/%d (cutup/y0b)\n",
                                 (int) before["buttons"], (int) before["rows"], cut, y0, (int) (bool) before["cutupLit"], (int) (bool) before["y0bLit"],
                                 (int) (bool) after["cutupLit"], (int) (bool) after["y0bLit"]);
                    check ((int) before["rows"] > 30 && (int) before["buttons"] == (int) before["rows"], "every parameter row has its RESET button");
                    check ((bool) before["cutupLit"] && (bool) before["y0bLit"], "the buttons of the moved parameters light up");
                    check (std::abs (cut - 1.0) < 1e-6 && std::abs ((double) after["cutupShown"] - 1.0) < 1e-6,
                           "Cut-up's RESET button returns Cut-up to its default (plugin and page)");
                    check (std::abs (y0 - 1.0) < 1e-6, "the other parameters stay where they were");
                    check (! (bool) after["cutupLit"] && (bool) after["y0bLit"], "only the button of the parameter still off its default stays lit");
                }
                // V2: the page chose the Gedackt in the stop menu (where the experiment recipes were), played c1 and the keys 23
                // and 97 beyond the compass, moved Open/Stopped to 40 % and pressed its RESET button
                {
                    const auto sm = r["stop_menu"], pb = sm["pageBefore"], pg = sm["page"], mv = sm["moved"], ar = sm["afterReset"];
                    std::printf ("  stop menu: %d options, \"%s\" -> \"%s\"; plugin: stop %d, program %d (%s), Open/Stopped %.2f, ears %.2f, arch %.2f, "
                                 "wall loss %.3f, scale %.2f HT, mouth %.4f, wood %d\n",
                                 (int) pb["options"], pb["label"].toString().toRawUTF8(), pg["label"].toString().toRawUTF8(), (int) (double) sm["stop"],
                                 (int) sm["program"], sm["programName"].toString().toRawUTF8(), (double) sm["morph"], (double) sm["ears"], (double) sm["arch"],
                                 (double) sm["loss"], (double) sm["scaleHT"], (double) sm["mouthFrac"], (int) (double) sm["wood"]);
                    std::printf ("  page: rows %d, wood row %d, metal row %d, ears %d, arch %d, Open/Stopped \"%s\"; keys 60, 23, 97: %d voice(s), focus MIDI %d "
                                 "stop %d, depth %.1f mm; moved %.2f (lit %d), after RESET %.2f (plugin %.2f, lit %d)\n",
                                 (int) pg["rows"], (int) (bool) pg["woodShown"], (int) (bool) pg["metalShown"], (int) (bool) pg["earsShown"],
                                 (int) (bool) pg["archShown"], pg["morphText"].toString().toRawUTF8(), (int) sm["voices"], (int) sm["focusMidi"],
                                 (int) sm["focusStop"], 1000.0 * (double) sm["focusDepth"], (double) mv["morph"], (int) (bool) mv["morphLit"],
                                 (double) ar["morph"], (double) sm["morphAfterReset"], (int) (bool) ar["morphLit"]);
                    check ((int) pb["options"] == 2 && (int) (double) sm["stop"] == 1 && (int) sm["program"] == 1 && pg["sel"].toString() == "1"
                               && (int) (double) pg["stop"] == 1,
                           "the stop menu holds the 2 stops; choosing the Gedackt sets the stop and the program (plugin and page)");
                    check (std::abs ((double) sm["morph"] - 1.0) < 1e-6 && std::abs ((double) sm["ears"]) < 1e-6 && std::abs ((double) sm["arch"]) < 1e-6
                               && std::abs ((double) sm["loss"] - 1.27) < 1e-4 && (double) sm["scaleHT"] < -9.0 && (double) sm["scaleHT"] > -10.0
                               && (int) (double) sm["wood"] == 0 && ! (bool) pg["morphLit"],
                           "every knob goes to the Gedackt as built (stopped, no ears, arch 0 %, Dickson's scale -9.4 HT, its wall loss, oak), and that is its default");
                    check ((bool) pg["woodShown"] && ! (bool) pg["metalShown"] && (bool) pg["earsShown"] && (bool) pg["archShown"],
                           "the panels show the wood instead of the metal");
                    check ((int) sm["voices"] == 1 && (int) sm["focusMidi"] == 60 && (int) sm["focusStop"] == 1 && (double) sm["focusDepth"] > 0.0,
                           "a key plays a Gedackt pipe, the keys beyond MIDI 24-96 nothing");
                    check (std::abs ((double) mv["morph"] - 0.4) < 1e-3 && (bool) mv["morphLit"] && std::abs ((double) sm["morphAfterReset"] - 1.0) < 1e-6
                               && std::abs ((double) ar["morph"] - 1.0) < 1e-6 && ! (bool) ar["morphLit"],
                           "Open/Stopped's RESET button returns the Gedackt to stopped 100 % (its own default)");
                }
                int errors = 0;
                if (auto* js = r["js_messages"].getArray())
                    for (const auto& m : *js)
                        if (m.toString().startsWith ("JS ")) { ++errors; std::printf ("    %s\n", m.toString().toRawUTF8()); }
                check (errors == 0, "no script errors in the page");
            }
        }
        ed.reset();
    }

    std::printf ("\n%s (%d failed)\nrenders: %s\n", g_fail ? "FAILED" : "OK", g_fail, outDir.getFullPathName().toRawUTF8());
    return g_fail ? 1 : 0;
}
