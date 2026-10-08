#!/usr/bin/env bash
# OkumuLab 1 - build on macOS and Linux (CMake 3.22 or later; Ninja if it is installed)
#   bash build.sh          configure + build everything (Release)
#   bash build.sh dsp      only the DSP core and labium_check (no JUCE)
# macOS: one binary for Apple silicon and Intel Macs (arm64 + x86_64), macOS 10.15 or later; Xcode's command line tools.
# Linux: the packages listed in README.md (ALSA, JACK, X11, FreeType, fontconfig, GTK and WebKitGTK headers).
# labium_check keeps whole engines on the stack: on Linux run it with a larger stack, e.g. (ulimit -s unlimited; ...).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"

GEN=()
if command -v ninja >/dev/null 2>&1; then GEN=(-G Ninja); fi
ARCH=()
if [ "$(uname)" = "Darwin" ]; then ARCH=(-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"); fi
JOBS="$( (command -v nproc >/dev/null 2>&1 && nproc) || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

if [ "${1:-}" = "dsp" ]; then
    cmake -S "$ROOT" -B "$ROOT/build-dsp" ${GEN[@]+"${GEN[@]}"} ${ARCH[@]+"${ARCH[@]}"} -DCMAKE_BUILD_TYPE=Release -DOKL_WITH_PLUGIN=OFF
    cmake --build "$ROOT/build-dsp" --parallel "$JOBS"
    exit 0
fi

cmake -S "$ROOT" -B "$ROOT/build" ${GEN[@]+"${GEN[@]}"} ${ARCH[@]+"${ARCH[@]}"} -DCMAKE_BUILD_TYPE=Release
cmake --build "$ROOT/build" --parallel "$JOBS"

A="$ROOT/build/OkumuLab1_artefacts/Release"
echo
echo "VST3:       $A/VST3/OkumuLab 1.vst3"
if [ -d "$A/AU" ]; then echo "AU:         $A/AU/OkumuLab 1.component"; fi
if [ "$(uname)" = "Darwin" ]; then echo "Standalone: $A/Standalone/OkumuLab 1.app"; else echo "Standalone: $A/Standalone/OkumuLab 1"; fi
