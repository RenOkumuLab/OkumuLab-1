
# OkumuLab 1

OkumuLab 1 is a highly experimental, innovative and avant-garde virtual organ VST3 plugin.

It is a physical model of organ flue pipes (two stops: Prinzipal 8' and Gedackt 8'), made with JUCE 8 as a VST3 instrument and a standalone app for Windows, macOS and Linux (on macOS also as an Audio Unit). The screen uses the system's web view (WebView2 on Windows, WKWebView on macOS, WebKitGTK on Linux) and Three.js, and draws the jet, the standing wave inside the pipe and the vibration of the pipe wall exactly as the sound engine computes them.

- Version 2.0.0, vendor name OkumuLab
- Formats: VST3 and standalone (Windows x64, Linux x86-64); VST3, Audio Unit and standalone (macOS 10.15 or later, Apple silicon and Intel)
<img width="2560" height="1600" alt="okumulab1_prinzipal_wind" src="https://github.com/user-attachments/assets/d6fc1eea-95d9-4b10-a23c-8abed95e8232" />

<img width="2560" height="1600" alt="okumulab1_gedackt_wind" src="https://github.com/user-attachments/assets/020c1eba-15c6-4e85-bdb6-82def8e53e87" />
<img width="2560" height="1600" alt="okumulab1_gedackt_tuning" src="https://github.com/user-attachments/assets/385a5d9b-fc4e-4ce5-b4c9-6466f8826e86" />

## Features

