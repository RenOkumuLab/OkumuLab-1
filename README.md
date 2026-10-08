
# OkumuLab 1

OkumuLab 1 is a highly experimental, innovative and avant-garde virtual organ VST3 plugin.

It is a physical model of an organ flue pipe (Prinzipal 8'), made as a VST3 instrument and a standalone app for Windows (x64) with JUCE 8. The screen uses WebView2 and Three.js and draws the jet, the standing wave inside the pipe and the vibration of the pipe wall exactly as the sound engine computes them.

- Version 1.0.0, vendor name OkumuLab
- Formats: VST3 and standalone (Windows x64)
<img width="1266" height="792" alt="スクリーンショット 2026-10-08 154201" src="https://github.com/user-attachments/assets/d39dea1d-3336-4553-88fe-ee4d444414aa" />

## Features

- In WIND mode the pipe sounds from the wind blown into it. In MALLET mode you strike the pipe body with a mallet.
- The keyboard covers the full MIDI range from 0 to 127, and each key plays a pipe with its own dimensions.
- There are 19 experimental recipes (DAW programs) and a modulation matrix with 4 slots.
- The reverb convolves the sound with an impulse response computed for the nave of a church.
- Every knob is a DAW parameter. Right-click a knob for MIDI learn, and double-click it to return to the default value. The default mapping matches the knobs, faders and pads of the Arturia MiniLab 3.

The details of the physical model, the MIDI mapping and the test results are in [docs/DEVELOPMENT_NOTES_ja.md](docs/DEVELOPMENT_NOTES_ja.md) (in Japanese).

## Requirements

- Windows (x64). Tested on Windows 11.
- The screen uses the WebView2 runtime, which comes with Windows 11. Without it, the plugin opens a simple native screen.
- On CPUs with AVX2 the engine uses AVX2 code, and on other CPUs it uses SSE2 code.
- The VST3 has been tested in Bitwig Studio and SAVIHost.

## Installation

Download the zip from Releases on GitHub and extract it.

- VST3: copy the `OkumuLab 1.vst3` folder to `C:\Program Files\Common Files\VST3\` (this needs administrator rights), then rescan the plugins in your DAW.
- Standalone: start `OkumuLab 1.exe` and choose the audio output and the MIDI input under Options at the top left.

The binaries are not code-signed, so Windows SmartScreen may show a warning the first time you open them. On Windows only one application at a time can open a MIDI device, so do not run the standalone app and a DAW at the same time.

## Building

You need Visual Studio 2022 (the Build Tools are enough) with the "Desktop development with C++" workload. CMake and Ninja come with Visual Studio.

```
build.bat          VST3, standalone and test programs (Release)
build.bat dsp      sound engine (DSP) and labium_check only (no JUCE)
```

- Output: `build\OkumuLab1_artefacts\Release\VST3\OkumuLab 1.vst3` and `build\OkumuLab1_artefacts\Release\Standalone\OkumuLab 1.exe`
- JUCE has long file names, so put the folder on a short path such as `C:\dev\OkumuLab1` before building.
- `third_party/JUCE` is JUCE 8.0.12 without changes (the bundled DemoRunner.exe and Projucer.exe are left out). `third_party/nuget` is the unpacked NuGet package of the WebView2 SDK 1.0.3485.44.
- The C runtime is linked statically, so users do not need the Visual C++ Redistributable.

### Tests

- `build-dsp\labium_check.exe [section]`: tests of the sound engine. The sections are port, derive, midi, pitchlock, wind, tone, blocks, cpu, stress, mallet, room, lab and simd. Without a section, all of them run.
- `build\okl_hosttest_artefacts\Release\okl_hosttest.exe`: loads the built VST3 the way a DAW does and plays it, including tests with the plugin window open.
- To validate the plugin with pluginval (Tracktion), get pluginval separately. It is not included in this repository.

### Environment variables for testing

These are not needed for normal use.

- `OKL_NATIVE_UI=1`: use the native screen instead of the WebView2 screen
- `OKL_MUTE=1`: no sound output (for screen tests)
- `OKL_PERFTEST=<file>`: run the screen test automatically and write the results to that file
- `OKL_RENDERTEST=<recorded notes>|<WAV>`: when the plugin is created, play the recorded notes and write the result to a WAV file

## License

Copyright (C) 2026 Ren Okumura

This program is free software. You can redistribute and modify it under the terms of the GNU Affero General Public License version 3 (AGPLv3). The full text is in [LICENSE](LICENSE). It comes with no warranty.

JUCE 8 is used under the AGPLv3. The licenses of the other libraries (including the VST3 SDK bundled with JUCE), Three.js and the fonts are in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and [licenses/](licenses/).

VST is a trademark of Steinberg Media Technologies GmbH.
