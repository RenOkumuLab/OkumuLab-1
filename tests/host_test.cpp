/*
 * OkumuLab 1 — host test: loads the built VST3 the way a DAW does and plays it
 *
 *   okl_hosttest [path/to/OkumuLab 1.vst3] [--no-editor]
 *
 * Checks: plugin description, sound / silence after release, channel-10 pads,
 * controllers -> host parameters, pitch bend, state save/restore, sample rates
 * and block sizes (incl. odd and varying), pitch lock through the plugin,
 * CPU load with 16 / 32 voices, and the screen: the plugin's WebView2 editor
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
#include <xmmintrin.h>

namespace
{
int g_fail = 0;
int g_hostRounding = 0;     // --render: the host thread's floating-point rounding mode while it calls the plugin (OKL_HOST_ROUNDING)
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
        const unsigned csr = _mm_getcsr();
        if (g_hostRounding > 0) _mm_setcsr ((csr & ~0x6000u) | ((unsigned) (g_hostRounding & 3) << 13));     // only while the plugin runs
        p.processBlock (blk, mb);
        _mm_setcsr (csr);
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
        _mm_setcsr ((_mm_getcsr() & ~0x6000u) | ((unsigned) ra << 13));
        std::printf ("whole plugin on a thread with rounding mode %d\n", ra);
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

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    if (argc >= 4 && (juce::String (argv[1]) == "--render" || juce::String (argv[1]) == "--render-editor"))
        return renderLog (argv[2], argv[3], argc >= 5 ? juce::String (argv[4]) : juce::String (OKL_VST3_PATH), juce::String (argv[1]) == "--render-editor");
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
        // pad B1 (ch10 note 44): wind pipe C, pressure = wind
        auto pb = render (*p, 48000, 256, 3.0, { { 0.05, on (10, 44, 80) }, { 0.06, juce::MidiMessage::aftertouchChange (10, 44, 40) },
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
            auto* a = p->getParameters()[i];
            auto* b2 = q->getParameters()[i];
            if (a->getName (64).startsWith ("MIDI CC")) continue;      // the wrapper's controller placeholders
            ++compared;
            if (std::abs (a->getValue() - b2->getValue()) > 1e-5f)
            {
                if (diffs < 6) std::printf ("    %s: %s vs %s\n", a->getName (64).toRawUTF8(), a->getCurrentValueAsText().toRawUTF8(), b2->getCurrentValueAsText().toRawUTF8());
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
            head->setValueNotifyingHost (1.0f);                // brass (4 heads)
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
            ex->setValueNotifyingHost (0.0f);
        }
    }

    // ---------------------------------------------------------------- CPU
    std::printf ("\n== CPU (48 kHz, 256-sample blocks, this machine, one core) ==\n");
    for (int voices : { 1, 8, 16, 32 })
    {
        auto p = make (48000, 256);
        std::vector<Ev> evs;
        for (int i = 0; i < voices; ++i) evs.push_back ({ 0.001 * i, on (1, 36 + (i * 7) % 60, 100) });
        render (*p, 48000, 256, 1.0, evs);
        double cpu = 0;
        render (*p, 48000, 256, 4.0, {}, false, &cpu);
        std::printf ("  %2d voices: %5.1f %% of real time\n", voices, cpu * 100);
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
                // the session: settles, chord at 2 s, knob sweeps 4-10 s, pad 7-8.5 s, release 10.5 s, report 13 s after the page is ready
                const auto t0 = juce::Time::getMillisecondCounterHiRes();
                while (! editorReport.existsAsFile() && juce::Time::getMillisecondCounterHiRes() - t0 < 30000) pump (200);
                pump (200);
                win.clearContentComponent();
            }
            audio.stopThread (2000);
            std::printf ("  audio thread: %d blocks\n", audio.blocks.load());
            check (! audio.bad, "output finite and within full scale while the screen runs");
            const auto r = juce::JSON::parse (editorReport);
            check (r.isObject(), "the WebView2 screen loaded, played the session and reported (" + editorReport.getFileName() + ")");
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