- Two stops, chosen in the menu at the top (they are also the DAW's programs): the Prinzipal 8' (round organ metal, open) and the Gedackt 8' (wood, stopped).
- In WIND mode the pipe sounds from the wind blown into it. In MALLET mode you strike the pipe body with a mallet: the metal Prinzipal rings as a thin cylindrical shell, the wooden Gedackt as the boards of a box.
- The keyboard covers MIDI 24 to 96 (C, to c⁴), and each key plays a pipe with its own dimensions. Other notes sound nothing.
- The organ plays in equal temperament at a¹ = 440 Hz unless you choose one of six historical temperaments, another a¹ (380 to 480 Hz) or a Scala scale. The TUNING button in the top bar, beside the stop menu, shows the tuning in use and opens the panel where it is set.
- A modulation matrix with 4 slots.
- The reverb convolves the sound with an impulse response computed for the nave of a church.
- Every knob is a DAW parameter. Right-click a knob for MIDI learn. To return one knob to its default value, press the RESET button (↺) at the right of its row, or double-click the knob; the RESET button in the top bar returns them all. The default mapping matches the knobs, faders and pads of the Arturia MiniLab 3.

The details of the physical model, the MIDI mapping and the test results are in [docs/DEVELOPMENT_NOTES_ja.md](docs/DEVELOPMENT_NOTES_ja.md) (in Japanese).

## What's new in 2.0.0

- **A second stop: the Gedackt 8'.** A stopped wooden pipe built from the literature. The scale is Dickson's English Stopped Diapason (1882: C 82.5 × 101.6 mm inside, tenor C 54.0 × 66.7 mm, the section halving every 19.7 half-tones), and the mouth is as wide as the inside. The cut-up follows Janke's rule for a Gedackt (5 half-tones above the norm cut-up at the house-organ wind pressure the plugin plays at). From g up it was then voiced by ear, key by key: about x1.46 from g to f#¹, x1.58-1.61 from g¹ to f#², x1.53-1.61 from g² to g³ and x1.41-1.46 from g#³ to c⁴. The flue is as wide as on real wooden pipes (1 mm at 47 mm inside width and 0.6 mm at 9 mm, from the BDO's pipe and Fischer et al.'s Schuke stopped pipe; 1.19 mm at C). The mouth adds Fletcher & Rossing's end correction of a high mouth and the air in the mouth cut through the front board. The wall loss is that of a rectangular bore, with the excess measured on wooden pipes (Rucz et al. 2023). A stopper tunes each pipe, as organ builders tune a Gedackt. The upper lip sits on the jet's centre plane (Labium offset 0 b; Fletcher & Douglas 1980), so the tone has the odd partials of a stopped pipe. The only even partials come from the Bernoulli term of the jet drive (Fletcher 1976) and lie about 70 dB below the fundamental. The twelfth is about 14 dB below the fundamental at c¹ and 27 dB below it at c². An arched cut-up makes the tone rounder, and moving the Labium offset brings even partials in.
- **Open/Stopped draws the stopper.** On the Gedackt the knob starts at Stopped (100 %); turning it toward Open draws the leather-sealed stopper out of the top, on the screen too, and the pipe rises toward the octave (a little flat, as an open end of that body is).
- **Side ears and an arched cut-up.** Two new knobs in VOICING. The ears' air plug lengthens the mouth's end correction (the pipe a few cents flatter). Under an arched (semicircular) upper lip the jet crosses the mouth in strips of different lengths, which rounds the tone. Both start at 0 % on both stops (no ears, a straight cut-up).
- **The struck wooden pipe.** In MALLET mode the Gedackt's front board is an orthotropic plate of the chosen wood (white oak by default; also red pine, Sitka spruce, western red cedar, from the Wood Handbook).
- **A yarn mallet.** Mallet head adds Yarn (wool wound on a rubber core), for both stops. Wool does not follow Hertz's law: it is a power law F = K x^p, here the felt mallet of Bilbao, Torin & Chatziioannou (2015, after Rhaouti, Chaigne & Joly 1999), K = 1.6e8 N/m^2.54, p = 2.54. It stays on the wall several times longer than the other heads, and the stroke is soft and dark. It is the last in the list, so saved settings keep their heads.
- **Tuning.** The TUNING button in the top bar, beside the stop menu, shows the temperament and a¹ in use and opens the TUNING panel. It lights amber while the organ is not in equal temperament at 440 Hz. The panel has two new knobs: Temperament (equal, Pythagorean, 1/4-comma meantone, Werckmeister III, Kirnberger III, Vallotti, Young, or Custom) and Pitch a¹ (380–480 Hz). The historical temperaments are computed from their circles of fifths. The panel also loads a Scala scale (.scl) and a keyboard mapping (.kbm), read with the Surge Synth Team's tuning-library (MIT), and Custom plays them. A .scl or .kbm file dropped anywhere on the screen is loaded too, and the files are saved in the project. A tuned key sounds the pipe of the nearest equal-tempered key, its body cut to the tuned length, as an organ builder tunes it. Equal temperament at 440 Hz changes nothing, and choosing another stop keeps the tuning.
- **The stop menu takes the place of the experiment recipes.** Choosing a stop sets every knob to that stop as built. The RESET buttons return to the defaults of the stop in use (Open/Stopped, scale, mouth and wall loss differ).
- **MIDI 24 to 96** on both stops. The Prinzipal's blown sound in that range is the same as in 1.1.x.
- **Struck walls without a cut-off.** In MALLET mode the wall's modes used to be taken only below 12 / (the contact time at 1 m/s), a cut made for speed. With a soft head the wall then fell silent from one key to the next (the yarn Gedackt dropped 23 dB between c¹ and d¹). Every mode below 20 kHz is now a candidate, and the contact force decides how much each one rings. While the mallet pushes, a mode radiates by its acceleration, so a mode pushed far below its resonance no longer radiates the force itself. The Prinzipal below c¹ sounds as before. Above c¹, and on the Gedackt everywhere, the struck sound changed (the yarn Gedackt is about 12 dB softer in the bass).
- The Gedackt's lowest key, C, (32.7 Hz), is at the edge of speech at the reference wind: it fades slowly. Cut-up ×0.90 (or more wind) makes it steady.
- With the cut-up arched, a Gedackt pipe takes about twice the CPU of a Prinzipal pipe (the jet in strips). On the laptop it was developed on (Ryzen 5 3500U), 16 such pipes take about 60 % of one core and 32 more than one core. At the default 0 % it is as light as the Prinzipal.


## What's new in 1.1.0

- **The tone comes from the physics alone.** Version 1.0.0 shaped its tone with adjustments made to hide a buzz (the buzz itself was a bug with the host's block size, fixed in 1.0.0). They are gone. Instead the jet now spreads on its way across the mouth as a turbulent plane jet (Thwaites & Fletcher 1983), its waves grow at a rate set by its local width (Yoshikawa 1998), and the open top and the mouth radiate as unflanged pipe ends (Norris & Sheng 1989). Without these, the plain model sounded like a gamba (flat upper partials, a weak octave). With them the spectrum has the shape measured on real principals: the fundamental strongest, 5 to 6 partials within 30 dB, the octave stronger in the bass. There is no filter or equalizer after the physics. The octave is weaker than in 1.0.0; for a stronger one, raise Labium offset (+1.0 b gives −12 dB at c').
- **Pitch lock now starts at 0 %**, so the pitch follows the wind, the temperature and the gas as a real pipe does. Set it to 100 % to hold every pipe on its key. A weak wind lowers the pitch, and below about 0.3 times the normal wind the pipe falls silent.
- The Labium offset knob now counts in the jet's half-width where it meets the labium. The jet spreads to several times its width at the flue, so beyond about ±1.2 b the pipe stays silent, and settings saved with 1.0.0 put the lip at another position.
- The output is 2 to 7 dB louder than in 1.0.0 for the same playing (about 7 dB in the middle of the compass, 2 dB at the top), because the spreading jet draws in the surrounding air; the output gain is unchanged. For the loudness of 1.0.0, set Output to about −6 dB. Big chords reach the output limiter (−1 dBFS).
- Nicking now only thickens the jet and makes the tone rounder.
- macOS and Linux versions.

## Requirements

- Windows (x64). Tested on Windows 11 in Bitwig Studio and SAVIHost. The screen uses the WebView2 runtime, which comes with Windows 11; without it, the plugin opens a simple native screen.
- macOS: macOS 10.15 or later, Apple silicon or Intel.
- Linux: x86-64 with glibc 2.35 or later (Ubuntu 22.04, Debian 12, Fedora 36 or later), X11 or XWayland and ALSA (JACK too for the standalone app). The screen uses WebKitGTK 4.1 (`libwebkit2gtk-4.1-0`); without it, the plugin opens a simple native screen.
- On x86-64 CPUs with AVX2 the engine uses AVX2 code, and on other x86-64 CPUs SSE2 code. On Apple silicon it uses plain C++ that the compiler vectorizes.

The macOS and Linux versions are built and checked on GitHub's machines (the sound engine's tests, pluginval and auval) but have not been played on real Macs or Linux PCs yet. Reports are welcome.

