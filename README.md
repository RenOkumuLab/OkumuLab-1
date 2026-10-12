
# OkumuLab 1

OkumuLab 1 is a highly experimental, innovative and avant-garde virtual organ VST3 plugin.

It is a physical model of an organ flue pipe (Prinzipal 8'), made with JUCE 8 as a VST3 instrument and a standalone app for Windows, macOS and Linux (on macOS also as an Audio Unit). The screen uses the system's web view (WebView2 on Windows, WKWebView on macOS, WebKitGTK on Linux) and Three.js, and draws the jet, the standing wave inside the pipe and the vibration of the pipe wall exactly as the sound engine computes them.

- Version 1.1.0, vendor name OkumuLab
- Formats: VST3 and standalone (Windows x64, Linux x86-64); VST3, Audio Unit and standalone (macOS 10.15 or later, Apple silicon and Intel)
<img width="1266" height="792" alt="スクリーンショット 2026-10-08 154201" src="https://github.com/user-attachments/assets/d39dea1d-3336-4553-88fe-ee4d444414aa" />

## Features

- In WIND mode the pipe sounds from the wind blown into it. In MALLET mode you strike the pipe body with a mallet.
- The keyboard covers the full MIDI range from 0 to 127, and each key plays a pipe with its own dimensions.
- There are 19 experimental recipes (DAW programs) and a modulation matrix with 4 slots.
- The reverb convolves the sound with an impulse response computed for the nave of a church.
- Every knob is a DAW parameter. Right-click a knob for MIDI learn, and double-click it to return to the default value. The default mapping matches the knobs, faders and pads of the Arturia MiniLab 3.

The details of the physical model, the MIDI mapping and the test results are in [docs/DEVELOPMENT_NOTES_ja.md](docs/DEVELOPMENT_NOTES_ja.md) (in Japanese).

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

- `build-dsp/labium_check [section]`: tests of the sound engine. The sections are port, derive, midi, pitchlock, wind, blocks, tone, cpu, stress, mallet, room, lab and simd. Without a section, all of them run. On Linux run it with a larger stack (`ulimit -s unlimited`).
- `build/okl_hosttest_artefacts/Release/okl_hosttest`: loads the built VST3 the way a DAW does and plays it, including tests with the plugin window open.
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