## Installation

Download the package for your system from Releases on GitHub and extract it. The `README.txt` in each package has the details.

- Windows: copy the `OkumuLab 1.vst3` folder to `C:\Program Files\Common Files\VST3\` (this needs administrator rights), then rescan the plugins in your DAW. For the standalone app, start `OkumuLab 1.exe` and choose the audio output and the MIDI input under Options at the top left.
- macOS: copy `OkumuLab 1.vst3` to `~/Library/Audio/Plug-Ins/VST3/` and `OkumuLab 1.component` to `~/Library/Audio/Plug-Ins/Components/`, and the app to Applications. The files are not signed with an Apple Developer ID, so run `xattr -dr com.apple.quarantine "OkumuLab 1.vst3" "OkumuLab 1.component" "OkumuLab 1.app"` once in the folder where you put them.
- Linux: copy the `OkumuLab 1.vst3` folder to `~/.vst3/` and rescan the plugins in your DAW. The standalone app is `OkumuLab 1`.

The Windows binaries are not code-signed, so Windows SmartScreen may show a warning the first time you open them. On Windows only one application at a time can open a MIDI device, so do not run the standalone app and a DAW at the same time.

## Building

Windows: Visual Studio 2022 or later (the Build Tools are enough) with "Desktop development with C++", including "C++ CMake tools for Windows". CMake and Ninja come with Visual Studio.

```
build.bat          VST3, standalone and test programs (Release)
build.bat dsp      sound engine (DSP) and labium_check only (no JUCE)
```

macOS (Xcode's command line tools, CMake 3.22 or later) and Linux (CMake, a C++20 compiler and the packages below):

```
bash build.sh      VST3, standalone, Audio Unit on macOS, and test programs (Release)
bash build.sh dsp  sound engine (DSP) and labium_check only (no JUCE)
```

On Ubuntu or Debian the packages are `ninja-build pkg-config libasound2-dev libjack-jackd2-dev libfreetype-dev libfontconfig1-dev libcurl4-openssl-dev libx11-dev libxcomposite-dev libxcursor-dev libxext-dev libxinerama-dev libxrandr-dev libxrender-dev libgtk-3-dev libwebkit2gtk-4.1-dev libglu1-mesa-dev mesa-common-dev`.

- Output: `build/OkumuLab1_artefacts/Release/` (`VST3/`, `Standalone/`, and `AU/` on macOS)
- JUCE has long file names, so on Windows put the folder on a short path such as `C:\dev\OkumuLab1` before building.
- `third_party/JUCE` is JUCE 8.0.12 without changes (the bundled DemoRunner.exe and Projucer.exe are left out). `third_party/nuget` is the unpacked NuGet package of the WebView2 SDK 1.0.3485.44 (Windows only).
- On Windows the C runtime is linked statically, so users do not need the Visual C++ Redistributable.
- `packaging/make_windows_zip.ps1` makes the Windows package from the build. On GitHub, `.github/workflows/build.yml` builds, checks and packages the macOS and Linux versions on every push to main, and a tag `v<version>` also drafts a release with them.

### Tests

- `build-dsp/labium_check [section]`: tests of the sound engine. The sections are port, derive, midi, pitchlock, wind, blocks, tone, cpu, stress, mallet, room, lab, simd, gedackt and tuning. Without a section, all of them run. On Linux run it with a larger stack (`ulimit -s unlimited`). `labium_check kcalgen` prints the Gedackt's tuning table (`src/dsp/KcalTableGedackt.inc`).
- `build/okl_hosttest_artefacts/Release/okl_hosttest`: loads the built VST3 the way a DAW does and plays it, including tests with the plugin window open. `--state-save <older.vst3> <file>` and then `--state-load <file>` open a project saved with an older build in this one.
- To validate the plugin with pluginval (Tracktion), get pluginval separately. It is not included in this repository.

### Environment variables for testing

These are not needed for normal use.

- `OKL_NATIVE_UI=1`: use the native screen instead of the web screen
- `OKL_MUTE=1`: no sound output (for screen tests)
- `OKL_PERFTEST=<file>`: run the screen test automatically and write the results to that file
- `OKL_RENDERTEST=<recorded notes>|<WAV>`: when the plugin is created, play the recorded notes and write the result to a WAV file

## License

Copyright (C) 2026 Ren Okumura

This program is free software. You can redistribute and modify it under the terms of the GNU Affero General Public License version 3 (AGPLv3). The full text is in [LICENSE](LICENSE). It comes with no warranty.

JUCE 8 is used under the AGPLv3. The licenses of the other libraries (including the VST3 SDK and the Audio Unit SDK bundled with JUCE), Three.js and the fonts are in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and [licenses/](licenses/).

VST is a trademark of Steinberg Media Technologies GmbH. Audio Units and macOS are trademarks of Apple Inc.
